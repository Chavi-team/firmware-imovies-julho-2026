#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bancada.py — Assistente de bancada das fechaduras Chavi FI (app web local).

Roda um servidor local e abre a interface no NAVEGADOR (renderiza bonito e
confiável — nada de Tkinter). Feito para um leigo montar 300 fechaduras:

  Tela 1 — digita o serial na máscara:  CH [xxx] FI [xxxxxx]
  Tela 2 — passos com ✓/✗, na ordem:
     1. Gravar firmware      (cabo USBasp / avrdude)
     2. Validar gravação     (relê o chip: serial + seeds)
     3. Conectar (BLE)        (scan pelo nome = serial sem "CH", PING→PONG)
     4. Auto-teste           (buzzer, LEDs, motor A/B, bateria, módulo BLE)
     5. Cadastrar no sistema  (backend admin/devices, só o serial)
     6. Finalizar             (próxima fechadura)

Rode por  ./tools/bancada.sh  (cuida do venv, do PATH e das dependências).
"""
import os
import sys
import re
import json
import time
import queue
import shutil
import socket
import threading
import subprocess
import webbrowser
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# EMPACOTADO (PyInstaller / Chavi-Fi-Imoveis-Setup): os recursos somente-
# leitura (hex pré-compilado, avrdude) vêm de dentro do bundle (_MEIPASS);
# os arquivos de trabalho (config, logs, seeds) ficam AO LADO do executável.
FROZEN = bool(getattr(sys, "frozen", False))
RES = getattr(sys, "_MEIPASS", HERE)
if FROZEN:
    _exe = os.path.abspath(sys.executable)
    if ".app/Contents/MacOS" in _exe:
        # dentro de um bundle .app (read-only): grava no Home do usuário
        WORK = os.path.expanduser("~/Chavi-FI-Bancada")
        os.makedirs(WORK, exist_ok=True)
    else:
        WORK = os.path.dirname(_exe)          # pasta descompactada (gravável)
else:
    WORK = HERE

SKETCH_DIR = os.path.join(ROOT, "chavi_fi")
BIN_DIR = os.path.join(WORK, "bancada-arquivos") if FROZEN else os.path.join(ROOT, "bin")
HEX = os.path.join(RES, "chavi_fi.ino.hex") if FROZEN else os.path.join(BIN_DIR, "chavi_fi.ino.hex")
CFG_PATH = os.path.join(WORK, ".bancada.json")


def _avrdude_cmd():
    """avrdude embutido no pacote (com o próprio .conf) ou o do PATH."""
    exe = "avrdude.exe" if os.name == "nt" else "avrdude"
    bundled = os.path.join(RES, "avrdude", exe)
    if os.path.exists(bundled):
        cmd = [bundled]
        conf = os.path.join(RES, "avrdude", "avrdude.conf")
        if os.path.exists(conf):
            cmd += ["-C", conf]
        return cmd
    return ["avrdude"]

SEED_MAX_RANGE = 429496729
SEED_SECRET = os.getenv("SEED_SECRET", "CHAVI")
AVR_PROG = os.getenv("AVR_PROG", "usbasp")
BAUD_CABO = 2400
API_BASE_DEFAULT = "https://api-imoveis.chavi.com.br/v2/api"

# ---------------------------------------------------------------------------
# Auto-aviso de atualização
# A bancada é empacotada (PyInstaller) e publicada nos GitHub Releases via tag
# "bancada-v*" (ver .github/workflows/build-bancada.yml). O app NÃO se auto-
# atualiza; aqui só CHECAMOS se há versão mais nova e mostramos um aviso.
BANCADA_VERSION = "1.0.0"                 # versão desta bancada (bump a cada release)
GITHUB_REPO = "Chavi-team/firmware-imovies-julho-2026"

# snapshot compartilhado (preenchido em background; lido pelo endpoint)
_UPDATE = {"checado": False, "current": BANCADA_VERSION,
           "latest": None, "outdated": False, "url": None}


def _parse_versao(tag):
    """'bancada-v2.7.3' / 'v2.7.3' / '2.7.3' -> (2,7,3); tolerante -> None."""
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", tag or "")
    return tuple(int(x) for x in m.groups()) if m else None


def _checar_atualizacao():
    """Consulta os Releases do GitHub e marca se há versão nova.
    TOTALMENTE tolerante a falha: sem internet / rate limit / timeout →
    não mostra nada, não loga erro, não trava (roda em thread daemon)."""
    try:
        import requests
        r = requests.get(
            f"https://api.github.com/repos/{GITHUB_REPO}/releases",
            headers={"Accept": "application/vnd.github+json"},
            params={"per_page": 30}, timeout=4)
        if not r.ok:
            return
        cand = None                      # (versao_tupla, release) mais nova bancada-v*
        for rel in (r.json() or []):
            if rel.get("draft") or rel.get("prerelease"):
                continue
            tag = rel.get("tag_name", "") or ""
            if not tag.startswith("bancada-v"):
                continue                 # ignora releases de firmware etc.
            v = _parse_versao(tag)
            if v and (cand is None or v > cand[0]):
                cand = (v, rel)
        if not cand:
            return
        v, rel = cand
        atual = _parse_versao(BANCADA_VERSION)
        _UPDATE.update({
            "latest": ".".join(str(x) for x in v),
            "outdated": bool(atual and v > atual),
            "url": rel.get("html_url"),
        })
    except Exception:
        pass                             # silêncio total: rede/timeout/JSON
    finally:
        _UPDATE["checado"] = True        # marca que a tentativa terminou


# ---------------------------------------------------------------------------
# Seeds
# ---------------------------------------------------------------------------
def get_seed(serial, k):
    return int(sha256(f"{serial}{SEED_SECRET}{k}".encode()).hexdigest()[:8], 16) % SEED_MAX_RANGE


def seeds_de(serial):
    return [get_seed(serial, k) for k in range(1, 5)]


# BEFC/AFTC a partir do pino do MOSFET (idêntico ao AT.py do dev): o gate do
# MOSFET (alimenta periféricos/MCU) fica ALTO antes e depois da conexão; o PIO6
# (wake) fica BAIXO antes e ALTO depois -> a borda que acorda o MCU.
#   pino MOSFET 8 -> BEFC020 / AFTC028  (90% das FIs). Campo configurável.
def calcular_hex_befc_aftc(mosfet_pin):
    try:
        m_pin = int(mosfet_pin)
        mosfet_bit = m_pin - 3          # PIO3 = bit0
        bits_befc = [0] * 12
        bits_aftc = [0] * 12
        if 0 <= mosfet_bit < 12:
            bits_befc[mosfet_bit] = 1
            bits_aftc[mosfet_bit] = 1
        bits_befc[3] = 0                # PIO6 (wake) BAIXO antes da conexão
        bits_aftc[3] = 1                # PIO6 ALTO depois -> borda de wake
        befc_hex = f"{int(''.join(map(str, bits_befc[::-1])), 2):03X}"
        aftc_hex = f"{int(''.join(map(str, bits_aftc[::-1])), 2):03X}"
        return befc_hex, aftc_hex
    except Exception:
        return "020", "028"             # fallback = pino 8


# Gera o seed.bin INTERNAMENTE (espelho do gerar_seed.py — no pacote não há
# interpretador Python para subprocess). Layout idêntico ao seedGenerator legado.
def gerar_seed_bin(serial, placa, caminho):
    eeprom = bytearray(1024)
    eeprom[1] = 0x01     # setupSeedOk
    eeprom[101] = 0x01   # warning sound
    eeprom[102] = 0x01   # light warning
    eeprom[104] = 0x01   # button
    eeprom[105] = 0x01   # auto close
    eeprom[150] = 0x01   # setupProductionOk
    s11 = serial[2:]
    eeprom[769:769 + len(s11)] = s11.encode()
    for i in range(4):
        a = 10 * i + 5
        eeprom[a:a + 4] = get_seed(serial, i + 1).to_bytes(4, "little")
    eeprom[900] = 0x01   # layout de telemetria
    eeprom[912] = 0x01 if placa == "fi10" else 0x00   # byte de PLACA
    with open(caminho, "wb") as f:
        f.write(eeprom)
    LOG(f"seed.bin gerado: {serial} placa={'FI 1.0' if placa == 'fi10' else 'FI 1.5'} "
        f"seeds={seeds_de(serial)}")


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
def load_cfg():
    try:
        with open(CFG_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def save_cfg(cfg):
    try:
        with open(CFG_PATH, "w") as f:
            json.dump(cfg, f, indent=2)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Broadcast de log/status para o navegador (via SSE)
# ---------------------------------------------------------------------------
class Bus:
    def __init__(self):
        self.subs = []
        self.lock = threading.Lock()

    def subscribe(self):
        q = queue.Queue()
        with self.lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)

    def publish(self, obj):
        with self.lock:
            for q in list(self.subs):
                q.put(obj)


BUS = Bus()

# Log server-side: TODA linha vai para tools/bancada-live.log (o assistente pode
# ler direto, sem depender de download). Recriado a cada boot do servidor.
LIVE_LOG = os.path.join(WORK, "bancada-live.log")
try:
    open(LIVE_LOG, "w").close()
except Exception:
    pass


def LOG(msg, tag=""):
    ts = time.strftime("%H:%M:%S")
    BUS.publish({"kind": "log", "msg": msg, "tag": tag, "ts": ts})
    try:
        with open(LIVE_LOG, "a") as f:
            f.write(f"[{ts}] {msg}\n")
    except Exception:
        pass


def STATUS(step, state):
    BUS.publish({"kind": "status", "step": step, "state": state})


# ---------------------------------------------------------------------------
# Cabo (USB-TTL / pyserial)
# ---------------------------------------------------------------------------
class Cabo:
    def __init__(self):
        self.ser = None

    @staticmethod
    def portas():
        import serial.tools.list_ports as lp
        return [(p.device, f"{p.device} — {p.description or ''}".strip())
                for p in lp.comports()]

    @staticmethod
    def porta_provavel():
        import serial.tools.list_ports as lp
        chaves = ("usbserial", "usbmodem", "cp210", "ch340", "ch910",
                  "ftdi", "pl2303", "wch", "slab")
        for p in lp.comports():
            hay = f"{p.device} {p.description} {p.manufacturer}".lower()
            if any(k in hay for k in chaves):
                return p.device
        return None

    def conectar(self, porta):
        import serial
        self.fechar()
        LOG(f"Abrindo {porta} a {BAUD_CABO} baud...")
        self.ser = serial.Serial(porta, BAUD_CABO, timeout=0.3)
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def fechar(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def conectado(self):
        return self.ser is not None and self.ser.is_open

    def _ler_ate(self, alvos, timeout):
        t0 = time.time()
        buf, linhas = "", []
        while time.time() - t0 < timeout:
            dado = self.ser.read(64)
            if dado:
                buf += dado.decode("utf-8", errors="replace")
                while "\n" in buf:
                    linha, buf = buf.split("\n", 1)
                    linha = linha.strip()
                    if linha:
                        linhas.append(linha)
                        LOG(f"  ⟵ {linha}")
                        if any(a in linha for a in alvos):
                            return True, linhas
        return False, linhas

    def cmd(self, texto, alvos, timeout=8.0):
        if not self.conectado():
            return False, ["(cabo não conectado)"]
        self.ser.reset_input_buffer()
        LOG(f"  ⟶ {texto}")
        self.ser.write((texto + "\n").encode())
        self.ser.flush()
        return self._ler_ate(alvos, timeout)

    def ping(self, tentativas=20):
        for i in range(tentativas):
            ok, _ = self.cmd("PING", ["PONG", "BANCADA-PRONTA"], timeout=1.2)
            if ok:
                return True
            if i == 1:
                LOG("Desligue e ligue a bateria da fechadura AGORA...", "warn")
        return False


# ---------------------------------------------------------------------------
# BLE (bleak) — transporte de teste SEM FIO (nesta placa os pads do UART são
# difíceis de achar; o BLE já é provado — é como o app abre a fechadura).
# Serviço FFE0 / característica FFE1. A fechadura anuncia com o nome = serial
# sem "CH" (ex.: 003FI002585) ou "CHAVIFI" de fábrica.
# ---------------------------------------------------------------------------
import asyncio

SVC_FFE0 = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"


class Ble:
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self._run, daemon=True).start()
        self.client = None
        self._buf = ""

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def _call(self, coro, timeout=90):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout=timeout)

    def conectado(self):
        return self.client is not None and self.client.is_connected

    # ---- scan ----
    async def _scan(self, alvo, timeout):
        from bleak import BleakScanner
        LOG(f"Procurando fechadura '{alvo}' por BLE ({timeout:.0f}s)...", "hi")
        achado = {}
        devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
        for addr, (dev, adv) in devs.items():
            nome = adv.local_name or dev.name or ""
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            ffe0 = any("ffe0" in u for u in uuids)
            match = (nome == alvo) or nome in ("CHAVIFI", "CHAVIFIPR") or ffe0
            if nome or ffe0:
                LOG(f"  {'★' if match else '·'} {nome or '(sem nome)'}  {dev.address}  rssi={adv.rssi}")
            if match:
                # prioridade: nome exato > CHAVIFI > só ffe0
                peso = 3 if nome == alvo else (2 if nome.startswith("CHAVIFI") else 1)
                if peso > achado.get("peso", 0):
                    achado = {"addr": dev.address, "nome": nome, "peso": peso}
        return achado.get("addr")

    def scan(self, alvo, timeout=8.0):
        return self._call(self._scan(alvo, timeout), timeout=timeout + 20)

    # ---- conectar ----
    async def _connect(self, addr):
        from bleak import BleakClient
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self.client = BleakClient(addr)
        await self.client.connect()
        self._buf = ""

        # DIAGNÓSTICO: lista os serviços/características e confirma que o FFE1
        # existe com a propriedade 'notify' (senão nunca chega resposta nenhuma).
        try:
            achou_ffe1 = False
            for svc in self.client.services:
                for ch in svc.characteristics:
                    if "ffe1" in ch.uuid.lower():
                        achou_ffe1 = True
                        LOG(f"  GATT: FFE1 props={','.join(ch.properties)}", "hi")
            if not achou_ffe1:
                LOG("  ⚠️ GATT: característica FFE1 NÃO encontrada!", "err")
        except Exception as e:
            LOG(f"  (dump GATT falhou: {e})")

        def cb(_c, data: bytearray):
            # Mostra o HEX CRU (o decode em '⟵' esconde bytes não-texto). Assim
            # dá p/ ver se o que volta é "PONG" (50 4F 4E 47) ou lixo/baud errado.
            hexs = " ".join(f"{b:02X}" for b in data)
            txt = data.decode("utf-8", errors="replace")
            leg = "".join(c if 32 <= ord(c) < 127 else "." for c in txt)
            LOG(f"  ⟵ [{hexs}]  \"{leg}\"")
            if self._buf and not self._buf.endswith("\n") and not txt.startswith("\n"):
                self._buf += "\n"
            self._buf += txt

        await self.client.start_notify(CHR_FFE1, cb)
        await asyncio.sleep(1.5)      # espera o MCU acordar após a conexão
        return True

    def connect(self, addr):
        return self._call(self._connect(addr), timeout=30)

    async def _disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self.client = None

    def disconnect(self):
        try:
            self._call(self._disconnect(), timeout=15)
        except Exception:
            pass

    # ---- comando ----
    async def _cmd(self, texto, alvos, timeout):
        self._buf = ""
        LOG(f"  ⟶ {texto}")
        await self.client.write_gatt_char(CHR_FFE1, texto.encode(), response=False)
        t0 = self.loop.time()
        while self.loop.time() - t0 < timeout:
            await asyncio.sleep(0.05)
            if any(a in self._buf for a in alvos):
                return True, self._buf
        return False, self._buf

    def cmd(self, texto, alvos, timeout=8.0):
        if not self.conectado():
            return False, "(BLE não conectado)"
        return self._call(self._cmd(texto, alvos, timeout), timeout=timeout + 15)

    # ---- provisionamento do módulo POR BLE (backup baud-agnóstico) ----
    # Scan AMPLO: acha o módulo virgem ("Soft AT"/"MLT-BT05"), de fábrica CHAVIFI,
    # já com o serial, ou por \d+FI\d+. Pega o de sinal MAIS FORTE (a fechadura na
    # bancada é a mais perto) e loga os candidatos.
    async def _scan_prov(self, alvo, timeout):
        from bleak import BleakScanner
        LOG(f"Procurando módulo p/ provisionar (serial {alvo} ou de fábrica, {timeout:.0f}s)...", "hi")
        devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
        melhor = None
        for addr, (dev, adv) in devs.items():
            nome = (adv.local_name or dev.name or "")
            up = nome.upper()
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            ffe0 = any("ffe0" in u for u in uuids)
            match = (nome == alvo or "SOFT AT" in up or "MLT-BT05" in up
                     or up.startswith("CHAVIFI") or re.search(r"\d+FI\d+", up) or ffe0)
            if nome or ffe0:
                LOG(f"  {'★' if match else '·'} {nome or '(sem nome)'}  {dev.address}  rssi={adv.rssi}")
            if match:
                peso = (10_000 if nome == alvo else 0) + adv.rssi   # serial exato ganha; senão RSSI
                if melhor is None or peso > melhor[0]:
                    melhor = (peso, dev.address, nome)
        if melhor:
            LOG(f"  → escolhido: {melhor[2] or '(sem nome)'}  {melhor[1]}")
            return melhor[1]
        return None

    def scan_prov(self, alvo, timeout=8.0):
        return self._call(self._scan_prov(alvo, timeout), timeout=timeout + 20)

    # Conecta, manda a sequência AT (uma por vez, lendo a resposta) e desconecta.
    # Baud-agnóstico: em MODE2 o módulo interpreta AT vindos do rádio (BLE).
    async def _provisionar_at(self, addr, comandos):
        from bleak import BleakClient
        if self.client and self.client.is_connected:
            await self.client.disconnect()
        self.client = BleakClient(addr)
        await self.client.connect()
        rx = {"txt": "", "ev": asyncio.Event()}

        def cb(_c, data: bytearray):
            r = data.decode("utf-8", errors="ignore").strip("\x00\r\n ")
            if r:
                rx["txt"] = r
                rx["ev"].set()

        await self.client.start_notify(CHR_FFE1, cb)
        await asyncio.sleep(1.2)
        ok_all = True
        for cmd in comandos:
            rx["txt"] = ""; rx["ev"].clear()
            await self.client.write_gatt_char(CHR_FFE1, cmd.encode("utf-8"), response=False)
            try:
                await asyncio.wait_for(rx["ev"].wait(), timeout=2.0)
                LOG(f"  📤 {cmd:<22} → 📥 {rx['txt']}")
            except asyncio.TimeoutError:
                LOG(f"  📤 {cmd:<22} → ⏳ (sem resposta)", "warn")
                if cmd != "AT+RESET":     # o RESET derruba a conexão: sem resposta é NORMAL
                    ok_all = False
            await asyncio.sleep(0.5)
        for fn in (lambda: self.client.stop_notify(CHR_FFE1), lambda: self.client.disconnect()):
            try:
                await fn()
            except Exception:
                pass
        self.client = None
        return ok_all

    def provisionar_at(self, addr, comandos):
        return self._call(self._provisionar_at(addr, comandos), timeout=60)


# ---------------------------------------------------------------------------
# Backend
# ---------------------------------------------------------------------------
class Backend:
    def __init__(self, cfg):
        self.cfg = cfg
        self.base = cfg.get("api_base", API_BASE_DEFAULT)
        self.token = cfg.get("token")

    def _req(self, metodo, rota, **kw):
        import requests
        kw.setdefault("timeout", 20)
        return requests.request(metodo, self.base.rstrip("/") + rota, **kw)

    def tem_token(self):
        return bool(self.token)

    def solicitar_otp(self, phone, country="55"):
        r = self._req("POST", "/otp/generate",
                      json={"phone": phone, "countryCode": country},
                      headers={"Accept": "application/json"})
        LOG(f"otp/generate → {r.status_code}")
        return r.ok

    def login_otp(self, phone, otp, country="55"):
        r = self._req("POST", "/loginotp",
                      json={"phone": phone, "countryCode": country, "otp": otp},
                      headers={"Accept": "application/json"})
        LOG(f"loginotp → {r.status_code}")
        if r.ok:
            self.token = r.json().get("token")
            self.cfg.update({"token": self.token, "phone": phone, "country": country})
            save_cfg(self.cfg)
            return True
        LOG(f"  falhou: {r.text[:160]}", "err")
        return False

    def cadastrar(self, serial):
        payload = {"serial_number": serial, "name": serial,
                   "version": "000", "device_type_id": 1}
        r = self._req("POST", "/admin/devices", json=payload,
                      headers={"Accept": "application/json",
                               "Authorization": f"Bearer {self.token}"})
        LOG(f"admin/devices → {r.status_code}: {r.text[:160]}")
        if r.status_code in (200, 201):
            return "ok"
        if r.status_code == 409:
            return "existe"
        if r.status_code in (401, 403):
            return "auth"
        return "erro"


# ===========================================================================
# Ações (rodam no handler; logam via BUS)
# ===========================================================================
CFG = load_cfg()
CABO = Cabo()
BLE = Ble()
BACKEND = Backend(CFG)


def _exec(cmd):
    LOG("$ " + " ".join(cmd), "hi")
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, bufsize=1)
    except FileNotFoundError as e:
        LOG(f"comando não encontrado: {e}", "err")
        return 1, ""
    out = []
    for ln in p.stdout:
        ln = ln.rstrip()
        if ln:
            out.append(ln)
            LOG("  " + ln)
    p.wait()
    return p.returncode, "\n".join(out)


def _seed_bin(serial):
    return os.path.join(BIN_DIR, f"seed_{serial}.bin")


def _fonte_mais_nova_que_hex():
    """True se algum fonte do sketch é mais novo que o .hex (precisa recompilar)."""
    if not os.path.exists(HEX):
        return True
    hx = os.path.getmtime(HEX)
    for nome in os.listdir(SKETCH_DIR):
        if nome.endswith((".ino", ".h", ".cpp", ".c")):
            if os.path.getmtime(os.path.join(SKETCH_DIR, nome)) > hx:
                return True
    return False


# placa (byte 912 do seed.bin) deriva do chip: 328PB = FI 1.5; 328/328P = FI 1.0
def _placa_de(mcu):
    return "fi15" if mcu == "m328pb" else "fi10"


# Chip que REALMENTE funcionou no Gravar (o retry de assinatura pode divergir
# do select da tela) — o Validar usa este, não o do select.
MCU_REAL = {}


def act_gravar(serial, mcu):
    STATUS("gravar", "run")
    os.makedirs(BIN_DIR, exist_ok=True)
    if FROZEN:
        # Pacote: usa o .hex EMBUTIDO (pré-compilado) — sem arduino-cli.
        if not os.path.exists(HEX):
            LOG("Pacote sem o firmware embutido (.hex) — pacote corrompido.", "err")
            STATUS("gravar", "fail"); return False
    elif _fonte_mais_nova_que_hex():
        LOG("Firmware mudou (ou 1ª vez) — compilando antes de gravar...", "warn")
        rc, _ = _exec(["arduino-cli", "compile", "--profile", "chavi_fi",
                       "--build-path", BIN_DIR, SKETCH_DIR])
        if rc != 0 or not os.path.exists(HEX):
            LOG("Falha ao compilar.", "err"); STATUS("gravar", "fail"); return False
    seed_bin = _seed_bin(serial)
    try:
        gerar_seed_bin(serial, _placa_de(mcu), seed_bin)
    except Exception as e:
        LOG(f"Falha ao gerar as seeds: {e}", "err"); STATUS("gravar", "fail"); return False
    LOG("Gravando... NÃO mexa no cabo agora.", "hi")
    # Se a ASSINATURA do chip não bater com o MCU selecionado, tenta os outros
    # (m328pb=FI1.5, m328p/m328=FI1.0 — placas antigas misturam 328 e 328P).
    candidatos = [mcu] + [m for m in ("m328pb", "m328p", "m328") if m != mcu]
    for i, m in enumerate(candidatos):
        if i > 0:
            LOG(f"Assinatura não bateu — tentando chip {m}...", "warn")
            try:
                gerar_seed_bin(serial, _placa_de(m), seed_bin)
            except Exception:
                break
        # CRISTAL EXTERNO 16MHz (a placa TEM — schema): OBRIGATÓRIO p/
        # SoftwareSerial a 9600 (baud de fábrica do módulo) ser confiável. O RC
        # de 8MHz não fala 9600 bem -> não convertia o módulo (ficava mudo).
        #   lfuse: 328PB=0xFF, 328/328P=0xF7 (16MHz, CKDIV8 off)
        #   efuse: 0xF7 (BOD; 2.7V p/ reintroduzir depois de estabilizar)
        #   hfuse: 0xD7 (EESAVE liga, sem bootloader) | lock: 0xCF
        lfuse = "0xFF" if m == "m328pb" else "0xF7"
        efuse = "0xF7"
        rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                        "-U", f"lfuse:w:{lfuse}:m", "-U", "hfuse:w:0xD7:m",
                        "-U", f"efuse:w:{efuse}:m", "-U", "lock:w:0xCF:m",
                        "-U", f"eeprom:w:{seed_bin}:r", "-U", f"flash:w:{HEX}:i"])
        if rc == 0:
            MCU_REAL[serial] = m
            LOG(f"✓ {serial} gravada (chip {m}, placa {_placa_de(m)}). 1 bipe = viva; "
                "aguarde a MELODIA (Rocky) = pronta. 4 bipes graves = módulo mudo.", "ok")
            global _GRAVA_TS
            _GRAVA_TS = time.time()   # marca p/ a etapa Conectar esperar o boot
            STATUS("gravar", "ok"); return True
        if "signature" not in out.lower():
            break                          # falha que não é de assinatura: não insiste
    LOG("✗ Gravação falhou. Verifique bateria DENTRO e contato firme do USBasp.", "err")
    STATUS("gravar", "fail"); return False


def act_validar(serial, mcu):
    STATUS("validar", "run")
    eep = os.path.join(BIN_DIR, "_verify_eeprom.bin")
    # usa o chip que o Gravar descobriu; se não houver, tenta os 3 (assinatura)
    preferido = MCU_REAL.get(serial, mcu)
    candidatos = [preferido] + [m for m in ("m328pb", "m328p", "m328") if m != preferido]
    rc = 1
    for i, m in enumerate(candidatos):
        if i > 0:
            LOG(f"Assinatura não bateu — relendo como {m}...", "warn")
        rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                        "-U", f"eeprom:r:{eep}:r"])
        if rc == 0:
            MCU_REAL[serial] = m
            break
        if "signature" not in out.lower():
            break
    if rc != 0 or not os.path.exists(eep):
        LOG("✗ Não consegui reler a fechadura.", "err"); STATUS("validar", "fail"); return False
    with open(eep, "rb") as f:
        data = f.read()
    ser_lido = bytes(data[769:780]).split(b"\x00")[0].decode(errors="replace")
    esperado = serial[2:]
    sd = seeds_de(serial)
    ok = ser_lido == esperado
    for i in range(4):
        a = 10 * i + 5
        v = int.from_bytes(data[a:a + 4], "little")
        bate = v == sd[i]; ok = ok and bate
        LOG(f"  {'✓' if bate else '✗'} seed{i+1}: chip={v} esperado={sd[i]}",
            "ok" if bate else "err")
    LOG(f"  {'✓' if ser_lido==esperado else '✗'} serial: chip={ser_lido!r} esperado={esperado!r}",
        "ok" if ser_lido == esperado else "err")
    if ok:
        LOG("✓ Gravação validada.", "ok"); STATUS("validar", "ok"); return True
    LOG("✗ Validação divergente. Regrave.", "err"); STATUS("validar", "fail"); return False


_GRAVA_TS = 0.0   # timestamp do fim da última gravação (p/ esperar o boot)
BOOT_ESPERA_S = 30   # boot + provisionamento + melodia levam ~24s; margem p/ 30s

def act_provisionar(serial, mcu, mosfet_pin):
    # BACKUP baud-agnóstico: provisiona o módulo MANDANDO A SEQUÊNCIA AT POR BLE
    # (método do AT.py do dev). Serve p/ o caso raro em que o auto-provisionamento
    # do firmware não fecha (módulo em estado exótico). Alvo = padrão NOVO: 9600
    # (AT+BAUD2) + PWRM0 (auto-sleep OFF; a 9600 não há wake-por-dado) + BEFC/AFTC
    # (gate do MOSFET + wake) + nome = serial.
    STATUS("provisionar", "run")
    alvo = serial[2:] if serial.startswith("CH") else serial
    befc, aftc = calcular_hex_befc_aftc(mosfet_pin)
    LOG(f"Provisionando módulo por BLE (pino MOSFET={mosfet_pin} → BEFC{befc}/AFTC{aftc})...", "hi")
    comandos = [
        "AT+SHIELD1",
        "AT+BAUD2",              # 9600 (padrão de fábrica = auto-cura no reset)
        "AT+PWRM0",              # auto-sleep OFF (sempre acordado)
        f"AT+BEFC{befc}",
        f"AT+AFTC{aftc}",
        f"AT+NAME{alvo}",
        "AT+RESET",              # aplica o baud/config novos
    ]
    BLE.disconnect(); time.sleep(1.0)
    try:
        addr = BLE.scan_prov(alvo, timeout=8.0)
    except Exception as e:
        LOG(f"✗ Erro no scan BLE: {e}", "err"); STATUS("provisionar", "fail"); return False
    if not addr:
        LOG("✗ Módulo não encontrado. Ele anuncia como 'Soft AT' (virgem) ou pelo serial. "
            "Religue a bateria e tente de novo.", "err")
        STATUS("provisionar", "fail"); return False
    try:
        ok = BLE.provisionar_at(addr, comandos)
    except Exception as e:
        LOG(f"✗ Erro ao provisionar: {e}", "err"); STATUS("provisionar", "fail"); return False
    if ok:
        LOG("✓ Módulo provisionado por BLE (9600 + PWRM0 + wake + nome). "
            "Religue a bateria (Rocky) e siga p/ o passo Conectar.", "ok")
        STATUS("provisionar", "ok"); return True
    LOG("⚠️ Provisionamento parcial (algum AT sem resposta). Pode repetir.", "warn")
    STATUS("provisionar", "fail"); return False


def act_conectar(serial, mcu):
    STATUS("conectar", "run")
    # ⛔ NÃO conectar no meio do boot: o provisionamento (que converge o baud do
    # módulo e configura MODE2/NOTI) leva ~24s e, se o app conecta no meio, o
    # módulo fica em modo túnel e a config não completa -> baud/estado errado
    # (visto: bytes 0xDE 0xFE = 9600 x 2400 descasado, melodia tocando durante o
    # PONG). Espera o boot terminar contando do fim da gravação.
    if _GRAVA_TS:
        falta = BOOT_ESPERA_S - (time.time() - _GRAVA_TS)
        if falta > 0:
            LOG(f"Aguardando o boot/provisionamento terminar ({falta:.0f}s)...", "hi")
            time.sleep(falta)
    alvo = serial[2:] if serial.startswith("CH") else serial
    # Desconecta a sessão anterior ANTES do scan: dispositivo conectado NÃO
    # anuncia — escanear ainda conectado dava "não encontrada" falso (visto na
    # 2584, cujo módulo não aceita AT+DROP e a conexão nunca caía sozinha).
    BLE.disconnect()
    time.sleep(1.0)
    try:
        addr = BLE.scan(alvo, timeout=8.0)
    except Exception as e:
        LOG(f"✗ Erro no scan BLE: {e}. (Dê permissão de Bluetooth ao Terminal em "
            "Ajustes ▸ Privacidade ▸ Bluetooth.)", "err")
        STATUS("conectar", "fail"); return False
    if not addr:
        LOG("✗ Fechadura não encontrada por BLE. Ela anuncia ao ACORDAR: aperte o "
            "botão físico ou religue a bateria e tente de novo.", "err")
        STATUS("conectar", "fail"); return False
    try:
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao conectar: {e}", "err"); STATUS("conectar", "fail"); return False
    # Baud é FIXO em 2400 (provisionado no 1º boot após gravar). Se o PING não
    # virar PONG: (a) a fechadura ainda estava terminando o provisionamento —
    # espere a MELODIA de pronta e reconecte; (b) o wake não subiu (conectou e
    # ela não bipou) — módulo/solda do pino de wake.
    LOG("Conectado. A fechadura deve dar 1 bipe curto (acordou por BLE).", "hi")
    for i in range(6):
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=3)
        if ok:
            LOG("✓ Fechadura conectada por BLE (PONG).", "ok")
            STATUS("conectar", "ok"); return True
    LOG("✗ Sem PONG. Se ela NÃO bipou ao conectar, o wake (PD3) não subiu — "
        "religue a bateria (espere a melodia) e tente de novo; persistindo, "
        "suspeite do módulo/solda.", "err")
    STATUS("conectar", "fail"); return False


def act_autoteste(serial, mcu):
    if not BLE.conectado():
        LOG("Conecte por BLE (passo 3) antes do auto-teste.", "warn"); return False
    STATUS("autoteste", "run")
    etapas = [("Ping", "TST-PING", ["PONG"], 3), ("Buzzer", "TST-BUZ", ["OK-BUZ"], 6),
              ("LEDs", "TST-LED", ["OK-LED"], 6), ("Motor →", "TST-MOT1", ["FIM-MOT1"], 6),
              ("Motor ←", "TST-MOT2", ["FIM-MOT2"], 6), ("Bateria", "TST-BAT", ["BAT:"], 4),
              ("Módulo BLE", "TST-INFO", ["FIM-INFO"], 5)]
    todos_ok = True; info = ""
    for nome, cmd, alvos, tmo in etapas:
        LOG(f"— {nome}", "hi")
        ok, buf = BLE.cmd(cmd, alvos, timeout=tmo)
        if cmd == "TST-INFO":
            info = buf
        LOG(f"  {'✓' if ok else '✗'} {nome} {'OK' if ok else 'sem confirmação'}",
            "ok" if ok else "err")
        todos_ok = todos_ok and ok
    # O módulo BLE está PROVADO: estamos falando com a fechadura por BLE agora.
    # O MOD:xx vem do auto-teste AT interno do firmware, que dá falso-negativo
    # em alguns clones (não respondem a um "AT" pelado). NÃO reprova o teste.
    mod = next((l for l in info.splitlines() if l.startswith("MOD:")), "")
    if "MOD:OK" in mod:
        LOG("  ✓ Módulo BLE (auto-teste AT interno OK)", "ok")
    elif mod:
        LOG(f"  ✓ Módulo BLE funcionando (a conexão BLE prova; auto-teste AT "
            f"interno = {mod}, irrelevante neste clone)", "ok")
    if todos_ok:
        LOG("✓✓ AUTO-TESTE PASSOU.", "ok"); STATUS("autoteste", "ok"); return True
    LOG("✗ Auto-teste com falha(s).", "err"); STATUS("autoteste", "fail"); return False


def act_teste1(serial, cmd):
    """Roda UM comando de teste e devolve {ok, resp}. A confirmação FÍSICA
    (o motor girou? apitou?) é feita no navegador e cruzada com este resultado."""
    if not BLE.conectado():
        LOG("Conecte por BLE (passo 3) antes de testar.", "warn")
        return {"ok": False, "resp": "", "erro": "sem BLE"}
    # Motor: aceita OK-MOT (motor JÁ começou a girar) e NÃO espera FIM-MOT — o
    # giro puxa corrente e pode dar brownout que derruba o BLE antes do FIM. Com
    # OK-MOT o teste fecha antes do pico; o operador confirma o giro na modal.
    alvo = {"TST-BUZ": ["OK-BUZ"], "TST-LED": ["OK-LED"], "TST-MOT1": ["OK-MOT1"],
            "TST-MOT2": ["OK-MOT2"], "TST-BAT": ["BAT:"],
            "TST-HIB": ["OK-HIB"], "TST-ROCKY": ["OK-ROCKY"]}.get(cmd, ["OK"])
    LOG(f"— {cmd}", "hi")
    ok, resp = BLE.cmd(cmd, alvo, timeout=8)
    LOG(f"  {'✓' if ok else '✗'} {cmd} firmware {'respondeu' if ok else 'não respondeu'}",
        "ok" if ok else "err")
    return {"ok": ok, "resp": resp.strip()}


def act_cadastrar(serial, mcu):
    STATUS("cadastrar", "run")
    if not BACKEND.tem_token():
        LOG("Faça o login (admin) para cadastrar — use o botão Entrar.", "warn")
        STATUS("cadastrar", "fail"); return {"need_login": True}
    r = BACKEND.cadastrar(serial)
    if r == "auth":
        BACKEND.token = None
        LOG("Sessão expirada — faça login de novo.", "warn")
        STATUS("cadastrar", "fail"); return {"need_login": True}
    if r in ("ok", "existe"):
        LOG(f"✓ {serial} " + ("cadastrada." if r == "ok" else "já estava cadastrada."), "ok")
        STATUS("cadastrar", "ok"); return True
    LOG("✗ Falha ao cadastrar.", "err"); STATUS("cadastrar", "fail"); return False


# Renomeia o MÓDULO pelo ar (AT remoto do MODE2): conecta no nome ERRADO que
# está no scan e manda AT+NAME<serial-sem-CH> + AT+RESET direto pro módulo —
# não passa pela UART MCU↔módulo (que é justamente o que está quebrado quando
# o nome fica preso/corrompido, ex. '803FI002485' com bit trocado).
def act_renomear(serial, nome_errado):
    alvo = serial[2:] if serial.startswith("CH") else serial
    nome_errado = nome_errado.strip()
    if not nome_errado or nome_errado == alvo:
        LOG("Informe o nome ERRADO exatamente como aparece no scan.", "err")
        return False
    LOG(f"Renomear módulo: '{nome_errado}' → '{alvo}' (AT remoto via BLE)", "hi")
    try:
        addr = BLE.scan(nome_errado, timeout=8.0)
    except Exception as e:
        LOG(f"✗ Erro no scan: {e}", "err"); return False
    if not addr:
        LOG(f"✗ '{nome_errado}' não está anunciando. Religue a bateria e tente de novo.", "err")
        return False
    try:
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao conectar: {e}", "err"); return False
    # Clones nem sempre respondem — manda 2x e segue (best-effort).
    for _ in range(2):
        BLE.cmd(f"AT+NAME{alvo}", ["OK", "NAME"], timeout=2)
        time.sleep(0.4)
    BLE.cmd("AT+RESET", ["OK"], timeout=2)
    BLE.disconnect()
    LOG("Comandos enviados. Aguardando o módulo reiniciar (4s)...", "hi")
    time.sleep(4)
    try:
        addr2 = BLE.scan(alvo, timeout=10.0)
    except Exception:
        addr2 = None
    if addr2:
        LOG(f"✓ Módulo renomeado: '{alvo}' está anunciando. Use o passo 3 normalmente.", "ok")
        return True
    LOG("Ainda não apareceu com o nome novo — RELIGUE A BATERIA e escaneie de "
        "novo (alguns módulos só aplicam o nome no próximo boot). Se voltar com "
        "o nome ERRADO, o módulo não aceita AT remoto → físico.", "warn")
    return False


# Diagnóstico + conserto do módulo PELO AR (AT remoto): pergunta VERS/BAUD/
# MODE/ROLE, força a UART do módulo p/ 2400 (AT+BAUD0 + RESET) e re-testa o
# PING. Salva o caso "módulo bom mas UART em baud errado" — que é indistinguível
# de solda quebrada até se perguntar AT+BAUD? pelo ar.
def act_consertar_modulo(serial):
    alvo = serial[2:] if serial.startswith("CH") else serial
    # canal CH001 = geração FI 1.0 (tem o MOSFET de energia no PIO do módulo)
    is10 = serial[2:5] == "001"
    LOG(f"🩺 Diagnóstico do módulo '{alvo}' pelo ar (placa {'1.0' if is10 else '1.5'})...", "hi")
    try:
        addr = BLE.scan(alvo, timeout=8.0)
        if not addr:
            LOG("✗ Não está anunciando. Religue a bateria.", "err"); return False
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao conectar: {e}", "err"); return False
    for q in ("AT+VERS?", "AT+BAUD?", "AT+MODE?", "AT+ROLE?", "AT+BEFC?", "AT+AFTC?"):
        BLE.cmd(q, ["ver", "OK", "+", "Get"], timeout=2)   # respostas vão pro log
        time.sleep(0.3)
    if is10:
        # RESGATE DA FI 1.0 PELO AR: religa o trilho de energia (gate do MOSFET
        # num PIO do módulo — pino 4/5/7/8/9). Grava na NVM: BEFCFF7/AFTCFFF =
        # todos os PIOs altos (menos PIO6, p/ o wake). Assim, ao religar a
        # bateria, o módulo já sobe o gate e o MCU liga sozinho. NÃO mexe no
        # baud (o lote 1.0 pode não ser 2400 — deixa como está).
        LOG("Religando o trilho de energia da 1.0 (PIOs do MOSFET) na NVM do módulo...", "hi")
        for c in ("AT+PIO41", "AT+PIO51", "AT+PIO71", "AT+PIO81", "AT+PIO91",
                  "AT+BEFCFF7", "AT+AFTCFFF"):
            BLE.cmd(c, ["OK", "+", "Set"], timeout=2)
            time.sleep(0.3)
        BLE.cmd("AT+RESET", ["OK", "+"], timeout=2)
        BLE.disconnect()
        LOG("✓ Config de energia gravada. AGORA: tire o USBasp, RELIGUE A BATERIA "
            "e veja se ela dá o bipe/melodia sozinha (= trilho religado). Depois "
            "reconecte no passo 3.", "ok")
        time.sleep(4)
        return True
    LOG("Forçando a UART do módulo para 2400 (AT+BAUD0) + reset...", "hi")
    BLE.cmd("AT+BAUD0", ["OK", "+"], timeout=2)
    time.sleep(0.4)
    BLE.cmd("AT+RESET", ["OK", "+"], timeout=2)
    BLE.disconnect()
    LOG("Aguardando o módulo reiniciar (4s) e re-testando o PING...", "hi")
    time.sleep(4)
    try:
        addr2 = BLE.scan(alvo, timeout=10.0)
        if not addr2:
            LOG("✗ Sumiu do scan após o reset — religue a bateria e rode o passo 3.", "warn")
            return False
        BLE.connect(addr2)
    except Exception as e:
        LOG(f"✗ Erro ao reconectar: {e}", "err"); return False
    for i in range(6):
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=3)
        if ok:
            LOG("✓✓ PONG! A UART do módulo estava em baud errado — PLACA RECUPERADA. "
                "Rode o auto-teste completo.", "ok")
            return True
    LOG("✗ Sem PONG mesmo com a UART forçada a 2400 e módulo comprovadamente bom "
        "→ solda/trilha TX-RX entre módulo e MCU = DEFEITO FÍSICO (laudo fechado).", "err")
    return False


def act_finalizar(serial):
    if BLE.conectado():
        BLE.disconnect()
    if CABO.conectado():
        CABO.fechar()
    LOG(f"Fechadura {serial} finalizada. Próxima!", "ok")
    return True


# ===========================================================================
# Página web (HTML/CSS/JS embutido)
# ===========================================================================
PAGE = r"""<!DOCTYPE html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Chavi FI — Bancada</title>
<style>
  :root{ --orange:#E86628; --ink:#0F172A; --muted:#64748B; --line:#E2E8F0;
         --ok:#16A34A; --err:#DC2626; --amber:#D97706; --bg:#F4F6F9; }
  *{box-sizing:border-box; font-family:-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
  body{margin:0; background:var(--bg); color:var(--ink)}
  .wrap{max-width:820px; margin:0 auto; padding:26px 20px 40px}
  h1{font-size:30px; margin:6px 0 2px; letter-spacing:-.5px}
  .sub{color:var(--muted); font-size:15px; margin-bottom:22px}
  .card{background:#fff; border:1px solid var(--line); border-radius:16px;
        box-shadow:0 1px 3px rgba(15,23,42,.06); padding:22px}
  /* tela serial */
  .mask{display:flex; align-items:center; justify-content:center; gap:10px; margin:14px 0 6px}
  .mask .pfx{font:700 34px ui-monospace,Menlo,monospace; color:var(--orange)}
  .mask input{font:700 34px ui-monospace,Menlo,monospace; text-align:center;
        border:2px solid var(--line); border-radius:12px; background:#F8FAFC;
        padding:8px 6px; color:var(--ink); outline:none; transition:.15s}
  .mask input:focus{border-color:var(--orange); background:#fff}
  #ggg{width:96px} #nnn{width:180px}
  .prev{text-align:center; font:700 18px ui-monospace,Menlo,monospace; margin-top:14px}
  .seeds{text-align:center; color:var(--muted); font:12px ui-monospace,Menlo,monospace; margin-top:4px}
  .row{display:flex; align-items:center; gap:12px}
  .center{justify-content:center}
  select{font-size:15px; padding:8px 10px; border-radius:10px; border:1px solid var(--line); background:#fff}
  /* botões */
  button{font-size:15px; font-weight:700; border:0; border-radius:12px; cursor:pointer;
         padding:12px 20px; color:#fff; background:var(--orange); transition:.15s}
  button:hover{filter:brightness(1.05)} button:active{transform:translateY(1px)}
  button:disabled{opacity:.45; cursor:not-allowed}
  .big{font-size:19px; padding:16px 26px; width:100%}
  .ghost{background:#EEF2F7; color:var(--ink)}
  .green{background:var(--ok)} .grey{background:#94A3B8}
  .small{padding:8px 12px; font-size:13px; border-radius:10px}
  /* passos */
  .head{display:flex; align-items:center; justify-content:space-between; margin-bottom:14px}
  .serial{font:800 26px ui-monospace,Menlo,monospace}
  .step{display:flex; align-items:center; gap:14px; padding:14px 16px; border:1px solid var(--line);
        border-radius:14px; background:#fff; margin-bottom:10px}
  .chip{width:34px; height:34px; border-radius:50%; display:flex; align-items:center;
        justify-content:center; font-size:18px; font-weight:800; background:#F1F5F9; color:var(--muted); flex:0 0 auto}
  .chip.run{background:#FEF3C7; color:var(--amber)} .chip.ok{background:#DCFCE7; color:var(--ok)}
  .chip.fail{background:#FEE2E2; color:var(--err)}
  .step .t{flex:1} .step .t b{font-size:16px} .step .t div{color:var(--muted); font-size:12.5px; margin-top:2px}
  .comp{display:flex; flex-wrap:wrap; gap:8px; margin:8px 0 4px}
  .comp button{background:#475569; padding:9px 12px; font-size:13px}
  /* log */
  .logbar{display:flex; justify-content:space-between; align-items:center; margin:18px 0 6px}
  .logbar b{color:var(--muted); font-size:13px; font-weight:700}
  #log{background:#0B1220; color:#E2E8F0; border-radius:12px; padding:12px 14px;
       font:12px ui-monospace,Menlo,monospace; height:240px; overflow:auto; white-space:pre-wrap; line-height:1.55}
  #log .ok{color:#4ADE80} #log .err{color:#F87171} #log .warn{color:#FBBF24} #log .hi{color:#FDBA74}
  /* modal */
  .mask-bg{position:fixed; inset:0; background:rgba(15,23,42,.45); display:none; align-items:center; justify-content:center}
  .modal{background:#fff; border-radius:16px; padding:24px; width:340px; text-align:center}
  .modal h3{margin:0 0 4px} .modal p{color:var(--muted); font-size:13px; margin:0 0 14px}
  .modal input{font:700 22px ui-monospace,Menlo,monospace; text-align:center; width:100%;
        border:2px solid var(--line); border-radius:10px; padding:10px; outline:none}
  .modal .row{margin-top:16px; gap:8px}
  .hide{display:none}
  .toplink{position:fixed; top:12px; right:16px; font-size:12px; color:var(--muted)}
  /* estado "processando" */
  @keyframes spin{to{transform:rotate(360deg)}}
  .spin{display:inline-block; width:14px; height:14px; margin-right:8px; vertical-align:-2px;
        border:2px solid rgba(255,255,255,.45); border-top-color:#fff; border-radius:50%;
        animation:spin .7s linear infinite}
  button.running{background:var(--amber)!important; box-shadow:0 0 0 3px rgba(217,119,6,.25)}
  body.processing button:not(.running):not(.keep){opacity:.4; cursor:not-allowed}
  #proc{position:fixed; top:10px; left:50%; transform:translateX(-50%); display:none;
        align-items:center; gap:2px; background:var(--amber); color:#fff; font-weight:800;
        font-size:13px; padding:8px 16px; border-radius:999px;
        box-shadow:0 4px 14px rgba(217,119,6,.4); z-index:50}
  body.processing #proc{display:flex}
  /* seção de recuperação */
  .flow-hint{color:var(--muted); font-size:13px; margin:-4px 0 14px}
  .recovery{margin-top:26px; border:1px dashed var(--amber); background:#FFFBEB;
        border-radius:14px; padding:16px 18px}
  .rec-title{font-weight:800; color:var(--amber); font-size:15px; margin-bottom:2px}
  .rec-sub{color:var(--muted); font-size:12.5px; margin-bottom:14px; line-height:1.45}
  .rec-item{margin-bottom:12px} .rec-item:last-child{margin-bottom:0}
  .rec-item button{background:#B45309}
  .rec-desc{color:var(--muted); font-size:12.5px; margin-top:5px; line-height:1.4}
  /* aviso de atualização disponível — banner fixo no topo, difícil de ignorar */
  #update-banner{position:fixed; top:0; left:0; right:0; z-index:100; display:none;
        align-items:center; justify-content:center; gap:18px; flex-wrap:wrap;
        background:linear-gradient(90deg,#DC2626,#E86628); color:#fff;
        padding:14px 20px; box-shadow:0 5px 20px rgba(220,38,38,.55)}
  #update-banner .ub-txt{font-size:15px; font-weight:800; letter-spacing:.2px}
  #update-banner .ub-txt b{font-size:17px}
  #update-banner button{background:#fff; color:#B91C1C; border:none; border-radius:10px;
        padding:12px 24px; font-weight:900; font-size:15px; cursor:pointer;
        box-shadow:0 2px 10px rgba(0,0,0,.25); text-transform:uppercase; letter-spacing:.4px}
  #update-banner button:hover{background:#FEF2F2}
  @keyframes ubpulse{0%,100%{box-shadow:0 5px 20px rgba(220,38,38,.5)}
        50%{box-shadow:0 5px 30px rgba(220,38,38,.9)}}
  #update-banner.on{display:flex; animation:ubpulse 1.5s ease-in-out infinite}
  body.has-update .wrap{padding-top:70px}
  #update-ok{position:fixed; top:12px; left:16px; font-size:12px; color:var(--ok);
        font-weight:700; opacity:.85}
</style></head>
<body>
<div id="update-banner">
  <span class="ub-txt">⚠️ Atualização disponível: <b id="ub-new">—</b>
    — você está na v<span id="ub-cur">—</span></span>
  <button id="ub-btn" class="keep">⬇ Baixar atualização</button>
</div>
<div id="update-ok"></div>
<div id="proc"><span class="spin"></span>Executando…</div>
<div class="toplink" id="login-state">não conectado</div>
<div class="wrap">

  <!-- TELA 1 -->
  <div id="tela-serial">
    <h1>Nova fechadura</h1>
    <div class="sub">Digite o número de série impresso na etiqueta</div>
    <div class="card">
      <div class="mask">
        <span class="pfx">CH</span>
        <input id="ggg" inputmode="numeric" maxlength="3" placeholder="003">
        <span class="pfx">FI</span>
        <input id="nnn" inputmode="numeric" maxlength="6" placeholder="002585">
      </div>
      <div class="prev" id="prev"></div>
      <div class="seeds" id="seeds"></div>
      <div class="row center" style="margin-top:16px">
        <label style="color:var(--muted);font-size:14px">Placa</label>
        <!-- só a GERAÇÃO da placa; o chip exato (328/328P/328PB) a gravação
             descobre sozinha pelo retry de assinatura -->
        <select id="mcu"><option value="m328pb">FI 1.5</option>
          <option value="m328p">FI 1.0</option></select>
      </div>
      <div class="row center" style="margin-top:12px">
        <label style="color:var(--muted);font-size:14px">Pino MOSFET</label>
        <input id="mosfet" inputmode="numeric" maxlength="2" value="8"
          style="width:60px;text-align:center"
          title="Pino do MOSFET do módulo BLE. 90% das FIs = 8. Só mude se a placa usar outro.">
      </div>
    </div>
    <button class="big" id="btn-next" style="margin-top:20px" disabled>PRÓXIMO ▶</button>
  </div>

  <!-- TELA 2 -->
  <div id="tela-passos" class="hide">
    <div class="head">
      <div class="serial" id="serial-lbl"></div>
      <button class="ghost small" onclick="voltar()">‹ Trocar</button>
    </div>
    <div class="flow-hint">Siga os passos na ordem, de cima para baixo ▼</div>
    <div id="steps"></div>
    <div class="comp" id="comp"></div>
    <button class="big green" style="margin-top:12px" onclick="finalizar()">✔ FINALIZAR e iniciar a próxima</button>

    <!-- RECUPERAÇÃO: só quando algo dá errado. No fluxo normal, ignore. -->
    <div class="recovery">
      <div class="rec-title">⚠️ Recuperação — só se algo der errado</div>
      <div class="rec-sub">No fluxo NORMAL você <b>não precisa</b> destes botões — basta seguir os passos numerados acima. Use um destes só quando o problema abaixo acontecer:</div>
      <div class="rec-item">
        <button id="btn-provisionar">⚙️ Provisionar módulo (BLE)</button>
        <div class="rec-desc">Use se a fechadura <b>NÃO conectar</b> ou der <b>4 bipes graves</b>. Reconfigura o rádio por Bluetooth.</div>
      </div>
      <div class="rec-item">
        <button id="btn-consertar">🩺 Consertar módulo</button>
        <div class="rec-desc">Use se a comunicação estiver <b>saindo com lixo/travando</b>.</div>
      </div>
      <div class="rec-item">
        <button id="btn-renomear">🔧 Renomear módulo</button>
        <div class="rec-desc">Use se no scan aparecer um <b>nome errado/antigo</b> em vez do serial.</div>
      </div>
    </div>
  </div>

  <div class="logbar"><b>REGISTRO</b><button class="ghost small" onclick="salvarLog()">salvar</button></div>
  <div id="log"></div>
</div>

<!-- modal login -->
<div class="mask-bg" id="modal-bg">
  <div class="modal">
    <div id="m-phone">
      <h3>Entrar (admin)</h3>
      <p>Telefone do admin, só dígitos com DDD</p>
      <input id="in-phone" inputmode="numeric" placeholder="41999999999">
      <div class="row center"><button class="keep" onclick="otpGerar()">Enviar código</button>
        <button class="grey keep" onclick="fecharModal()">Cancelar</button></div>
    </div>
    <div id="m-code" class="hide">
      <h3>Código do WhatsApp</h3>
      <p>Digite o código de 6 dígitos</p>
      <input id="in-otp" inputmode="numeric" maxlength="6" placeholder="000000">
      <div class="row center"><button class="green keep" onclick="otpVerificar()">Entrar</button>
        <button class="grey keep" onclick="fecharModal()">Cancelar</button></div>
    </div>
  </div>
</div>

<!-- modal de confirmação física (o operador VÊ e confirma) -->
<div class="mask-bg" id="confirm-bg">
  <div class="modal">
    <h3 id="cf-title">Confirmar</h3>
    <p id="cf-q" style="font-size:16px;color:var(--ink);font-weight:600"></p>
    <div class="row center" style="flex-direction:column;gap:10px">
      <button class="green big keep" id="cf-sim">✓ SIM, funcionou</button>
      <button class="big keep" id="cf-nao" style="background:var(--err)">✗ NÃO funcionou</button>
    </div>
  </div>
</div>

<script>
const PASSOS = [
  ["gravar","1 · Gravar firmware","Grava o programa e as seeds (cabo USBasp)."],
  ["validar","2 · Validar gravação","Relê o chip e confere serial + seeds."],
  ["conectar","3 · Conectar (BLE)","Acha a fechadura por Bluetooth e dá um PING."],
  ["autoteste","4 · Auto-teste","Testa cada peça e PERGUNTA se funcionou de verdade."],
  ["cadastrar","5 · Cadastrar no sistema","Registra só o serial no backend."],
];
// Testes com a PERGUNTA física (o firmware pode dizer OK e a peça não funcionar).
// Ordem: os leves primeiro; os MOTORES por ÚLTIMO (puxam corrente e podem dar
// brownout que derruba o BLE — assim o resto já passou). motor=true -> pausa
// antes p/ a bateria recuperar.
const TESTES = [
  {cmd:"TST-BUZ",  label:"Buzzer",  pergunta:"A fechadura APITOU?", fisico:true},
  {cmd:"TST-LED",  label:"LEDs",    pergunta:"Os LEDs ACENDERAM (vermelho, verde, azul, branco)?", fisico:true},
  {cmd:"TST-BAT",  label:"Bateria", pergunta:"", fisico:false},
  {cmd:"TST-MOT1", label:"Motor →", pergunta:"O motor GIROU para UM lado?", fisico:true, motor:true},
  {cmd:"TST-MOT2", label:"Motor ←", pergunta:"O motor GIROU para o OUTRO lado?", fisico:true, motor:true},
  // manuais: só no botão avulso, NUNCA no auto-teste
  {cmd:"TST-ROCKY", label:"♪ Rocky", pergunta:"", fisico:false, manual:true},
  {cmd:"TST-HIB",  label:"⚡ Hibernar", fisico:true, manual:true,
   pergunta:"Após o OK-HIB a conexão SEMPRE cai (é o DROP, esperado). O teste é o SOM: ficou em SILÊNCIO (= cortou; responda SIM) ou deu 3 bipes GRAVES (= não cortou; responda NÃO)? Se SIM, reconecte no passo 3: bipe de boot + PONG = ciclo cortar/religar PROVADO."},
];
let SERIAL="", MCU="m328pb", busy=false;

const $ = s=>document.querySelector(s);
const ggg=$("#ggg"), nnn=$("#nnn");

function serialAtual(){ const g=ggg.value.trim(), n=nnn.value.trim();
  return (g.length===3 && n.length===6) ? ("CH"+g+"FI"+n) : null; }

function onlyDigits(e){ e.target.value = e.target.value.replace(/\D/g,""); prev(); }
async function prev(){
  const s=serialAtual();
  if(s){ $("#prev").textContent="→  "+s; $("#prev").style.color="var(--ok)"; $("#btn-next").disabled=false;
    const r=await fetch("/api/seeds?serial="+s).then(r=>r.json());
    $("#seeds").textContent="seeds: "+r.seeds.join(" · ");
  } else { $("#prev").textContent="preencha 3 + 6 dígitos"; $("#prev").style.color="var(--err)";
    $("#seeds").textContent=""; $("#btn-next").disabled=true; }
}
ggg.addEventListener("input",onlyDigits); nnn.addEventListener("input",onlyDigits);
ggg.addEventListener("keydown",e=>{if(e.key==="Enter")nnn.focus()});
nnn.addEventListener("keydown",e=>{if(e.key==="Enter" && !$("#btn-next").disabled)irPassos()});
$("#btn-next").onclick=irPassos;

function irPassos(){
  SERIAL=serialAtual(); MCU=$("#mcu").value;
  $("#serial-lbl").textContent=SERIAL;
  $("#tela-serial").classList.add("hide"); $("#tela-passos").classList.remove("hide");
  renderSteps();
}
function voltar(){ $("#tela-passos").classList.add("hide"); $("#tela-serial").classList.remove("hide"); }

function renderSteps(){
  const c=$("#steps"); c.innerHTML="";
  for(const [k,t,d] of PASSOS){
    const el=document.createElement("div"); el.className="step";
    el.innerHTML=`<div class="chip" id="chip-${k}">○</div>
      <div class="t"><b>${t}</b><div>${d}</div></div>
      <button id="btn-${k}">Executar</button>`;
    c.appendChild(el);
    $("#btn-"+k).onclick = (k==="autoteste") ? autoteste : ()=>runStep(k);
  }
  const comp=$("#comp"); comp.innerHTML="";
  for(const t of TESTES){ const b=document.createElement("button");
    b.textContent=t.label; b.onclick=(e)=>teste1(t, e.currentTarget); comp.appendChild(b); }
  // RECUPERAÇÃO (botões estáticos na TELA 2) — só fiação, texto/ajuda vêm do HTML.
  // BACKUP: provisiona o módulo POR BLE. Só se o firmware não auto-provisionou.
  $("#btn-provisionar").onclick=(e)=>runStep("provisionar", e.currentTarget);
  // diagnostica e conserta a UART do módulo pelo ar (baud errado etc.)
  $("#btn-consertar").onclick=doConsertar;
  // renomeia o módulo pelo ar (nome residual/corrompido de gravação antiga)
  $("#btn-renomear").onclick=doRenomear;
}

function setChip(step,state){
  const el=$("#chip-"+step); if(!el)return;
  el.className="chip "+(state==="pending"?"":state);
  el.textContent={pending:"○",run:"⏳",ok:"✓",fail:"✗"}[state]||"○";
}

// Trava/destrava TODA a interface enquanto uma ação roda: desabilita os botões
// de fluxo (passos, testes, recuperação, finalizar/trocar), reduz a opacidade,
// e mostra "Executando…" no botão ativo + a barra flutuante #proc. Os botões
// dos modais (classe .keep) continuam clicáveis (ex.: "funcionou?" no auto-teste).
let ACTIVE_BTN=null;
function setBusy(on, btn){
  busy = on;
  ACTIVE_BTN = on ? (btn||null) : ACTIVE_BTN;
  document.body.classList.toggle("processing", on);
  document.querySelectorAll("#tela-passos button, #btn-next").forEach(b=>{
    if(b.classList.contains("keep")) return;
    b.disabled = on && (b!==btn);
  });
  if(on){
    if(btn){ btn.classList.add("running");
      if(btn.dataset.orig===undefined) btn.dataset.orig=btn.textContent;
      btn.innerHTML='<span class="spin"></span>Executando…'; }
  } else if(ACTIVE_BTN){
    ACTIVE_BTN.classList.remove("running");
    if(ACTIVE_BTN.dataset.orig!==undefined){ ACTIVE_BTN.textContent=ACTIVE_BTN.dataset.orig;
      delete ACTIVE_BTN.dataset.orig; }
    ACTIVE_BTN=null;
  }
}

async function runStep(step, btn){
  if(busy)return;
  btn = btn || $("#btn-"+step);
  setBusy(true, btn);
  const mosfet=($("#mosfet")&&$("#mosfet").value)||"8";
  const r=await fetch("/api/step",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({step,serial:SERIAL,mcu:MCU,mosfet})}).then(r=>r.json()).catch(()=>({ok:false}));
  setBusy(false);
  if(r && r.need_login){ abrirLogin(); }
}
// pede o comando ao firmware e, se físico, PERGUNTA se funcionou de verdade.
async function teste1(t, btn){
  if(busy)return;
  setBusy(true, btn);
  const r=await fetch("/api/test",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL,cmd:t.cmd})}).then(r=>r.json()).catch(()=>({ok:false}));
  if(r.ok && t.fisico){ await confirmarFisico(t); }
  setBusy(false);
}
// recuperação: renomear e consertar (envolvidos por setBusy p/ travar a UI)
async function doRenomear(e){
  if(busy)return;
  const errado=prompt("Nome ERRADO que aparece no scan (ex.: 803FI002485):");
  if(!errado)return;
  setBusy(true, e.currentTarget);
  await fetch("/api/renomear",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL,nomeErrado:errado.trim()})}).catch(()=>{});
  setBusy(false);
}
async function doConsertar(e){
  if(busy)return;
  setBusy(true, e.currentTarget);
  await fetch("/api/consertar",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL})}).catch(()=>{});
  setBusy(false);
}

