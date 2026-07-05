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
SKETCH_DIR = os.path.join(ROOT, "chavi_fi")
BIN_DIR = os.path.join(ROOT, "bin")
HEX = os.path.join(BIN_DIR, "chavi_fi.ino.hex")
GERAR_SEED = os.path.join(HERE, "gerar_seed.py")
CFG_PATH = os.path.join(HERE, ".bancada.json")

SEED_MAX_RANGE = 429496729
SEED_SECRET = os.getenv("SEED_SECRET", "CHAVI")
AVR_PROG = os.getenv("AVR_PROG", "usbasp")
BAUD_CABO = 2400
API_BASE_DEFAULT = "https://api-imoveis.chavi.com.br/v2/api"


# ---------------------------------------------------------------------------
# Seeds
# ---------------------------------------------------------------------------
def get_seed(serial, k):
    return int(sha256(f"{serial}{SEED_SECRET}{k}".encode()).hexdigest()[:8], 16) % SEED_MAX_RANGE


def seeds_de(serial):
    return [get_seed(serial, k) for k in range(1, 5)]


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
LIVE_LOG = os.path.join(HERE, "bancada-live.log")
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

        def cb(_c, data: bytearray):
            txt = data.decode("utf-8", errors="replace")
            if self._buf and not self._buf.endswith("\n") and not txt.startswith("\n"):
                self._buf += "\n"
            self._buf += txt
            for l in txt.splitlines():
                if l.strip():
                    LOG(f"  ⟵ {l.strip()}")

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
    if _fonte_mais_nova_que_hex():
        LOG("Firmware mudou (ou 1ª vez) — compilando antes de gravar...", "warn")
        rc, _ = _exec(["arduino-cli", "compile", "--profile", "chavi_fi",
                       "--build-path", BIN_DIR, SKETCH_DIR])
        if rc != 0 or not os.path.exists(HEX):
            LOG("Falha ao compilar.", "err"); STATUS("gravar", "fail"); return False
    seed_bin = _seed_bin(serial)
    rc, _ = _exec([sys.executable, GERAR_SEED, serial, seed_bin, _placa_de(mcu)])
    if rc != 0 or not os.path.exists(seed_bin):
        LOG("Falha ao gerar as seeds.", "err"); STATUS("gravar", "fail"); return False
    LOG("Gravando... NÃO mexa no cabo agora.", "hi")
    # Se a ASSINATURA do chip não bater com o MCU selecionado, tenta os outros
    # (m328pb=FI1.5, m328p/m328=FI1.0 — placas antigas misturam 328 e 328P).
    candidatos = [mcu] + [m for m in ("m328pb", "m328p", "m328") if m != mcu]
    for i, m in enumerate(candidatos):
        if i > 0:
            LOG(f"Assinatura não bateu — tentando chip {m}...", "warn")
            rc, _ = _exec([sys.executable, GERAR_SEED, serial, seed_bin, _placa_de(m)])
            if rc != 0:
                break
        rc, out = _exec(["avrdude", "-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                         "-U", "lfuse:w:0xE2:m", "-U", "hfuse:w:0xD7:m",
                         "-U", "efuse:w:0xF7:m", "-U", "lock:w:0xCF:m",
                         "-U", f"eeprom:w:{seed_bin}:r", "-U", f"flash:w:{HEX}:i"])
        if rc == 0:
            MCU_REAL[serial] = m
            LOG(f"✓ {serial} gravada (chip {m}, placa {_placa_de(m)}). 1 bipe = viva; "
                "aguarde a MELODIA (Rocky) = pronta. 4 bipes graves = módulo mudo.", "ok")
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
        rc, out = _exec(["avrdude", "-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
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


def act_conectar(serial, mcu):
    STATUS("conectar", "run")
    alvo = serial[2:] if serial.startswith("CH") else serial
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
</style></head>
<body>
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
        <select id="mcu"><option value="m328pb">m328pb (FI 1.5)</option>
          <option value="m328p">m328p (FI 1.0)</option>
          <option value="m328">m328 (FI 1.0 chip antigo)</option></select>
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
    <div id="steps"></div>
    <div class="comp" id="comp"></div>
    <button class="big green" style="margin-top:12px" onclick="finalizar()">✔ FINALIZAR e iniciar a próxima</button>
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
      <div class="row center"><button onclick="otpGerar()">Enviar código</button>
        <button class="grey" onclick="fecharModal()">Cancelar</button></div>
    </div>
    <div id="m-code" class="hide">
      <h3>Código do WhatsApp</h3>
      <p>Digite o código de 6 dígitos</p>
      <input id="in-otp" inputmode="numeric" maxlength="6" placeholder="000000">
      <div class="row center"><button class="green" onclick="otpVerificar()">Entrar</button>
        <button class="grey" onclick="fecharModal()">Cancelar</button></div>
    </div>
  </div>
</div>

<!-- modal de confirmação física (o operador VÊ e confirma) -->
<div class="mask-bg" id="confirm-bg">
  <div class="modal">
    <h3 id="cf-title">Confirmar</h3>
    <p id="cf-q" style="font-size:16px;color:var(--ink);font-weight:600"></p>
    <div class="row center" style="flex-direction:column;gap:10px">
      <button class="green big" id="cf-sim">✓ SIM, funcionou</button>
      <button class="big" id="cf-nao" style="background:var(--err)">✗ NÃO funcionou</button>
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
    b.textContent=t.label; b.onclick=()=>teste1(t); comp.appendChild(b); }
}

function setChip(step,state){
  const el=$("#chip-"+step); if(!el)return;
  el.className="chip "+(state==="pending"?"":state);
  el.textContent={pending:"○",run:"⏳",ok:"✓",fail:"✗"}[state]||"○";
}

async function runStep(step){
  if(busy)return; busy=true;
  const r=await fetch("/api/step",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({step,serial:SERIAL,mcu:MCU})}).then(r=>r.json()).catch(()=>({ok:false}));
  busy=false;
  if(r && r.need_login){ abrirLogin(); }
}
// pede o comando ao firmware e, se físico, PERGUNTA se funcionou de verdade.
async function teste1(t){ if(busy)return; busy=true;
  const r=await fetch("/api/test",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({serial:SERIAL,cmd:t.cmd})}).then(r=>r.json()).catch(()=>({ok:false}));
  if(r.ok && t.fisico){ await confirmarFisico(t); }
  busy=false;
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
async function autoteste(){ if(busy)return; busy=true;
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
  busy=false;
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
    for t in ("arduino-cli", "avrdude"):
        p = shutil.which(t)
        LOG(("✓ " if p else "✗ ") + f"{t}: {p or 'NÃO encontrado no PATH'}",
            "ok" if p else "err")


def main():
    if not os.path.exists(SKETCH_DIR):
        print(f"Sketch não encontrado em {SKETCH_DIR}", file=sys.stderr); sys.exit(1)
    porta = _porta_livre()
    srv = ThreadingHTTPServer(("127.0.0.1", porta), Handler)
    url = f"http://127.0.0.1:{porta}/"
    print(f">> Bancada Chavi FI em {url}")
    threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    threading.Timer(1.0, _checar_ferramentas).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n>> encerrando")
        CABO.fechar()


if __name__ == "__main__":
    main()