const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function apiTest(cmd){
  return fetch("/api/test",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL,cmd})}).then(r=>r.json()).catch(()=>({ok:false}));
}
async function reconectar(){
  const r=await fetch("/api/step",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({step:"conectar",serial:SERIAL,mcu:MCU})}).then(r=>r.json()).catch(()=>({ok:false}));
  return r.ok;
}

// AUTO-TESTE: roda cada peça e CRUZA o OK do firmware com o que o operador vê.
// Motores por último, com pausa antes (bateria recupera). Se um teste não
// responder (brownout derrubou o BLE), tenta RECONECTAR 1x e refaz.
async function autoteste(){
  if(busy)return;
  setBusy(true, $("#btn-autoteste"));
  setChip("autoteste","run");
  let allok=true;
  for(const t of TESTES){
    if(t.manual) continue;                         // Hibernar/Rocky: só no botão avulso
    if(t.motor) await sleep(1500);                 // deixa a bateria recuperar
    let r=await apiTest(t.cmd);
    if(!r.ok){                                     // sem resposta -> reconecta e refaz
      if(await reconectar()){ await sleep(500); r=await apiTest(t.cmd); }
    }
    if(!r.ok){ allok=false; continue; }
    if(t.fisico){ const sim=await confirmarFisico(t); if(!sim) allok=false; }
  }
  setChip("autoteste", allok?"ok":"fail");
  setBusy(false);
}

// modal "funcionou?" -> registra no backend e resolve true/false
function confirmarFisico(t){
  return new Promise(resolve=>{
    $("#cf-title").textContent=t.label;
    $("#cf-q").textContent=t.pergunta || (t.label+" funcionou?");
    const bg=$("#confirm-bg"); bg.style.display="flex";
    const done=async(ok)=>{ bg.style.display="none";
      await fetch("/api/confirm",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({comp:t.label, ok})}).catch(()=>{});
      resolve(ok);
    };
    $("#cf-sim").onclick=()=>done(true);
    $("#cf-nao").onclick=()=>done(false);
  });
}

async function finalizar(){
  await fetch("/api/finalize",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL})});
  // pré-preenche o próximo número (+1)
  try{ const n=(parseInt(SERIAL.slice(-6))+1).toString().padStart(6,"0"); nnn.value=n; }catch(e){}
  voltar(); prev(); nnn.focus();
}

/* login */
function abrirLogin(){ $("#modal-bg").style.display="flex"; $("#m-phone").classList.remove("hide");
  $("#m-code").classList.add("hide"); $("#in-phone").focus(); }
function fecharModal(){ $("#modal-bg").style.display="none"; }
async function otpGerar(){ const phone=$("#in-phone").value.replace(/\D/g,"");
  if(phone.length<10)return; const r=await fetch("/api/login/generate",{method:"POST",
    headers:{"Content-Type":"application/json"},body:JSON.stringify({phone})}).then(r=>r.json());
  if(r.ok){ $("#m-phone").classList.add("hide"); $("#m-code").classList.remove("hide"); $("#in-otp").focus(); } }
async function otpVerificar(){ const otp=$("#in-otp").value.replace(/\D/g,"");
  const phone=$("#in-phone").value.replace(/\D/g,"");
  const r=await fetch("/api/login/verify",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({phone,otp})}).then(r=>r.json());
  if(r.ok){ fecharModal(); atualizaLoginState(true); runStep("cadastrar"); } }
function atualizaLoginState(on){ $("#login-state").textContent=on?"conectado ✓":"não conectado";
  $("#login-state").style.color=on?"var(--ok)":"var(--muted)"; }

/* botão Entrar sempre acessível: clique no canto */
$("#login-state").onclick=abrirLogin; $("#login-state").style.cursor="pointer";

/* log via SSE */
const logEl=$("#log");
function addLog(o){ const span=document.createElement("span"); span.className=o.tag||"";
  span.textContent="["+o.ts+"] "+o.msg+"\n"; logEl.appendChild(span); logEl.scrollTop=logEl.scrollHeight; }
function salvarLog(){ const blob=new Blob([logEl.textContent],{type:"text/plain"});
  const a=document.createElement("a"); a.href=URL.createObjectURL(blob);
  a.download="bancada-"+(SERIAL||"x")+".log"; a.click(); }
const ev=new EventSource("/events");
ev.onmessage=e=>{ const o=JSON.parse(e.data);
  if(o.kind==="log") addLog(o);
  else if(o.kind==="status") setChip(o.step,o.state);
  else if(o.kind==="login") atualizaLoginState(o.on); };

fetch("/api/state").then(r=>r.json()).then(s=>atualizaLoginState(s.logged));

/* aviso de atualização (não-bloqueante; falha de rede = silêncio) */
async function updateCheck(tent){
  let d;
  try{ d=await fetch("/api/update-check").then(r=>r.json()); }
  catch(e){ return; }                       // sem servidor/rede: ignora
  if(!d) return;
  if(!d.checado){                            // checagem ainda rodando: re-tenta
    if((tent||0)<5) setTimeout(()=>updateCheck((tent||0)+1), 2500);
    return;
  }
  if(d.outdated && d.latest){
    $("#ub-new").textContent = "v"+d.latest;
    $("#ub-cur").textContent = d.current;
    $("#update-banner").classList.add("on");
    document.body.classList.add("has-update");
  } else if(d.latest){
    $("#update-ok").textContent = "✓ atualizado (v"+d.current+")";
  }
}
$("#ub-btn").onclick=()=>{
  fetch("/api/open-update",{method:"POST"}).catch(()=>{});
};
updateCheck(0);

prev();
</script>
</body></html>"""


# ===========================================================================
# HTTP handler
# ===========================================================================
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        if n:
            return json.loads(self.rfile.read(n).decode())
        return {}

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            self._send(200, PAGE, "text/html; charset=utf-8"); return
        if self.path.startswith("/api/seeds"):
            m = re.search(r"serial=([A-Za-z0-9]+)", self.path)
            s = (m.group(1) if m else "").upper()
            self._send(200, {"serial": s, "seeds": seeds_de(s) if len(s) >= 5 else []}); return
        if self.path == "/api/state":
            self._send(200, {"logged": BACKEND.tem_token()}); return
        if self.path == "/api/update-check":
            self._send(200, dict(_UPDATE)); return
        if self.path == "/events":
            self._sse(); return
        self._send(404, {"erro": "nao encontrado"})

    def do_POST(self):
        try:
            b = self._body()
        except Exception:
            b = {}
        if self.path == "/api/step":
            self._send(200, self._step(b)); return
        if self.path == "/api/test":
            # síncrono: devolve o resultado REAL do firmware p/ o navegador
            # cruzar com a confirmação física do operador.
            self._send(200, act_teste1(b.get("serial", ""), b.get("cmd", ""))); return
        if self.path == "/api/confirm":
            # o operador respondeu "funcionou?/não" — só registra e ecoa o veredito.
            comp, ok = b.get("comp", "?"), bool(b.get("ok"))
            LOG(f"  {'✓' if ok else '✗'} {comp}: operador confirmou "
                f"{'FUNCIONOU' if ok else 'que NÃO funcionou (hardware)'}",
                "ok" if ok else "err")
            self._send(200, {"ok": True}); return
        if self.path == "/api/finalize":
            act_finalizar(b.get("serial", "")); self._send(200, {"ok": True}); return
        if self.path == "/api/renomear":
            ok = act_renomear(b.get("serial", ""), b.get("nomeErrado", ""))
            self._send(200, {"ok": bool(ok)}); return
        if self.path == "/api/consertar":
            ok = act_consertar_modulo(b.get("serial", ""))
            self._send(200, {"ok": bool(ok)}); return
        if self.path == "/api/open-update":
            # abre a página da release no navegador padrão do SISTEMA (mais
            # confiável que window.open dentro da janela nativa/webview).
            url = _UPDATE.get("url")
            if url:
                try:
                    webbrowser.open(url)
                except Exception:
                    pass
            self._send(200, {"ok": bool(url)}); return
        if self.path == "/api/login/generate":
            ok = BACKEND.solicitar_otp(b.get("phone", ""), CFG.get("country", "55"))
            self._send(200, {"ok": ok}); return
        if self.path == "/api/login/verify":
            ok = BACKEND.login_otp(b.get("phone", ""), b.get("otp", ""), CFG.get("country", "55"))
            if ok:
                BUS.publish({"kind": "login", "on": True})
            self._send(200, {"ok": ok}); return
        self._send(404, {"erro": "nao encontrado"})

    def _step(self, b):
        step, serial, mcu = b.get("step"), b.get("serial", ""), b.get("mcu", "m328pb")
        if step == "provisionar":
            r = act_provisionar(serial, mcu, b.get("mosfet", "8"))
            return {"ok": bool(r)}
        fn = {"gravar": act_gravar, "validar": act_validar, "conectar": act_conectar,
              "autoteste": act_autoteste, "cadastrar": act_cadastrar}.get(step)
        if not fn:
            return {"ok": False, "erro": "passo desconhecido"}
        r = fn(serial, mcu) if step != "cadastrar" else act_cadastrar(serial, mcu)
        if isinstance(r, dict):
            return {"ok": False, **r}
        return {"ok": bool(r)}

    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        q = BUS.subscribe()
        try:
            while True:
                try:
                    obj = q.get(timeout=15)
                    self.wfile.write(("data: " + json.dumps(obj) + "\n\n").encode())
                except queue.Empty:
                    self.wfile.write(b": ping\n\n")   # keep-alive
                self.wfile.flush()
        except Exception:
            pass
        finally:
            BUS.unsubscribe(q)


def _porta_livre(inicio=8765):
    for p in range(inicio, inicio + 20):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            if s.connect_ex(("127.0.0.1", p)) != 0:
                return p
    return inicio


def _checar_ferramentas():
    av = _avrdude_cmd()
    if os.path.sep in av[0]:
        LOG(f"✓ avrdude embutido no pacote: {av[0]}", "ok")
    else:
        p = shutil.which("avrdude")
        LOG(("✓ " if p else "✗ ") + f"avrdude: {p or 'NÃO encontrado no PATH'}",
            "ok" if p else "err")
    if FROZEN:
        LOG(f"✓ firmware embutido: {os.path.basename(HEX)}"
            f" ({os.path.getsize(HEX)} bytes)" if os.path.exists(HEX)
            else "✗ firmware embutido AUSENTE", "ok" if os.path.exists(HEX) else "err")
    else:
        p = shutil.which("arduino-cli")
        LOG(("✓ " if p else "✗ ") + f"arduino-cli: {p or 'NÃO encontrado no PATH'}",
            "ok" if p else "err")


def main():
    if not FROZEN and not os.path.exists(SKETCH_DIR):
        print(f"Sketch não encontrado em {SKETCH_DIR}", file=sys.stderr); sys.exit(1)
    porta = _porta_livre()
    srv = ThreadingHTTPServer(("127.0.0.1", porta), Handler)
    url = f"http://127.0.0.1:{porta}/"
    print(f">> Bancada Chavi FI em {url}")
    # Servidor numa thread daemon; a GUI (webview) precisa da MAIN thread no macOS.
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Timer(1.0, _checar_ferramentas).start()
    # checagem de atualização em background (não bloqueia a abertura da janela)
    threading.Thread(target=_checar_atualizacao, daemon=True).start()

    if os.environ.get("BANCADA_NO_BROWSER"):        # modo teste/headless
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        CABO.fechar()
        return

    # JANELA NATIVA PRÓPRIA (pywebview: WKWebView no Mac / WebView2 no Windows).
    # Bloqueia até fechar a janela. Fallback: navegador padrão.
    try:
        import webview
        webview.create_window("Chavi FI — Bancada", url, width=940, height=860,
                              min_size=(760, 640))
        webview.start()                             # bloqueia; retorna ao fechar
    except Exception as e:
        print(f">> Janela nativa indisponível ({e}); abrindo no navegador.")
        webbrowser.open(url)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
    CABO.fechar()


if __name__ == "__main__":
    main()
