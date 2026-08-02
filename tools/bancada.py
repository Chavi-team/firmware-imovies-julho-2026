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
BANCADA_VERSION = "2.14.1"                # versão desta bancada (bump a cada release)
# Versão do FIRMWARE que esta bancada grava (bake junto do .hex). Enviada no
# cadastro do device (devices.firmware_version). Bumpar junto do FW_VERSION do .ino.
FIRMWARE_VERSION = "2.14.1"
VERSION_DATE = "2026-08-02"               # data desta versão (ISO; bump a cada release)
VERSION_NOTES = "Bancada v2.13.0 (MOSFET automático): suporte às placas v2.7/retrofit 2024 com gate no pino FÍSICO 12 do módulo = PIO2/VCC-EEPROM (inendereçável por AT — descoberta 31/07 via esquemático+manuais) · pino MOSFET aceita 12 na UI/seed.bin · provisionamento pelo ar e firmware usam AT+PWRM1 nessas placas (módulo ocioso dorme -> PIO2 cai -> corta a placa; conexão BLE religa; ~0,65mA ocioso) · teste de hibernação novo por UPTIME (TST-INFO) — prova corte+religa sem comando de corte · firmware v2.13.0 (mosfetAuto: PWRM1, BEFC000/AFTC008, boot-de-wake por PD3 alto, MOSFET:12-AUTO e UPTIME no TST-INFO)"
GITHUB_REPO = "Chavi-team/firmware-imovies-julho-2026"
# O repo acima é PRIVADO → a API de releases dá 404 sem token. Então a checagem de
# atualização lê um BEACON PÚBLICO (repo Chavi-team/chavi-bancada-latest, latest.json)
# que o workflow de release atualiza a cada versão. Leitura sem login (raw).
BEACON_URL = "https://raw.githubusercontent.com/Chavi-team/chavi-bancada-latest/main/latest.json"

# ⚡ MODO DEV (opt-in explícito: BANCADA_DEV=1 no ambiente) — para iterar
# firmware rápido na mesa. Executa DE VERDADE só Gravar e Validar; os demais
# passos são PULADOS com aviso claro no log (nunca fingem aprovação) e o
# CADASTRO é BLOQUEADO (nenhum device de teste vaza pro backend). O header da
# UI mostra o badge. Produção = rodar SEM a variável (fluxo completo).
DEV_MODE = os.environ.get("BANCADA_DEV", "") == "1"

# snapshot compartilhado (preenchido em background; lido pelo endpoint)
_UPDATE = {"checado": False, "current": BANCADA_VERSION,
           "latest": None, "outdated": False, "url": None,
           "date": VERSION_DATE, "firmware": FIRMWARE_VERSION, "notes": VERSION_NOTES}


def _parse_versao(tag):
    """'bancada-v2.7.3' / 'v2.7.3' / '2.7.3' -> (2,7,3); tolerante -> None."""
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", tag or "")
    return tuple(int(x) for x in m.groups()) if m else None


def _corrompido_do_alvo(nome_up, alvo_up):
    """True se 'nome' é claramente o ALVO com o(s) último(s) char(es) GARBLED por
    UART instável do módulo (ex.: alvo '002FI001874' anunciando '002FI00187<').
    Regra SEGURA: mesmo tamanho e difere só em posições cujo char NÃO é [A-Z0-9]
    (lixo). Um serial DIFERENTE é todo alfanumérico → NÃO casa (não renomeia
    fechadura errada). Só cobre garble em char inválido (o observado)."""
    if not nome_up or len(nome_up) != len(alvo_up) or nome_up == alvo_up:
        return False
    difs = 0
    for a, b in zip(nome_up, alvo_up):
        if a != b:
            if a.isalnum():          # difere com char VÁLIDO -> outro serial
                return False
            difs += 1
    return 1 <= difs <= 2            # até 2 chars de lixo (padrão: 1-2 no fim)


def _checar_atualizacao():
    """Lê o BEACON PÚBLICO (latest.json) e marca se há versão nova.
    TOTALMENTE tolerante a falha: sem internet / timeout / JSON inválido →
    não mostra nada, não loga erro, não trava (roda em thread daemon).
    (O repo do firmware é privado, por isso NÃO dá p/ usar a API de releases
    sem token — o beacon público resolve sem embutir segredo no app.)"""
    try:
        import requests
        r = requests.get(BEACON_URL, timeout=5,
                         headers={"Cache-Control": "no-cache"})
        if not r.ok:
            return
        data = r.json() or {}
        latest = data.get("version")
        v = _parse_versao(latest)
        if not v:
            return
        atual = _parse_versao(BANCADA_VERSION)
        _UPDATE.update({
            "latest": latest,
            "outdated": bool(atual and v > atual),
            "url": data.get("url"),
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
        # ⭐ v2.13 MOSFET-AUTO (pino físico 12 = PIO2/VCC-EEPROM do módulo): o
        # gate NÃO cabe nas máscaras (BEFC/AFTC cobrem só PIO3..11) — quem
        # corta/religa é o auto-sleep (PWRM1). ⭐ v2.13.1 SEGURANÇA: as máscaras
        # seguram o PIO8 alto mesmo assim (inócuo no pino-12; salva uma GERAÇÃO 1
        # rotulada errada como 12 — o BEFC000 da v2.13.0 CORTAVA o trilho dessas
        # placas p/ sempre; caso real CH003FI002910/R0 em 02/08).
        if m_pin == 12:
            return "020", "028"
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
def gerar_seed_bin(serial, placa, caminho, mosfet="8"):
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
    # byte 914 = PIO do MOSFET (campo da UI; o firmware calcula BEFC/AFTC/
    # AT+PIOx0 em runtime a partir dele — 90% das FIs = 8)
    try:
        m = int(mosfet)
    except Exception:
        m = 8
    eeprom[914] = m if (4 <= m <= 9 or m == 12) else 8   # 12 = mosfet AUTO (v2.7)
    # ⭐ v2.14: a bancada PROVISIONA O MÓDULO PELO AR logo após gravar — o seed
    # dela já marca "módulo provisionado" (910=0xC9) p/ o firmware NUNCA rodar o
    # sweep pesado (os AT+RESET dele cortam a própria placa nos modelos com
    # mosfet — loop de suicídio do caso 2910). O gerar_seed.py de linha de
    # comando (gravação manual SEM bancada) continua deixando 0 = o firmware
    # se auto-provisiona como antes.
    eeprom[910] = 0xC9
    # 916 QUEIMADO (ex-variante sem MOSFET, removida na v2.11.1): fica 0 —
    # placas sem MOSFET usam a config NORMAL (o MCU delas é sempre alimentado).
    with open(caminho, "wb") as f:
        f.write(eeprom)
    LOG(f"seed.bin gerado: {serial} placa={'FI 1.0' if placa == 'fi10' else 'FI 1.5'} "
        f"mosfet=PIO{eeprom[914]} seeds={seeds_de(serial)}")


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
    # ⚡ Scan FATIADO com saída antecipada: o nome EXATO é decisivo sozinho
    # (ganha de qualquer outro candidato), então achou → retorna na hora, sem
    # queimar a janela inteira. O FALLBACK (CHAVIFI/ffe0 por sinal) continua
    # exigindo a janela COMPLETA — decisão idêntica à de antes.
    async def _scan_fatiado(self, alvo_up, timeout):
        # UMA sessão CONTÍNUA de scan com callback (nada de parar/religar o
        # rádio em fatias — isso se mostrou instável no macOS). O callback
        # acumula tudo que aparece e sinaliza no instante em que o nome EXATO
        # surge; aí seguramos +2s de CONFIRMAÇÃO (se houver uma SEGUNDA
        # fechadura com o MESMO nome — bug antigo de renome — ela aparece e o
        # desempate por RSSI mais forte fica IGUAL ao do scan de janela cheia).
        from bleak import BleakScanner
        acumulado = {}
        achou = asyncio.Event()

        def cb(dev, adv):
            acumulado[dev.address] = (dev, adv)
            if (adv.local_name or dev.name or "").upper() == alvo_up:
                achou.set()

        t0 = time.time()
        async with BleakScanner(detection_callback=cb):
            try:
                await asyncio.wait_for(achou.wait(), timeout=timeout)
                await asyncio.sleep(2.0)   # janela de confirmação (duplicatas)
            except asyncio.TimeoutError:
                pass                       # janela cheia sem o exato → fallback

        exatos = sorted(
            (adv.rssi, dev.address, adv.local_name or dev.name or "")
            for dev, adv in acumulado.values()
            if (adv.local_name or dev.name or "").upper() == alvo_up)
        if exatos:
            rssi, addr, nome = exatos[-1]   # RSSI mais forte ganha (como antes)
            LOG(f"  ★ {nome}  {addr}  rssi={rssi} (exato, em {time.time() - t0:.0f}s"
                + (f"; {len(exatos)} com o mesmo nome — mais forte ganha)"
                   if len(exatos) > 1 else ")"))
            return addr, acumulado
        return None, acumulado

    async def _scan(self, alvo, timeout):
        LOG(f"Procurando fechadura '{alvo}' por BLE (até {timeout:.0f}s)...", "hi")
        alvo_up = alvo.upper()
        exato_addr, devs = await self._scan_fatiado(alvo_up, timeout)
        if exato_addr:
            return exato_addr
        achado = {}
        for addr, (dev, adv) in devs.items():
            nome = adv.local_name or dev.name or ""
            up = nome.upper()
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            ffe0 = any("ffe0" in u for u in uuids)
            # ⛔ SEGURANÇA: um device cujo nome é OUTRO serial \d+FI\d+ (≠ alvo) é
            # OUTRA fechadura já gravada — NUNCA conectar/mexer nela (o ffe0 dela
            # não pode "puxar" a conexão). Evita testar/hibernar a fechadura errada.
            outro_serial = bool(re.search(r"\d+FI\d+", up)) and up != alvo_up
            match = (up == alvo_up) or up.startswith("CHAVIFI") or (ffe0 and not outro_serial)
            if outro_serial:
                match = False
            if nome or ffe0:
                tag = "★" if match else ("⛔" if outro_serial else "·")
                LOG(f"  {tag} {nome or '(sem nome)'}  {dev.address}  rssi={adv.rssi}")
            if match:
                # prioridade: nome exato > CHAVIFI > só ffe0 (mais forte desempata)
                peso = (3000 if up == alvo_up else (2000 if up.startswith("CHAVIFI") else 1000)) + adv.rssi
                if peso > achado.get("peso", -1e9):
                    achado = {"addr": dev.address, "nome": nome, "peso": peso}
        return achado.get("addr")

    def scan(self, alvo, timeout=8.0):
        return self._call(self._scan(alvo, timeout), timeout=timeout + 20)

    # ---- mapa de dispositivos "cara de fechadura" no ar (p/ a ADOÇÃO) ----
    # Devolve {addr: nome} de tudo que parece módulo de FI: nome de serial
    # (\d+FI\d+), virgem ("SOFT AT"/"MLT-BT05"/"CHAVIFI") ou serviço FFE0.
    async def _scan_mapa(self, timeout):
        from bleak import BleakScanner
        devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
        mapa = {}
        for addr, (dev, adv) in devs.items():
            nome = adv.local_name or dev.name or ""
            up = nome.upper()
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            ffe0 = any("ffe0" in u for u in uuids)
            if re.search(r"\d+FI\d+", up) or "SOFT AT" in up or "MLT-BT05" in up \
               or up.startswith("CHAVIFI") or ffe0 \
               or re.fullmatch(r"[0-9A-F]{12}", up):
                # 12 hex = clone virgem com o MAC como nome (ex.: 94DEB80D5852) —
                # sem isto a ADOÇÃO por ciclo de bateria era cega pra ele.
                mapa[dev.address] = nome or "(sem nome)"
        return mapa

    def scan_mapa(self, timeout=6.0):
        return self._call(self._scan_mapa(timeout), timeout=timeout + 20)

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
    # ⚠️ SÓ provisiona o ALVO exato (nome == serial) ou um módulo VIRGEM ("Soft AT"/
    # "MLT-BT05"/"CHAVIFI" de fábrica). NUNCA um device cujo nome já é OUTRO serial
    # \d+FI\d+ — isso RENOMEARIA a fechadura errada (bug real: numa bancada com
    # várias FIs, o "sinal mais forte" pegou a vizinha e a renomeou). Se só houver
    # outras fechaduras nomeadas por perto, ABORTA (não reconfigura ninguém).
    async def _scan_prov(self, alvo, timeout):
        LOG(f"Procurando módulo p/ provisionar (serial {alvo} ou VIRGEM, até {timeout:.0f}s)...", "hi")
        alvo_up = alvo.upper()
        # ⚡ mesmo scan fatiado: serial EXATO no ar → decide na hora (ganha de
        # virgem/garbled sempre); os fallbacks continuam vendo a janela cheia.
        exato_addr, devs = await self._scan_fatiado(alvo_up, timeout)
        if exato_addr:
            LOG(f"  → escolhido: {alvo}  {exato_addr}")
            return exato_addr
        melhor = None
        garbled = []   # candidatos = alvo com nome CORROMPIDO (garble no fim)
        for addr, (dev, adv) in devs.items():
            nome = (adv.local_name or dev.name or "")
            up = nome.upper()
            exato = (up == alvo_up)
            virgem = ("SOFT AT" in up or "MLT-BT05" in up or up.startswith("CHAVIFI"))
            # Clones que saem de fábrica com o MAC como nome (ex.: "94DEB80D5852",
            # exatamente 12 hex — visto na CH002FI001325 em 11/07). São virgens
            # TAMBÉM, mas com guarda-corpos p/ não capturar gadget alheio com
            # nome parecido: (a) só vale COLADO na bancada (rssi >= -65);
            # (b) perde de qualquer virgem clássico no desempate (peso menor);
            # (c) a receita pelo ar só "vinga" em quem responde OK+Set — num
            # device estranho ela falha sem renomear nada.
            mac_virgem = (not virgem and not exato
                          and bool(re.fullmatch(r"[0-9A-F]{12}", up))
                          and adv.rssi >= -65)
            virgem = virgem or mac_virgem
            corrompido = _corrompido_do_alvo(up, alvo_up)
            # OUTRA fechadura já gravada (serial \d+FI\d+ ≠ alvo e NÃO corrompido do alvo): PROIBIDO.
            outro_serial = bool(re.search(r"\d+FI\d+", up)) and not exato and not corrompido
            match = (exato or virgem) and not outro_serial
            if nome:
                tag = "★" if match else ("⚠" if corrompido else ("⛔" if outro_serial else "·"))
                LOG(f"  {tag} {nome}  {dev.address}  rssi={adv.rssi}")
            if corrompido:
                garbled.append((dev.address, nome, adv.rssi))
            if match:
                # serial exato ganha; virgem clássico > virgem-MAC; senão RSSI
                peso = (10_000 if exato else 0) + adv.rssi - (500 if mac_virgem else 0)
                if melhor is None or peso > melhor[0]:
                    melhor = (peso, dev.address, nome)
        if melhor:
            LOG(f"  → escolhido: {melhor[2] or '(sem nome)'}  {melhor[1]}")
            return melhor[1]
        # sem alvo exato/virgem: se há UM único nome CORROMPIDO do alvo, é ele (o
        # módulo garble o último byte do AT+NAME — UART instável). Reprovisionar por
        # BLE (caminho confiável do Mac) conserta o nome. Ambíguo (2+) → aborta.
        if len(garbled) == 1:
            addr, nome, _ = garbled[0]
            LOG(f"  ⚠ '{nome}' é o ALVO com o nome CORROMPIDO (garble de UART no módulo). "
                f"Vou reprovisionar por BLE e corrigir o nome para {alvo}.", "warn")
            return addr
        if len(garbled) >= 2:
            LOG("  ✗ Achei VÁRIOS nomes parecidos/corrompidos do alvo — ambíguo, NÃO vou "
                "arriscar renomear o errado. Deixe só a fechadura-alvo ligada e repita:", "err")
            for _a, _n, _r in garbled:
                LOG(f"     · {_n}  {_a}  rssi={_r}")
            return None
        LOG("  ✗ Não achei o alvo pelo serial nem um módulo VIRGEM (só outras "
            "fechaduras já nomeadas por perto). NÃO vou reconfigurar outra fechadura — "
            "religue a bateria da fechadura-alvo (ela deve anunciar 'SOFT AT' ou o "
            "próprio serial) e rode o passo de novo.", "err")
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
                   "version": "000", "device_type_id": 1,
                   "firmware_version": FIRMWARE_VERSION}   # grava em devices.firmware_version
        r = self._req("POST", "/admin/devices", json=payload,
                      headers={"Accept": "application/json",
                               "Authorization": f"Bearer {self.token}"})
        LOG(f"admin/devices → {r.status_code}: {r.text[:160]}")
        if r.status_code in (200, 201):
            return "ok"
        if r.status_code == 409:
            # Já cadastrada = REGRAVAÇÃO: atualiza o firmware_version do
            # registro existente (a rota aceita o SERIAL no lugar do id).
            r2 = self._req("PATCH", f"/admin/devices/{serial}/firmware",
                           json={"firmware_version": FIRMWARE_VERSION},
                           headers={"Accept": "application/json",
                                    "Authorization": f"Bearer {self.token}"})
            LOG(f"admin/devices/{serial}/firmware → {r2.status_code}: {r2.text[:120]}")
            if r2.status_code == 200:
                return "atualizado"
            LOG(f"⚠️ não consegui atualizar o firmware_version ({r2.status_code}) — "
                "cadastro segue válido com a versão antiga", "warn")
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
    except FileNotFoundError:
        LOG(f"comando não encontrado: '{cmd[0]}' — verifique se está instalado "
            f"e no PATH", "err")
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


def act_gravar(serial, mcu, mosfet="8"):
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
        gerar_seed_bin(serial, _placa_de(mcu), seed_bin, mosfet)
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
                gerar_seed_bin(serial, _placa_de(m), seed_bin, mosfet)
            except Exception:
                break
        # CRISTAL EXTERNO 16MHz (a placa TEM — schema): OBRIGATÓRIO p/
        # SoftwareSerial a 9600 (baud de fábrica do módulo) ser confiável. O RC
        # de 8MHz não fala 9600 bem -> não convertia o módulo (ficava mudo).
        #   lfuse: 328PB=0xFF (low-power crystal — o PB NÃO TEM full-swing;
        #          0xF7 num PB deixa o chip SEM CLOCK!), 328/328P=0xF7
        #          (full-swing, mais imune a ruído de motor)
        #   efuse: ⭐ BOD LIGADO em 2,7V (0xFD) nos DOIS chips. ⚠️ NUNCA 0xF4
        #          (4,3V): provado em bancada (07/07/2026, CH003FI002584) que o
        #          trilho real fica abaixo de 4,3V → MCU em RESET PERPÉTUO
        #          (grava por ISP normal, mas nunca boota — silêncio total na
        #          bateria). A frota legada roda até SEM BOD (efuse 0xF7 lido
        #          da FI de produção 002FI001767); 2,7V já protege as seeds.
        #   hfuse: 0xD7 (EESAVE liga, sem bootloader) | lock: 0xCF
        lfuse = "0xFF" if m == "m328pb" else "0xF7"
        efuse = "0xFD"
        # ⚡ GRAVAÇÃO EM 2 ESTÁGIOS (corta ~60-80s sem perder nada):
        #   Estágio 1 (-B 8, SCK lento): SÓ os fuses — obrigatório lento porque
        #   o chip pode vir de fábrica a 1MHz (CKDIV8) e o ISP exige SCK < clk/4.
        #   Estágio 2 (-B 1, SCK rápido): lock+eeprom+flash — o avrdude resetou
        #   o chip entre as invocações e o lfuse do estágio 1 já ativou o
        #   CRISTAL 16MHz → ISP aguenta SCK alto. O lock vai no estágio 2 (e não
        #   no 1) porque o chip-erase automático do write de flash APAGARIA um
        #   lock gravado antes — a ordem final (erase→lock→eeprom→flash) fica
        #   idêntica à da invocação única de hoje.
        #   Fallback: se o estágio rápido falhar (USBasp clone que ignora SCK),
        #   repete TUDO no -B 8 de hoje — nenhuma funcionalidade a menos.
        # Estágio 1 com RETRY de contato: USBasp recém-plugado às vezes falha o
        # 1º toque ("target does not answer") e funciona logo em seguida —
        # visto em produção 11/07 (2 de 7 exigiam um 2º clique). Re-tenta 1×
        # sozinho antes de devolver erro pro operador.
        for _contato in (1, 2):
            rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                            "-U", f"lfuse:w:{lfuse}:m", "-U", "hfuse:w:0xD7:m",
                            "-U", f"efuse:w:{efuse}:m"])
            if rc == 0 or "does not answer" not in out.lower():
                break
            LOG("Gravador não respondeu no 1º contato — tentando de novo em 2s "
                "(não mexa no cabo)...", "warn")
            time.sleep(2)
        if rc == 0:
            # ESTÁGIO 2a (-B 8): EEPROM no modo LENTO de propósito — o tempo de
            # escrita de EEPROM é do SILÍCIO (~3,4ms/byte): 10,75s no rápido ×
            # 10,99s no lento (medido 11/07). O rápido não ganha NADA e falhava
            # a verificação em parte dos chips (2 de 7) → lento aqui é grátis.
            # A EEPROM sobrevive ao chip-erase do estágio 2b via EESAVE (hfuse D7).
            rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                            "-U", f"eeprom:w:{seed_bin}:r"])
        if rc == 0:
            # ESTÁGIO 2b (-B 1): lock+flash no modo RÁPIDO (o flash é quem
            # ganha ~13s). O lock vem NESTE estágio porque o chip-erase
            # automático do write de flash reseta os lock bits — ele precisa
            # ser gravado depois do erase, como na invocação única original.
            rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "1",
                            "-U", "lock:w:0xCF:m", "-U", f"flash:w:{HEX}:i"])
            if rc != 0:
                LOG("Gravação rápida não pegou neste gravador — repetindo no modo "
                    "lento (compatível)...", "warn")
                rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "8",
                                "-U", "lock:w:0xCF:m", "-U", f"flash:w:{HEX}:i"])
        if rc == 0:
            MCU_REAL[serial] = m
            LOG(f"✓ {serial} gravada (chip {m}, placa {_placa_de(m)}). 1 bipe = viva; "
                "aguarde a MELODIA (Rocky) = pronta. 4 bipes graves = módulo mudo.", "ok")
            global _GRAVA_TS
            _GRAVA_TS = time.time()   # marca p/ a etapa Conectar esperar o boot
            STATUS("gravar", "ok"); return True
        if "signature" not in out.lower():
            break                          # falha que não é de assinatura: não insiste
    # Mensagem conforme a CAUSA (não adianta insistir clicando):
    low = out.lower()
    if "cannot find usb device" in low or "unable to open port" in low or "no usb" in low:
        if os.name == "nt":
            LOG("✗ O Windows NÃO enxerga o gravador USBasp. Causa mais comum: o "
                "DRIVER do USBasp não está instalado nesta máquina. ⇒ Instale o "
                "driver com o Zadig (zadig.akeo.ie): abra o Zadig, selecione o "
                "'USBasp' na lista e instale o 'libusbK' (ou WinUSB). É uma vez só "
                "por computador. Se já instalou, tire e reconecte o USBasp (sem hub). "
                "Clicar de novo NÃO resolve.", "err")
        else:
            LOG("✗ O GRAVADOR sumiu da USB (não é a fechadura). ⇒ TIRE o USBasp do "
                "computador, espere 5s e RECONECTE (direto, sem hub). Clicar de novo "
                "NÃO resolve — o USBasp travou e precisa ser religado.", "err")
    elif "does not answer" in low or "initialization failed" in low:
        LOG("✗ O chip não respondeu ao gravador. ⇒ Firme o cabo no conector ISP da "
            "placa e confirme a BATERIA dentro. Depois clique Gravar de novo.", "err")
    else:
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
        # ⚡ leitura no SCK rápido (-B 1): o gravar acabou de ativar o cristal
        # 16MHz. QUALQUER falha do modo rápido (inclusive assinatura — pode ser
        # o próprio SCK alto corrompendo a leitura) re-tenta no -B 8 de sempre
        # ANTES de concluir que o chip é outro.
        rc, out = _exec(_avrdude_cmd() + ["-P", "usb", "-c", AVR_PROG, "-p", m, "-b", "19200", "-B", "1",
                        "-U", f"eeprom:r:{eep}:r"])
        if rc != 0:
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
        LOG("✓ Gravação validada.", "ok")
        # Dica do CABO: só Gravar+Validar usam o USBasp; daqui em diante é tudo BLE.
        m = MCU_REAL.get(serial, mcu)
        if m == "m328pb":     # FI 1.5: roda pela própria bateria -> cabo já pode sair
            LOG("🔌 Pode REMOVER o cabo USBasp agora — os próximos passos "
                "(provisionar, conectar, hibernação, teste) são todos por Bluetooth.", "hi")
        else:                 # FI 1.0 (_400): o USBasp ALIMENTA o MCU até provisionar
            LOG("🔌 NÃO remova o cabo ainda — nesta placa (FI 1.0) o USBasp ALIMENTA a "
                "fechadura. Espere a MELODIA de pronta e só então remova o cabo.", "warn")
        STATUS("validar", "ok"); return True
    LOG("✗ Validação divergente. Regrave.", "err"); STATUS("validar", "fail"); return False


_GRAVA_TS = 0.0   # timestamp do fim da última gravação (p/ esperar o boot)
BOOT_ESPERA_S = 30   # boot + provisionamento + melodia levam ~24s; margem p/ 30s

# Receita AT v2.10.1 aplicada PELO AR (MODE2 = controle remoto, padrão de
# fábrica dos DOIS módulos — manuais 5.2 §7 / 1010 §7). Deterministica: a
# CONEXÃO BLE acorda o módulo mesmo com auto-sleep legado (PWRM1) — foi assim
# que a CH003FI002584 (surda p/ UART) foi recuperada em 07/07/2026.
#   PWRM0 PRIMEIRO: mata o auto-sleep herdado da esteira legada (PWRM1), que
#   deixava a UART do módulo DORMINDO — em 9600 nada a acorda pela serial.
#   Depois: baud 9600 + config de dados/wake + nome + reset.
def receita_ar(alvo, mosfet_pin):
    befc, aftc = calcular_hex_befc_aftc(mosfet_pin)
    # ⭐ v2.13 MOSFET-AUTO (pino 12): o PWRM final é 1 — auto-sleep LIGADO de
    # propósito (módulo ocioso dorme -> PIO2 cai -> mosfet corta a placa; a
    # conexão BLE acorda e religa). O PWRM0 INICIAL fica: acorda módulos com
    # herança de sleep p/ o resto da receita entrar; o PWRM definitivo vai por
    # último, imediatamente antes do RESET.
    # ⭐ v2.14: a BANCADA é a dona da config do módulo (decisão 02/08 — módulos
    # R0 são surdos p/ AT do MCU; pelo ar TODOS obedecem). Receita COMPLETA,
    # espelhando o que o firmware fazia por UART + a esteira legada:
    #   VERS?   = identificação família/rev no LOG (matriz r0/r2)
    #   SHIELD1 = 1º comando da esteira de produção legada
    #   MODE2   = controle remoto — é o que permite app/bancada mandarem AT
    #   IMME0   = anuncia sozinho ao ligar (IMME1 residual = some do ar)
    #   ADTY0   = anúncio conectável (ADTY errado = conserto pelo ar impossível)
    # NÃO mandar pelo ar: AT+PIO60 (derrubaria o PD3 no meio da sessão) e
    # AT+RENEW (proibido — apaga BEFC/AFTC = mata placa com mosfet).
    auto = str(mosfet_pin).strip() == "12"
    cmds = ["AT+PWRM0", "AT+VERS?", "AT+SHIELD1", "AT+BAUD2", "AT+MODE2",
            "AT+ROLE0", "AT+IMME0", "AT+ADTY0", "AT+TYPE0", "AT+DELI3",
            "AT+NOTI1", "AT+ADVI2", f"AT+BEFC{befc}", f"AT+AFTC{aftc}",
            f"AT+NAME{alvo}"]
    if auto:
        cmds.append("AT+PWRM1")
    cmds.append("AT+RESET")
    return cmds


# ADOÇÃO POR CICLO DE ENERGIA — identifica FISICAMENTE a fechadura da bancada
# quando o módulo dela anuncia um nome antigo/errado (fechadura REGRAVADA: o
# scan blindado recusa por segurança, e recusaria pra sempre). O operador
# desliga/religa a bateria e a bancada observa QUEM sumiu e voltou do ar —
# identificação mais forte que nome, sem risco de pegar a fechadura vizinha.
def act_adotar(alvo):
    LOG("🔎 ADOÇÃO por ciclo de energia — vou identificar a fechadura da bancada "
        "pelo desliga/religa da bateria (à prova de nome errado).", "hi")
    base = BLE.scan_mapa(6.0)
    if base:
        LOG("  No ar agora: " + " · ".join(f"{n}" for n in base.values()))
    LOG("👉 DESLIGUE a bateria da fechadura AGORA (aguardando ela sumir do ar...)", "warn")
    cand, faltando = None, {}
    t0 = time.time()
    while time.time() - t0 < 60:
        atual = BLE.scan_mapa(5.0)
        sumidos = {a: n for a, n in base.items() if a not in atual}
        # exige o MESMO addr sumido em 2 varreduras seguidas (scan BLE é flaky)
        confirmados = [a for a in sumidos if a in faltando]
        if len(confirmados) == 1:
            cand = confirmados[0]
            LOG(f"  ✓ Sumiu do ar: '{base[cand]}' ({cand}) — é a fechadura da bancada.", "ok")
            break
        faltando = sumidos
    if not cand:
        LOG("✗ Não vi nenhuma fechadura sumir do ar em 60s. Repita o passo e "
            "desligue a bateria quando eu pedir.", "err")
        return None
    LOG("👉 RELIGUE a bateria AGORA (aguardando ela voltar ao ar...)", "warn")
    t0 = time.time()
    while time.time() - t0 < 60:
        atual = BLE.scan_mapa(5.0)
        if cand in atual:
            LOG(f"  ✓ Voltou ao ar: '{atual[cand]}' — ADOTADA como {alvo}.", "ok")
            return cand
    LOG("✗ Ela não voltou ao ar em 60s. Religue a bateria e repita o passo.", "err")
    return None


def act_provisionar(serial, mcu, mosfet_pin):
    """PASSO 3 — garante o rádio BLE provisionado, entendendo os DOIS mundos:
    · VIRGEM de fábrica: o firmware se auto-provisiona no 1º boot; aqui só
      confirmamos por PONG (rápido) e reforçamos pelo ar se preciso.
    · REGRAVADA (módulo com nome antigo e/ou herança legada PWRM1 = UART
      dormindo): o scan não acha o serial → ADOÇÃO automática por ciclo de
      energia + receita v2.10.1 pelo ar (PWRM0 acorda, renomeia, reseta).
    Tudo sem cabo — a conexão BLE acorda o módulo mesmo dormindo."""
    global _GRAVA_TS
    STATUS("provisionar", "run")
    alvo = serial[2:] if serial.startswith("CH") else serial

    # espera o boot + auto-provisionamento do firmware (só logo após o gravar; se
    # rodado avulso, _GRAVA_TS é antigo e não espera)
    if _GRAVA_TS:
        falta = BOOT_ESPERA_S - (time.time() - _GRAVA_TS)
        if falta > 0:
            LOG(f"Aguardando o boot + auto-provisionamento do firmware ({falta:.0f}s)...", "hi")
            time.sleep(falta)

    def _pong():
        try:
            estava = BLE.conectado()
            BLE.disconnect()
            if estava:
                time.sleep(1.0)   # só espera anunciar de novo se havia conexão
            addr = BLE.scan(alvo, timeout=8.0)
            if not addr:
                return False
            BLE.connect(addr)
        except Exception:
            return False
        for _ in range(4):
            ok, _r = BLE.cmd("TST-PING", ["PONG"], timeout=3)
            if ok:
                return True
        return False

    def _reforcar(addr):
        global _GRAVA_TS
        try:
            ok = BLE.provisionar_at(addr, receita_ar(alvo, mosfet_pin))
        except Exception as e:
            LOG(f"✗ Erro ao provisionar pelo ar: {e}", "err"); return False
        if not ok:
            LOG("⚠️ Algum AT ficou sem resposta — repetindo uma vez...", "warn")
            time.sleep(3)
            try:
                ok = BLE.provisionar_at(addr, receita_ar(alvo, mosfet_pin))
            except Exception:
                ok = False
        # ⭐ CRÍTICO (aprendido na 002FI001767): depois do AT+RESET, o MÓDULO já
        # está 100% configurado (todos OK+Set acima) — mas o MCU faz o PRÓPRIO
        # provisionamento no boot, e enquanto NÓS estivermos conectados o módulo
        # fica em modo TÚNEL e não responde ao MCU → o MCU entra em loop (os
        # "AT+VERS?" que vazam pro túnel são ELE tentando) e nunca chega a
        # responder PONG. A cura é DESCONECTAR e deixar o MCU terminar SOZINHO.
        # Foi exatamente o "esperei e reconectei" que resolveu na mão.
        BLE.disconnect()
        for tentativa in (1, 2):
            LOG(f"Módulo configurado. Deixando o MCU terminar sozinho "
                f"(desconectado, ~30s — tentativa {tentativa}/2)...", "hi")
            time.sleep(30)   # DESCONECTADO de propósito: não competir com o MCU
            if _pong():
                LOG("✓ Rádio provisionado pelo ar (PWRM0 + 9600 + wake + nome) e "
                    "respondendo (PONG).", "ok")
                return True
        # 2 tentativas sem PONG: o rádio está OK (provado pelos OK+Set), o MCU não
        _GRAVA_TS = time.time()
        LOG("⚠️ Rádio configurado pelo ar, mas sem PONG do MCU após ~60s. "
            "DIAGNÓSTICO PELO SOM ao religar a bateria: SILÊNCIO = sem energia "
            "(bateria/fuses) · 1 bipe + 4 GRAVES = MCU vivo mas UART módulo→MCU "
            "marginal (solda) · MELODIA = pronto, é só conectar de novo.", "warn")
        return bool(ok)

    LOG("Verificando o rádio BLE (o firmware se auto-provisiona no boot)...", "hi")
    recem_gravada = bool(_GRAVA_TS) and (time.time() - _GRAVA_TS) < 300
    if _pong():
        if not recem_gravada:
            LOG("✓ Rádio provisionado e respondendo (PONG). Reforço por BLE dispensado.", "ok")
            STATUS("provisionar", "ok"); return True
        # ⭐ v2.14.1: placa RECÉM-GRAVADA responde PONG, mas a receita roda MESMO
        # ASSIM — a bancada é a dona da config do módulo (MODE2/IMME0/ADTY0/
        # VERS?/máscaras). Pular no PONG deixava drift antigo na NVM: na 2910 a
        # receita nova nunca chegou a rodar porque o PONG a dispensava.
        LOG("PONG ok — placa recém-gravada: aplicando a receita COMPLETA pelo ar "
            "mesmo assim (a bancada é a dona da config do módulo).", "hi")
        try:
            BLE.disconnect(); time.sleep(1.2)
            addr = BLE.scan(alvo, timeout=8.0)
        except Exception:
            addr = None
        if addr and _reforcar(addr):
            STATUS("provisionar", "ok"); return True
        LOG("⚠️ Não consegui aplicar a receita agora — a fechadura responde (PONG); "
            "sigo em frente. Rode o passo de novo p/ forçar a receita.", "warn")
        STATUS("provisionar", "ok"); return True

    # Sem PONG → acha o módulo: alvo exato, virgem ou garble (scan blindado)...
    LOG("Sem PONG — procurando o módulo (virgem, pelo serial ou nome corrompido)...", "hi")
    if BLE.conectado():
        BLE.disconnect(); time.sleep(1.0)
    try:
        addr = BLE.scan_prov(alvo, timeout=8.0)
    except Exception as e:
        LOG(f"✗ Erro no scan BLE: {e}", "err"); STATUS("provisionar", "fail"); return False
    # ...e se só houver fechadura com OUTRO nome (regravada!), ADOTA por ciclo
    # de energia — identificação física, sem risco de pegar a vizinha.
    if not addr:
        addr = act_adotar(alvo)
    if not addr:
        STATUS("provisionar", "fail"); return False
    ok = _reforcar(addr)
    STATUS("provisionar", "ok" if ok else "fail")
    return ok


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
    # ⚡ FAST-PATH: o provisionar acabou de deixar a fechadura CONECTADA e com
    # PONG — reusa a sessão em vez de derrubar + re-escanear (8s) + reconectar.
    # Se o PING falhar (sessão zumbi), segue o caminho completo de sempre.
    if BLE.conectado():
        for _ in range(2):
            ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=3)
            if ok:
                LOG("✓ Fechadura já conectada (sessão da preparação do rádio) — PONG ok.", "ok")
                STATUS("conectar", "ok"); return True
        LOG("Sessão anterior não responde — reconectando do zero...", "warn")
    # Desconecta a sessão anterior ANTES do scan: dispositivo conectado NÃO
    # anuncia — escanear ainda conectado dava "não encontrada" falso (visto na
    # 2584, cujo módulo não aceita AT+DROP e a conexão nunca caía sozinha).
    if BLE.conectado():
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
    # Baud é FIXO em 9600 (BAUD_MODULO; provisionado no 1º boot após gravar).
    # Se o PING não virar PONG: (a) a fechadura ainda estava terminando o
    # provisionamento — espere a MELODIA de pronta e reconecte; (b) módulo com
    # herança legada PWRM1 (UART dormindo) — o passo 1 conserta pelo ar.
    LOG("Conectado. A fechadura deve dar 1 bipe curto (acordou por BLE).", "hi")
    for i in range(6):
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=3)
        if ok:
            LOG("✓ Fechadura conectada por BLE (PONG).", "ok")
            STATUS("conectar", "ok"); return True
    LOG("✗ Sem PONG. Rode o passo 1 (ele acorda módulos dormindo pelo ar). "
        "DIAGNÓSTICO PELO SOM ao religar a bateria: SILÊNCIO = sem energia "
        "(bateria/BOD) · 1 bipe + 4 GRAVES = módulo surdo p/ o MCU (passo 1 "
        "resolve) · MELODIA = firmware ok, só reconectar.", "err")
    STATUS("conectar", "fail"); return False


def act_autoteste(serial, mcu):
    if not BLE.conectado():
        LOG("Faça o passo 1 (Gravar e preparar) antes do teste.", "warn"); return False
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
        LOG("Faça o passo 1 (Gravar e preparar) antes de testar.", "warn")
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
    if r in ("ok", "existe", "atualizado"):
        LOG(f"✓ {serial} " + {"ok": "cadastrada.",
                              "existe": "já estava cadastrada.",
                              "atualizado": f"já cadastrada — firmware atualizado p/ v{FIRMWARE_VERSION}."}[r], "ok")
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
        LOG(f"✓ Módulo renomeado: '{alvo}' está anunciando. Use o passo 1 normalmente.", "ok")
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
    for q in ("AT+VERS?", "AT+BAUD?", "AT+MODE?", "AT+ROLE?", "AT+PWRM?",
              "AT+NAME?", "AT+BEFC?", "AT+AFTC?"):
        BLE.cmd(q, ["ver", "OK", "+", "Get"], timeout=2)   # respostas vão pro log
        time.sleep(0.3)
    # ⭐ Leia o AT+PWRM? acima: "Get:1" = AUTO-SLEEP LIGADO (herança da esteira
    # legada) — é a causa nº1 de "módulo surdo": em 9600 a UART dele DORME e só
    # a conexão BLE acorda (caso CH003FI002584, 07/07/2026). O conserto abaixo
    # manda PWRM0 pelo ar de qualquer forma.
    if is10:
        # RESGATE DA FI 1.0 PELO AR: religa o trilho de energia (gate do MOSFET
        # num PIO do módulo — pino 4/5/7/8/9). Grava na NVM: BEFCFF7/AFTCFFF =
        # todos os PIOs altos (menos PIO6, p/ o wake). Assim, ao religar a
        # bateria, o módulo já sobe o gate e o MCU liga sozinho. NÃO mexe no
        # baud (o lote 1.0 pode não ser 9600 — deixa como está).
        LOG("Religando o trilho de energia da 1.0 (PIOs do MOSFET) na NVM do módulo...", "hi")
        for c in ("AT+PWRM0", "AT+PIO41", "AT+PIO51", "AT+PIO71", "AT+PIO81",
                  "AT+PIO91", "AT+BEFCFF7", "AT+AFTCFFF"):
            BLE.cmd(c, ["OK", "+", "Set"], timeout=2)
            time.sleep(0.3)
        BLE.cmd("AT+RESET", ["OK", "+"], timeout=2)
        BLE.disconnect()
        LOG("✓ Config de energia gravada. AGORA: tire o USBasp, RELIGUE A BATERIA "
            "e veja se ela dá o bipe/melodia sozinha (= trilho religado). Depois "
            "refaça o passo 1.", "ok")
        time.sleep(4)
        return True
    LOG("Aplicando o padrão v2.10.1 pelo ar: PWRM0 (acorda a UART) + 9600 + reset...", "hi")
    BLE.cmd("AT+PWRM0", ["OK", "+"], timeout=2)
    time.sleep(0.4)
    BLE.cmd("AT+BAUD2", ["OK", "+"], timeout=2)
    time.sleep(0.4)
    BLE.cmd("AT+RESET", ["OK", "+"], timeout=2)
    BLE.disconnect()
    LOG("Aguardando o módulo reiniciar (4s) e re-testando o PING...", "hi")
    time.sleep(4)
    try:
        addr2 = BLE.scan(alvo, timeout=10.0)
        if not addr2:
            LOG("✗ Sumiu do scan após o reset — religue a bateria e rode o passo 1.", "warn")
            return False
        BLE.connect(addr2)
    except Exception as e:
        LOG(f"✗ Erro ao reconectar: {e}", "err"); return False
    for i in range(6):
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=3)
        if ok:
            LOG("✓✓ PONG! O módulo estava dormindo (PWRM1 legado) ou em baud "
                "errado — PLACA RECUPERADA. Rode o auto-teste completo.", "ok")
            return True
    LOG("✗ Sem PONG mesmo com o módulo acordado (PWRM0) a 9600 e comprovadamente "
        "bom pelo ar → solda/trilha TX-RX entre módulo e MCU = DEFEITO FÍSICO "
        "(laudo fechado).", "err")
    return False


# ---------------------------------------------------------------------------
# HIBERNAÇÃO (economia de bateria) — teste do ciclo corta→religa PELO MOSFET.
# A hibernação corta a energia do MCU p/ poupar bateria; se o hardware não
# religar, a fechadura "morre" (só volta regravando pelo cabo USBasp). Por isso
# testamos o ciclo ANTES de ativar de vez: TST-HIB derruba o BLE e corta o
# MOSFET; se cortou, o MCU some do ar e depois RELIGA sozinho (Rocky). Re-scan +
# PONG = ciclo provado seguro nesta fechadura.
# ---------------------------------------------------------------------------
# ⭐ v2.13.5 — PLANO C: corte REMOTO pelo ar (p/ módulos surdos ao AT do MCU,
# caso 2910/R0). Em MODE2 o lado remoto controla os PIOs; a bancada manda
# AT+PIO<x>0 pela MESMA conexão de dados:
#   FASE 1 — corte comandado: AT+PIO<x>0 -> o MCU tem que CALAR (TST-PING mudo
#            com o módulo ainda conectado = prova em tempo real de corte);
#            desconecta/reconecta -> AFTC religa -> uptime voltou p/ trás.
#   FASE 2 — experimento NVM: AT+BEFC000 + desconectar -> se o módulo reaplica
#            o estado pré-conexão na desconexão, a placa corta SOZINHA sempre
#            que o app sai (arquitetura 100% hardware, nem o app comanda).
#            Restaura BEFC020 no fim (estado seguro conhecido).
# Se a FASE 1 passar, o corte em produção é comandável PELO APP (mesmo canal).
def _pong_paciente(alvo, tentativas=5):
    """PONG com paciência de BOOT FRIO: a placa recém-religada fica ~5-8s surda
    (bipe + config-leve + identificação antes do atenderApp) — um PING único se
    perde nesse meio (falso "não religou", caso real da 2910 em 02/08).
    Reconecta se a sessão cair entre as tentativas."""
    for _ in range(tentativas):
        if not BLE.conectado():
            try:
                addr = BLE.scan(alvo, timeout=8)
                if addr:
                    BLE.connect(addr)
            except Exception:
                pass
        if BLE.conectado():
            try:
                ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=4)
                if ok:
                    return True
            except Exception:
                pass
        time.sleep(3)
    return False


def _testar_corte_remoto(alvo, mosfet, u1):
    try:
        pio = int(str(mosfet).strip())
    except Exception:
        pio = 8
    if not 5 <= pio <= 11:
        pio = 8                         # MODE2 só controla PIO5..11 remotamente
    # ---- FASE 1: corte comandado pelo ar ----
    ok, _ = BLE.cmd(f"AT+PIO{pio}0", ["OK+Set"], timeout=5)
    if not ok:
        LOG(f"✗ O módulo não aceitou AT+PIO{pio}0 pelo ar (sem OK+Set) — nem o "
            "remoto controla os PIOs deste módulo. Sem caminho de corte.", "err")
        STATUS("hibernar", "fail"); return False
    ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=5)
    if ok:
        LOG(f"✗ O MCU AINDA RESPONDE após o corte remoto — o gate não é PIO{pio} "
            "(ou o módulo não executou de verdade). Religando por garantia.", "err")
        BLE.cmd(f"AT+PIO{pio}1", ["OK+Set"], timeout=5)
        STATUS("hibernar", "fail"); return False
    LOG(f"✓ CORTOU: o MCU calou após AT+PIO{pio}0 (módulo segue conectado, placa "
        "sem energia). Agora o religamento pela conexão (AFTC)...", "ok")
    try:
        BLE.disconnect()
    except Exception:
        pass
    time.sleep(3)
    addr = None
    for _ in range(3):
        try:
            addr = BLE.scan(alvo, timeout=10)
        except Exception:
            addr = None
        if addr:
            break
    if not addr:
        LOG("✗ Não reapareceu no scan após o corte — religue a bateria e me avise.", "err")
        STATUS("hibernar", "fail"); return False
    try:
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao reconectar: {e}", "err")
        STATUS("hibernar", "fail"); return False
    # v2.14.1: PONG com paciência de boot frio (o falso "AFTC não religou" da
    # 1ª rodada foi um PING único disparado ~2s após o GATT, no meio do boot).
    if not _pong_paciente(alvo):
        LOG("✗ Reconectou mas o MCU não voltou mesmo com paciência (~20s). "
            "Se o botão físico estiver mudo, o AFTC não religou — ciclo de "
            "bateria religa (BEFC020 no power-on). Regrave/reteste.", "err")
        STATUS("hibernar", "fail"); return False
    u2, _ = _uptime_via_info()
    if u2 is None or (u2 + 5) >= u1:
        LOG(f"✗ Inconclusivo: uptime {u1}s → {u2}s (esperava reboot).", "err")
        STATUS("hibernar", "fail"); return False
    LOG(f"✅ CORTE REMOTO PROVADO: uptime {u1}s → {u2}s (o MCU rebootou). O corte "
        f"pelo ar (AT+PIO{pio}0) funciona e a CONEXÃO religa — este é o caminho "
        "que o APP pode usar em produção nesta geração de placa.", "ok")
    # ---- FASE 2: corte automático na DESCONEXÃO (BEFC000 na NVM)? ----
    LOG("Experimento extra: BEFC000 pelo ar e desconecto — se o módulo reaplicar "
        "o estado pré-conexão ao desconectar, a placa corta SOZINHA quando o app "
        "sai (sem comando nenhum).", "hi")
    befc_corta = False
    if BLE.cmd("AT+BEFC000", ["OK+Set"], timeout=5)[0]:
        u_antes, _ = _uptime_via_info()
        try:
            BLE.disconnect()
        except Exception:
            pass
        time.sleep(15)
        addr = None
        try:
            addr = BLE.scan(alvo, timeout=10)
        except Exception:
            addr = None
        if addr:
            try:
                BLE.connect(addr)
                if _pong_paciente(alvo, tentativas=4):
                    u3, _ = _uptime_via_info()
                    if u3 is not None and u_antes is not None and (u3 + 5) < u_antes:
                        befc_corta = True
            except Exception:
                pass
    if befc_corta:
        LOG("✅✅ MELHOR CENÁRIO: com BEFC000 a placa cortou SOZINHA na desconexão "
            "e religou na conexão — hibernação 100% por hardware do módulo, sem "
            "app nem MCU comandando nada.", "ok")
    else:
        LOG("ℹ️ BEFC000 não cortou na desconexão (o módulo só aplica BEFC no "
            "power-on). O corte fica por comando do APP (fase 1, provado).", "hi")
    # estado seguro conhecido: BEFC020 de volta (power-on liga a placa)
    if BLE.conectado():
        BLE.cmd("AT+BEFC020", ["OK+Set"], timeout=5)
        LOG("(BEFC020 restaurado — power-on volta a ligar a placa)", "hi")
    else:
        LOG("⚠️ Não consegui restaurar o BEFC020 (sem conexão). A placa está com "
            "BEFC000: no próximo ciclo de bateria ela SÓ liga ao conectar. "
            "Reconecte e rode o teste de novo, ou me avise.", "warn")
    STATUS("hibernar", "ok")
    return {"ok": True, "ja_ativada": False, "modo": "remoto", "befc_corta": befc_corta}


def act_testar_hibernacao(serial, mcu, mosfet="8"):
    STATUS("hibernar", "run")
    alvo = serial[2:] if serial.startswith("CH") else serial
    # 1) garante conexão (o teste precisa mandar TST-HIB por BLE)
    if not BLE.conectado():
        try:
            addr = BLE.scan(alvo, timeout=8.0)
            if not addr:
                LOG("✗ Fechadura não encontrada por BLE — religue a bateria e tente de novo.", "err")
                STATUS("hibernar", "fail"); return False
            BLE.connect(addr)
        except Exception as e:
            LOG(f"✗ Erro ao conectar por BLE: {e}", "err")
            STATUS("hibernar", "fail"); return False
    # ⭐ v2.13.2: veredito por UPTIME (TST-INFO) — à prova de falso-positivo.
    # CASO REAL (2910/R0, 02/08): o OK-HIB chegava 1s ANTES do HIB-FALHOU-DROP,
    # a bancada retornava no OK-HIB, nunca via o FALHOU, reconectava, recebia
    # PONG de um MCU que NUNCA desligou e declarava "religou" — e ATIVAVA a
    # hibernação sem prova. Agora: uptime ANTES e DEPOIS; corte real = uptime
    # voltou p/ trás (o MCU rebootou). E módulo que tunela o AT+DROP do MCU
    # (comum nos antigos) ganha PLANO B: liga a hibernação e deixa o próprio
    # dormir() cortar DESCONECTADO (aí o módulo interpreta AT), mesmo veredito.
    u1, buf1 = _uptime_via_info()
    if u1 is None:
        LOG("✗ TST-INFO sem UPTIME — o firmware é anterior à 2.13. Regrave antes "
            "de testar a hibernação.", "err")
        STATUS("hibernar", "fail"); return False
    # módulo surdo p/ AT vindo do MCU (caso 2910/R0): o MCU não consegue
    # comandar DROP/PIO80 — nem o TST-HIB nem o plano B (dormir) funcionam.
    # ⭐ v2.13.5 PLANO C: o MODE2 permite que o lado REMOTO controle os PIOs
    # ("dispositivo remoto controla as saídas PIO5..11") — a BANCADA manda o
    # corte pelo ar, prova em tempo real (TST-PING tem que CALAR) e religa
    # pela conexão (AFTC). É o mesmo caminho que o APP usará em produção.
    sem_at = "MOD:SEM-AT" in (buf1 or "")
    if sem_at:
        LOG("⚠️ MOD:SEM-AT — módulo surdo p/ AT do MCU. Indo direto ao PLANO C: "
            "corte REMOTO pelo ar (o mesmo caminho que o app usará).", "warn")
    # ⚠️ millis() CONGELA no power-down: sem corte, u2 fica entre u1 e
    # u1+decorrido (relógio parado enquanto dorme); com corte real, u2 volta p/
    # TRÁS (~segundos desde o religamento). O veredito é "andou para trás", e
    # p/ ser inequívoco o uptime inicial precisa de gordura (>=40s).
    if u1 < 40:
        LOG(f"uptime inicial baixo ({u1}s) — deixo a fechadura acumular relógio "
            "por 40s antes do corte (veredito inequívoco).", "hi")
        time.sleep(40)
        u1, _ = _uptime_via_info()
        if u1 is None:
            LOG("✗ TST-INFO parou de responder na 2ª leitura.", "err")
            STATUS("hibernar", "fail"); return False
    t0 = time.time()
    if sem_at:
        return _testar_corte_remoto(alvo, mosfet, u1)
    LOG(f"🔋 Corte comandado (TST-HIB); uptime atual = {u1}s. O veredito sai pelo "
        "UPTIME após religar — silêncio no corte é esperado.", "hi")
    falhou_drop = False
    try:
        ok, buf = BLE.cmd("TST-HIB", ["HIB-FALHOU-DROP"], timeout=6)
        falhou_drop = bool(ok)          # só casa se o FALHOU chegou
        if not ok and "OK-HIB" not in (buf or ""):
            LOG("Sem OK-HIB — sigo (a conexão pode ter caído junto com o corte).", "warn")
    except Exception:
        pass                            # conexão caiu no meio = DROP funcionou
    if falhou_drop:
        LOG("⚠️ O módulo TUNELOU o AT+DROP (não derruba a conexão a pedido do MCU — "
            "comum nos módulos antigos). PLANO B: ativo a hibernação e deixo o "
            "firmware cortar SOZINHO no ocioso; veredito por UPTIME em ~90s.", "warn")
        ok, _ = BLE.cmd("TST-HIB-ON", ["OK-HIB-ON"], timeout=5)
        if not ok:
            LOG("✗ Não confirmou o TST-HIB-ON do plano B.", "err")
            STATUS("hibernar", "fail"); return False
        BLE.disconnect()
        time.sleep(90)                  # janela de teste (60s) + corte + folga
    else:
        time.sleep(6)                   # o corte vem ~1s após o OK-HIB
        try:
            BLE.disconnect()
        except Exception:
            pass
        time.sleep(2)
    # religa pela CONEXÃO e tira o veredito pelo UPTIME
    addr = None
    for _ in range(3):
        try:
            addr = BLE.scan(alvo, timeout=10)
        except Exception:
            addr = None
        if addr:
            break
    if not addr:
        LOG("⚠️ Não reapareceu no scan — se cortou e não religou, regrave pelo cabo.", "err")
        STATUS("hibernar", "fail"); return False
    try:
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao reconectar: {e}", "err")
        STATUS("hibernar", "fail"); return False
    ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=8)
    if not ok:
        time.sleep(3)                   # boot frio pode estar terminando
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=8)
    if not ok:
        LOG("⚠️ Reconectou mas SEM PONG — o MCU pode não ter religado. Regrave pelo cabo por segurança.", "err")
        STATUS("hibernar", "fail"); return False
    u2, _ = _uptime_via_info()
    elapsed = time.time() - t0
    # ⭐ v2.13.4: rebootou <=> o uptime ANDOU PARA TRÁS (u2 < u1). A fórmula
    # anterior comparava com o relógio de parede (u1+decorrido) e caía num
    # falso-positivo: no power-down o millis() do MCU CONGELA, então uma placa
    # que NÃO cortou (só dormiu) também fica "atrasada" vs o relógio — caso
    # real 2910: 123s→189s em 98s ("aprovada" sem nunca ter desligado). Com o
    # u1 mínimo de 40s garantido acima, u2<u1 só acontece com reboot real.
    if u2 is not None and (u2 + 5) < u1:
        LOG(f"✅ CORTOU E RELIGOU de verdade: uptime {u1}s → {u2}s em {int(elapsed)}s "
            "de teste (o uptime voltou p/ trás = o MCU rebootou). Ciclo "
            "corta→religa PROVADO.", "ok")
        STATUS("hibernar", "ok")
        return {"ok": True, "ja_ativada": falhou_drop}
    LOG(f"✗ NÃO CORTOU: uptime {u1}s → {u2}s em {int(elapsed)}s (o uptime nunca "
        "voltou p/ trás = o MCU nunca desligou; diferença p/ o relógio de parede "
        "é só o millis congelado no sono). Hibernação NÃO validada nesta placa.", "err")
    BLE.cmd("TST-HIB-OFF", ["OK-HIB-OFF"], timeout=5)   # deixa em estado seguro
    LOG("(hibernação desativada — modo IDLE seguro)", "warn")
    STATUS("hibernar", "fail"); return False


# ⭐ v2.13 — MOSFET-AUTO (pino físico 12 = PIO2/VCC-EEPROM do módulo): o corte
# NÃO é comandável (PIO2 fora do AT) — o módulo corta a placa SOZINHO ao entrar
# em auto-sleep (PWRM1) e religa ao acordar por CONEXÃO BLE. A prova do ciclo é
# o UPTIME do TST-INFO (firmware 2.13+): desconecta, espera o auto-sleep cortar,
# reconecta (a conexão religa) e lê o UPTIME — pequeno = o MCU REBOOTOU = cortou
# e religou (PASS); grande (~uptime1+espera) = nunca cortou (FAIL).
def _uptime_via_info():
    ok, buf = BLE.cmd("TST-INFO", ["FIM-INFO"], timeout=10)
    m = re.search(r"UPTIME:(\d+)", buf or "")
    return (int(m.group(1)) if m else None), buf


def act_testar_hibernacao_auto(serial, mcu, espera=180):
    STATUS("hibernar", "run")
    alvo = serial[2:] if serial.startswith("CH") else serial
    if not BLE.conectado():
        try:
            addr = BLE.scan(alvo, timeout=8.0)
            if not addr:
                LOG("✗ Fechadura não encontrada por BLE — religue a bateria e tente de novo.", "err")
                STATUS("hibernar", "fail"); return False
            BLE.connect(addr)
        except Exception as e:
            LOG(f"✗ Erro ao conectar por BLE: {e}", "err")
            STATUS("hibernar", "fail"); return False
    u1, _ = _uptime_via_info()
    if u1 is None:
        LOG("✗ TST-INFO sem UPTIME — o firmware desta fechadura é anterior à 2.13. "
            "Regrave com o firmware novo antes de testar a hibernação automática.", "err")
        STATUS("hibernar", "fail"); return False
    # v2.13.4: mesmo racional do teste G1 — millis() congela no power-down, o
    # veredito é "uptime voltou p/ trás"; garante gordura no u1 primeiro.
    if u1 < 40:
        LOG(f"uptime inicial baixo ({u1}s) — deixo acumular 40s de relógio antes "
            "do teste (veredito inequívoco).", "hi")
        time.sleep(40)
        u1, _ = _uptime_via_info()
        if u1 is None:
            LOG("✗ TST-INFO parou de responder na 2ª leitura.", "err")
            STATUS("hibernar", "fail"); return False
    LOG(f"🔋 MOSFET-AUTO: uptime atual = {u1}s. Vou DESCONECTAR e esperar {espera}s "
        "pelo auto-sleep do módulo (PWRM1) — ele deve CORTAR a placa sozinho.", "hi")
    LOG("👂 Durante a espera a fechadura deve ficar em silêncio e SEM luz; o anúncio "
        "BLE continua (é o módulo, que fica ligado). NÃO conecte nada nela.", "hi")
    BLE.disconnect()
    t0 = time.time()
    prox_log = espera - 30
    while time.time() - t0 < espera:
        rest = espera - (time.time() - t0)
        if rest <= prox_log:
            LOG(f"  … aguardando o auto-sleep ({int(rest)}s restantes)")
            prox_log -= 30
        time.sleep(1)
    # Reconecta — a PRÓPRIA conexão é o religamento (módulo acorda -> PIO2 sobe).
    addr = None
    for _ in range(3):
        try:
            addr = BLE.scan(alvo, timeout=10)
        except Exception:
            addr = None
        if addr:
            break
    if not addr:
        LOG("✗ Fechadura NÃO reapareceu no scan — o módulo deveria seguir anunciando "
            "mesmo dormindo. Módulo/energia suspeitos; regrave pelo cabo.", "err")
        STATUS("hibernar", "fail"); return False
    try:
        BLE.connect(addr)
    except Exception as e:
        LOG(f"✗ Erro ao reconectar: {e}", "err")
        STATUS("hibernar", "fail"); return False
    ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=10)
    if not ok:
        # dá uma 2ª chance: o MCU pode ainda estar terminando o boot frio
        time.sleep(3)
        ok, _ = BLE.cmd("TST-PING", ["PONG"], timeout=10)
    if not ok:
        LOG("✗ Reconectou mas SEM PONG — a placa pode não ter religado. Regrave pelo cabo.", "err")
        STATUS("hibernar", "fail"); return False
    u2, _ = _uptime_via_info()
    if u2 is None:
        LOG("⚠️ PONG ok mas TST-INFO sem UPTIME na volta — inconclusivo.", "warn")
        STATUS("hibernar", "fail"); return False
    # v2.13.4: reboot real <=> uptime voltou p/ trás (millis congela no sono —
    # comparação com relógio de parede dava falso-positivo, caso 2910).
    if (u2 + 5) < u1:
        LOG(f"✅ CORTOU E RELIGOU! uptime {u1}s → {u2}s após {espera}s de espera: o "
            "uptime voltou p/ trás = o MCU REBOOTOU (a placa ficou sem energia e "
            "religou na conexão). A hibernação automática (PWRM1) está ATIVA por "
            "hardware — nada a ativar.", "ok")
        STATUS("hibernar", "ok"); return True
    LOG(f"⚠️ NÃO CORTOU: uptime {u1}s → {u2}s (nunca voltou p/ trás). Ou o auto-sleep "
        f"demora mais que {espera}s (teste de novo com espera maior), ou o gate não "
        "está no pino 12 desta placa, ou o PWRM1 não pegou (reprovisione). A fechadura "
        "funciona 100% — só não está hibernando (consumo como hoje).", "warn")
    STATUS("hibernar", "fail"); return False


def act_ativar_hibernacao(serial, mcu):
    if not BLE.conectado():
        LOG("Faça o passo 1 (Gravar e preparar) antes de ativar a hibernação.", "warn"); return False
    ok, _ = BLE.cmd("TST-HIB-ON", ["OK-HIB-ON"], timeout=5)
    if ok:
        LOG("✅ Hibernação ATIVADA — vale no próximo boot. Religue a bateria.", "ok")
    else:
        LOG("✗ Não confirmou a ativação (sem OK-HIB-ON). Tente de novo.", "err")
    return ok


def act_desativar_hibernacao(serial, mcu):
    if not BLE.conectado():
        LOG("Faça o passo 1 (Gravar e preparar) antes de desativar a hibernação.", "warn"); return False
    ok, _ = BLE.cmd("TST-HIB-OFF", ["OK-HIB-OFF"], timeout=5)
    if ok:
        LOG("🔌 Hibernação desativada (modo seguro IDLE).", "ok")
    else:
        LOG("✗ Não confirmou a desativação (sem OK-HIB-OFF). Tente de novo.", "err")
    return ok


def act_hibernar_seguro(serial, mcu, mosfet="8"):
    """HIBERNAÇÃO (Avançado) — segura. VALIDA o ciclo (corta a energia do MCU pelo
    MOSFET e confere se RELIGA sozinha) e SÓ ATIVA a hibernação se o corte→religa
    passar; senão deixa em IDLE seguro (a fechadura funciona 100%, só não hiberna).
    NUNCA liga às cegas — ativar é irreversível-por-cabo se o hardware não religar,
    então o teste é o portão anti-brick. Requer o passo 1 (rádio conectado) feito.
    ⭐ v2.13 pino MOSFET = 12 (AUTO): valida por UPTIME (o corte é do auto-sleep
    PWRM1; não há comando nem 'ativar' — passa no teste = já está ativa)."""
    if str(mosfet).strip() == "12":
        ok = act_testar_hibernacao_auto(serial, mcu)
        if not ok:
            LOG("⚠️ Ciclo automático NÃO comprovado — a fechadura funciona 100%, só "
                "segue sem hibernar (consumo de hoje). Reteste com espera maior ou "
                "confira o pino do gate.", "warn")
            STATUS("hibernar", "ok")   # decisão segura tomada
        return {"hibernacao": bool(ok)}
    r = act_testar_hibernacao(serial, mcu, mosfet)
    if not r:
        LOG("⚠️ Hibernação NÃO validada nesta fechadura — mantida DESLIGADA (IDLE "
            "seguro). A fechadura está pronta e funciona 100%; apenas não hiberna.", "warn")
        STATUS("hibernar", "ok")   # o passo cumpriu seu papel: decisão segura tomada
        return {"hibernacao": False}
    # modo REMOTO (plano C): o MCU não comanda o corte — ativar EE_HIBERNA seria
    # inútil (dormir() mandaria AT p/ um módulo surdo). O corte é pelo AR (app).
    if isinstance(r, dict) and r.get("modo") == "remoto":
        LOG("📌 Corte validado no modo REMOTO: em produção, quem corta é o APP "
            "(AT+PIO<x>0 pelo ar ao final do uso)" +
            (" — ou o próprio módulo na desconexão (BEFC000), que também passou."
             if r.get("befc_corta") else "."), "ok")
        STATUS("hibernar", "ok")
        return {"hibernacao": True, "modo": "remoto", "befc_corta": bool(r.get("befc_corta"))}
    # plano B do teste já ativou (TST-HIB-ON) — não repetir
    on = True if (isinstance(r, dict) and r.get("ja_ativada")) else act_ativar_hibernacao(serial, mcu)
    if on:
        LOG("✅ Hibernação ATIVADA e validada (corta→religa OK).", "ok")
    else:
        LOG("⚠️ Não confirmei a ativação — deixei em IDLE seguro. Pode ativar "
            "manualmente na aba avançada.", "warn")
    STATUS("hibernar", "ok")
    return {"hibernacao": bool(on)}


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
  /* HEADER DE MARCA CHAVI (faixa degradê laranja + logo branco). */
  .brandbar{display:flex; align-items:center; gap:14px; padding:15px 26px;
    background:linear-gradient(100deg,#B8501F 0%,#E86628 52%,#E12E1D 100%)}
  .brand-logo{height:30px; width:auto; display:block; filter:drop-shadow(0 1px 2px rgba(0,0,0,.18))}
  .brand-sub{color:#fff; font-weight:700; font-size:14px; letter-spacing:.3px; opacity:.96}
  /* Bloco de CONTROLE DE VERSÃO no header (direita): versão + data + firmware +
     status da atualização. Texto branco translúcido sobre o degradê. */
  .brand-ver{margin-left:auto; text-align:right; color:#fff; line-height:1.45; white-space:nowrap}
  .brand-ver .bv-top{font-size:12.5px; font-weight:700; opacity:.97}
  .brand-ver .bv-top b{font-size:15px; letter-spacing:.2px}
  .brand-ver .bv-bot{font-size:11px; opacity:.9; margin-top:1px}
  .brand-ver .bv-dot{opacity:.55; margin:0 2px}
  .brand-ver .bv-check{opacity:.85; font-style:italic}
  .brand-ver .bv-ok{color:#DCFCE7; font-style:normal; font-weight:700}       /* atualizado */
  .brand-ver .bv-old{color:#FEF08A; font-style:normal; font-weight:800; cursor:pointer; text-decoration:underline} /* nova versão -> baixar */
  .brand-ver .bv-warn{color:#FED7AA; font-style:normal}                       /* não deu p/ verificar */
  body.has-update .brandbar{margin-top:56px}
  @media (max-width:720px){ .brand-sub{display:none} }
  @media (max-width:560px){ .brand-ver .bv-bot{display:none} }
  *{box-sizing:border-box; font-family:-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
  /* FUNDO com a arte "direcional" da Chavi (trilhas terminando no "C" da marca,
     estilo circuito — combina com bancada de hardware). Tons MUITO claros: um
     padrão SVG data-uri (poucos bytes) em laranja translúcido + dois washes
     radiais suaves nos cantos. Leve, não pesa a leitura. */
  body{margin:0; color:var(--ink);
    background-color:var(--bg);
    background-image:
      radial-gradient(1100px 520px at 88% -8%, rgba(232,102,40,.07), transparent 60%),
      radial-gradient(900px 520px at -8% 112%, rgba(225,46,29,.05), transparent 55%),
      url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='260' height='260' viewBox='0 0 260 260'%3E%3Cg fill='none' stroke='%23E86628' stroke-opacity='.10' stroke-width='3' stroke-linecap='round'%3E%3Cline x1='16' y1='46' x2='214' y2='46'/%3E%3Ccircle cx='228' cy='46' r='9' stroke-dasharray='46 12'/%3E%3Ccircle cx='2' cy='46' r='9' stroke-dasharray='46 12'/%3E%3Cpath d='M132 96 L132 190 Q132 212 154 212 L214 212'/%3E%3Ccircle cx='228' cy='212' r='9' stroke-dasharray='46 12'/%3E%3Ccircle cx='132' cy='82' r='9' stroke-dasharray='46 12'/%3E%3Cline x1='40' y1='150' x2='96' y2='150'/%3E%3Ccircle cx='110' cy='150' r='9' stroke-dasharray='46 12'/%3E%3C/g%3E%3C/svg%3E");
    background-repeat:no-repeat,no-repeat,repeat;
    background-attachment:fixed,fixed,fixed;}
  .wrap{max-width:1280px; margin:0 auto; padding:26px 20px 40px;
        display:flex; gap:24px; align-items:flex-start}
  /* DUAS COLUNAS: comandos/passos à esquerda, logs à direita. */
  .col-left{flex:1 1 520px; min-width:0}
  .col-right{flex:1 1 440px; min-width:0; position:sticky; top:26px}
  body.has-update .col-right{top:88px}
  /* Tela estreita: empilha (log embaixo), pra não espremer nada. */
  @media (max-width:900px){
    .wrap{flex-direction:column}
    .col-right{position:static; width:100%}
  }
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
         padding:12px 20px; color:#fff; transition:.15s;
         background:linear-gradient(180deg,#F07A3C 0%,#E86628 100%);
         box-shadow:0 2px 8px rgba(232,102,40,.28)}
  button:hover{filter:brightness(1.05)} button:active{transform:translateY(1px)}
  button:disabled{opacity:.45; cursor:not-allowed}
  .big{font-size:19px; padding:16px 26px; width:100%}
  .ghost{background:#EEF2F7; color:var(--ink); box-shadow:none}
  .green{background:var(--ok); box-shadow:0 2px 8px rgba(22,163,74,.28)} .grey{background:#94A3B8; box-shadow:none}
  .small{padding:8px 12px; font-size:13px; border-radius:10px}
  /* passos */
  .head{display:flex; align-items:center; justify-content:space-between; margin-bottom:14px}
  .serial{font:800 26px ui-monospace,Menlo,monospace}
  .step{display:flex; align-items:center; gap:14px; padding:14px 16px; border:1px solid var(--line);
        border-radius:14px; background:#fff; margin-bottom:10px; position:relative;
        transform-origin:center left; opacity:1;
        transition:transform .38s cubic-bezier(.22,1,.36,1), box-shadow .38s ease,
                   border-color .38s ease, background .38s ease, opacity .38s ease}
  /* PASSO ATUAL: cresce um pouco + destaque laranja. O efeito DESLIZA pro próximo
     conforme concluem (o anterior encolhe, o próximo cresce) — clareza imediata
     de "onde estou". Só os passos CONCLUÍDOS apagam um pouco. */
  .step.active{transform:scale(1.035); z-index:2;
        border-color:var(--orange); background:#FFF8F3;
        box-shadow:0 10px 26px rgba(232,102,40,.16)}
  .step.done{opacity:.6}
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
       font:12px ui-monospace,Menlo,monospace; height:calc(100vh - 150px); min-height:300px;
       overflow:auto; white-space:pre-wrap; line-height:1.55}
  @media (max-width:900px){ #log{height:320px} }
  #log .ok{color:#4ADE80} #log .err{color:#F87171} #log .warn{color:#FBBF24} #log .hi{color:#FDBA74}
  /* modal */
  /* z-index ALTO: a modal fica acima de tudo — passo ativo (z2), barra #proc (z50)
     e banner de update (z100). Sem isso, o destaque do passo atual subia por cima. */
  .mask-bg{position:fixed; inset:0; background:rgba(15,23,42,.45); display:none; align-items:center; justify-content:center; z-index:1000}
  .modal{background:#fff; border-radius:16px; padding:24px; width:340px; text-align:center}
  .modal h3{margin:0 0 4px} .modal p{color:var(--muted); font-size:13px; margin:0 0 14px}
  .modal input{font:700 22px ui-monospace,Menlo,monospace; text-align:center; width:100%;
        border:2px solid var(--line); border-radius:10px; padding:10px; outline:none}
  .modal .row{margin-top:16px; gap:8px}
  .hide{display:none}
  /* lado direito da barra: status de login (pill) + bloco de versão */
  .brand-right{margin-left:auto; display:flex; align-items:center; gap:16px}
  .toplink{font-size:11.5px; font-weight:700; color:#fff; cursor:pointer; white-space:nowrap;
    background:rgba(255,255,255,.16); padding:4px 11px; border-radius:20px; transition:background .2s}
  .toplink:hover{background:rgba(255,255,255,.28)}
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
</style></head>
<body>
<div id="update-banner">
  <span class="ub-txt">⚠️ Atualização disponível: <b id="ub-new">—</b>
    — você está na v<span id="ub-cur">—</span></span>
  <button id="ub-btn" class="keep">⬇ Baixar atualização</button>
</div>
<div id="proc"><span class="spin"></span>Executando…</div>
<div class="brandbar">
  <img class="brand-logo" alt="Chavi" src="data:image/png;base64,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">
  <span class="brand-sub">Bancada de Fechaduras FI</span>
  <div class="brand-right">
    <button id="dev-toggle" class="ghost small" style="margin-right:10px">…</button>
    <span class="toplink" id="login-state">não conectado</span>
    <div class="brand-ver" id="brand-ver" title="Controle de versão da bancada">
      <div class="bv-top">Bancada <b id="bv-cur">v—</b> <span class="bv-dot">·</span> <span id="bv-date">—</span></div>
      <div class="bv-bot">firmware <b id="bv-fw">v—</b> <span class="bv-dot">·</span> <span id="bv-status" class="bv-check">verificando atualização…</span></div>
    </div>
  </div>
</div>
<div class="wrap">
  <div class="col-left">

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
          title="Pino do MOSFET do módulo BLE. 90% das FIs = 8. Só mude se a placa usar outro. 12 = MOSFET AUTOMÁTICO (placa v2.7/retrofit 2024: gate no pino físico 12 = PIO2; o firmware usa PWRM1 e o módulo corta/religa sozinho).">
      </div>
    </div>
    <button class="big" id="btn-next" style="margin-top:20px" disabled>PRÓXIMO ▶</button>
    <!-- fechadura JÁ GRAVADA (nesta ou noutra bancada): testa direto por BLE,
         sem cabo — pula Gravar/Validar e roda rádio→conectar→auto-teste. -->
    <button class="big" id="btn-ble-direct" style="margin-top:10px;background:var(--muted)" disabled>
      📶 JÁ GRAVADA — testar por BLE (sem cabo)</button>
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

    <!-- ⭐ v2.13: teste de HIBERNAÇÃO visível (validação do MOSFET). Fora do
         fluxo numerado de propósito — é teste de engenharia, sob demanda.
         Usa o campo "Pino MOSFET" da tela 1: 12 = teste por UPTIME (auto,
         ~3,5 min); 4..9 = teste clássico TST-HIB (corte comandado). -->
    <div style="margin-top:18px;padding:12px;border:1px dashed var(--muted);border-radius:10px">
      <div style="font-weight:600;margin-bottom:6px">🔋 Hibernação (mosfet) — validação</div>
      <div style="color:var(--muted);font-size:13px;margin-bottom:10px">
        Requer o rádio conectado (passo 1 feito). Pino MOSFET <b>12</b> (AUTO):
        desconecta, espera ~3 min o auto-sleep cortar e reconecta conferindo o
        <b>UPTIME</b> — acompanhe no REGISTRO e <b>não mexa na fechadura</b>
        durante a espera. Pinos 4..9: corte comandado (TST-HIB).</div>
      <button id="btn-hibernar" onclick="runStep('hibernar', this)">🔋 Testar hibernação</button>
    </div>

    <!-- RECUPERAÇÃO: OCULTA a pedido do Leonardo (09/07/2026). Para reativar,
         descomente este bloco INTEIRO e a fiação JS dos botões (procurar por
         "RECUPERAÇÃO (botões estáticos"). O backend (/api/renomear,
         /api/consertar, steps hibernar/hib-off) continua funcionando.
    <div class="recovery">
      <div class="rec-title">⚠️ Recuperação — só se algo der errado</div>
      <div class="rec-sub">No fluxo NORMAL você <b>não precisa</b> destes botões — basta seguir os passos numerados acima. Use um destes só quando o problema abaixo acontecer:</div>
      <div class="rec-item">
        <button id="btn-consertar">🩺 Consertar módulo</button>
        <div class="rec-desc">Diagnóstico completo pelo ar (VERS/BAUD/MODE/PWRM/NAME) + conserto: <b>acorda módulo dormindo</b> (PWRM1 legado) e força 9600. Use se a comunicação estiver <b>muda/saindo com lixo</b>.</div>
      </div>
      <div class="rec-item">
        <button id="btn-renomear">🔧 Renomear módulo</button>
        <div class="rec-desc">Manual — normalmente <b>não precisa</b>: o passo 1 já adota sozinho a fechadura com nome errado (ciclo desliga/religa). Use só se quiser renomear digitando o nome errado.</div>
      </div>

      <div class="rec-item" style="margin-top:16px;border-top:1px dashed var(--amber);padding-top:14px">
        <div class="rec-title" style="font-size:14px">🔋 Hibernação (economia de bateria — AVANÇADO, opcional)</div>
        <div class="rec-desc" style="margin-bottom:10px">NÃO faz parte do fluxo normal — a fechadura já sai no modo IDLE seguro. Use só se quiser <b>ligar</b> a hibernação profunda numa unidade (corta o MCU no repouso): o botão testa o ciclo corta→religa e só ativa se passar. O outro botão volta ao modo seguro.</div>
        <div class="row" style="flex-wrap:wrap;gap:8px">
          <button id="btn-hib-on">🔋 Testar e ativar hibernação</button>
          <button id="btn-hib-off">🔌 Desativar (modo seguro)</button>
        </div>
      </div>
    </div>
    FIM DA RECUPERAÇÃO OCULTA -->
  </div>

  </div><!-- /col-left -->

  <div class="col-right">
  <div class="logbar"><b>REGISTRO</b><button class="ghost small" onclick="salvarLog()">salvar</button></div>
  <div id="log"></div>
  </div><!-- /col-right -->
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
// FLUXO ENXUTO (3 passos que o operador vê). Provisionar+Conectar viraram
// UMA fase interna do passo 1 (o "Gravar e preparar" encadeia gravar →
// validar → prepara rádio → conecta, tudo sozinho). Hibernação saiu do fluxo
// (agora é só Avançado). Os 3 passos batem com a tarefa, não com a engenharia.
const PASSOS = [
  ["gravar","1 · Gravar e preparar","Grava o firmware e as seeds (cabo USBasp), valida, e já PREPARA o rádio + CONECTA por Bluetooth — tudo de uma vez. Só este passo usa o cabo."],
  ["autoteste","2 · Testar","Testa cada peça e PERGUNTA se funcionou de verdade (buzzer, LEDs, motor, bateria)."],
  ["cadastrar","3 · Cadastrar no sistema","Registra só o serial no backend."],
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
  // Hibernação (⚡): agora é fluxo AUTOMATIZADO na seção de RECUPERAÇÃO
  // (Testar/Ativar/Desativar), que valida o ciclo corta→religa sozinho.
];
let SERIAL="", MCU="m328pb", busy=false;

const $ = s=>document.querySelector(s);
const ggg=$("#ggg"), nnn=$("#nnn");

// Complementa com ZEROS À ESQUERDA: CH sempre 3 dígitos, FI sempre 6.
// Ex.: digitar "3" -> "003"; "2585" -> "002585". Só dígitos.
function padCH(v){ v=(v||"").replace(/\\D/g,""); return v ? v.padStart(3,'0').slice(-3) : ''; }
function padFI(v){ v=(v||"").replace(/\\D/g,""); return v ? v.padStart(6,'0').slice(-6) : ''; }

function serialAtual(){
  const g=padCH(ggg.value), n=padFI(nnn.value);
  return (g && n) ? ("CH"+g+"FI"+n) : null;
}

function onlyDigits(e){
  e.target.value = e.target.value.replace(/\\D/g,"");
  // AO COMPLETAR a quantidade de dígitos, pula pro próximo campo.
  if(e.target===ggg && ggg.value.length>=3) nnn.focus();
  prev();
}
async function prev(){
  const s=serialAtual();
  if(s){ $("#prev").textContent="→  "+s; $("#prev").style.color="var(--ok)"; $("#btn-next").disabled=false;
    $("#btn-ble-direct").disabled=false;
    const r=await fetch("/api/seeds?serial="+s).then(r=>r.json());
    $("#seeds").textContent="seeds: "+r.seeds.join(" · ");
  } else { $("#prev").textContent="digite o canal (CH) e o nº (FI) — completo com zeros à esquerda"; $("#prev").style.color="var(--err)";
    $("#seeds").textContent=""; $("#btn-next").disabled=true; $("#btn-ble-direct").disabled=true; }
}
ggg.addEventListener("input",onlyDigits); nnn.addEventListener("input",onlyDigits);
// AO SAIR do campo, mostra já preenchido com os zeros à esquerda.
ggg.addEventListener("blur",()=>{ if(ggg.value) ggg.value=padCH(ggg.value); prev(); });
nnn.addEventListener("blur",()=>{ if(nnn.value) nnn.value=padFI(nnn.value); prev(); });
ggg.addEventListener("keydown",e=>{if(e.key==="Enter"){ if(ggg.value) ggg.value=padCH(ggg.value); nnn.focus(); }});
nnn.addEventListener("keydown",e=>{if(e.key==="Enter"){ if(nnn.value) nnn.value=padFI(nnn.value); prev(); if(!$("#btn-next").disabled)irPassos(); }});
$("#btn-next").onclick=irPassos;
$("#btn-ble-direct").onclick=irPassosBle;

function irPassos(){
  SERIAL=serialAtual(); MCU=$("#mcu").value;
  $("#serial-lbl").textContent=SERIAL;
  $("#tela-serial").classList.add("hide"); $("#tela-passos").classList.remove("hide");
  renderSteps();
}

// FECHADURA JÁ GRAVADA: pula o CABO (passo 1) e roda sozinho o caminho BLE —
// prepara rádio (adoção/conserto pelo ar se preciso) → conecta → auto-teste.
async function irPassosBle(){
  irPassos();
  setChip("gravar","skip");
  if(!(await prepararRadio())) return;
  await autoteste();
}
function voltar(){ $("#tela-passos").classList.add("hide"); $("#tela-serial").classList.remove("hide"); }

function renderSteps(){
  const c=$("#steps"); c.innerHTML=""; STEP_STATE={};
  for(const [k,t,d] of PASSOS){
    const el=document.createElement("div"); el.className="step"; el.id="step-"+k;
    el.innerHTML=`<div class="chip" id="chip-${k}">○</div>
      <div class="t"><b>${t}</b><div>${d}</div></div>
      <button id="btn-${k}">Executar</button>`;
    c.appendChild(el);
    STEP_STATE[k]="pending";
    $("#btn-"+k).onclick = (k==="autoteste") ? autoteste : ()=>runStep(k);
  }
  atualizaPassoAtivo();   // 1º passo já nasce destacado
  const comp=$("#comp"); comp.innerHTML="";
  for(const t of TESTES){ const b=document.createElement("button");
    b.textContent=t.label; b.onclick=(e)=>teste1(t, e.currentTarget); comp.appendChild(b); }
  // RECUPERAÇÃO (botões estáticos na TELA 2) — HTML OCULTO a pedido do
  // Leonardo (09/07/2026); fiação guardada por existência p/ reativar fácil
  // (basta descomentar o bloco <div class="recovery"> no HTML).
  if($("#btn-consertar")) $("#btn-consertar").onclick=doConsertar;
  if($("#btn-renomear"))  $("#btn-renomear").onclick=doRenomear;
  if($("#btn-hib-on"))    $("#btn-hib-on").onclick=(e)=>runStep("hibernar", e.currentTarget);
  if($("#btn-hib-off"))   $("#btn-hib-off").onclick=(e)=>runStep("hib-off", e.currentTarget);
}

let STEP_STATE={};
function setChip(step,state){
  const el=$("#chip-"+step); if(!el)return;
  el.className="chip "+(state==="pending"?"":(state==="skip"?"":state));
  el.textContent={pending:"○",run:"⏳",ok:"✓",fail:"✗",skip:"—"}[state]||"○";
  STEP_STATE[step]=state;
  atualizaPassoAtivo();
}

// Destaca (AUMENTA) o passo ATUAL e desliza o efeito conforme avança: ativo = o
// que está EXECUTANDO; senão o PRÓXIMO a fazer (1º ainda não concluído). Passos
// já concluídos ganham .done (apagam um pouco). A transição do CSS faz o
// anterior encolher e o próximo crescer suavemente.
function atualizaPassoAtivo(){
  const ordem = PASSOS.map(p=>p[0]);
  let ativo = ordem.find(k=>STEP_STATE[k]==="run");
  if(!ativo) ativo = ordem.find(k=>STEP_STATE[k]!=="ok" && STEP_STATE[k]!=="skip");   // 1º pendente/falho
  for(const k of ordem){
    const el=$("#step-"+k); if(!el) continue;
    el.classList.toggle("active", k===ativo);
    el.classList.toggle("done", (STEP_STATE[k]==="ok"||STEP_STATE[k]==="skip") && k!==ativo);
  }
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
  if(busy)return {ok:false};
  btn = btn || $("#btn-"+step);
  setBusy(true, btn);
  const mosfet=($("#mosfet")&&$("#mosfet").value)||"8";
  const r=await fetch("/api/step",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({step,serial:SERIAL,mcu:MCU,mosfet})}).then(r=>r.json()).catch(()=>({ok:false}));
  setBusy(false);
  if(r && r.need_login){ abrirLogin(); return r; }
  // AUTO-FLUXO do passo 1 ("Gravar e preparar"): grava → valida → prepara
  // rádio → conecta, tudo SOZINHO — o operador vê só 1 passo. Para em qualquer
  // falha (o REGISTRO à direita mostra onde travou). O passo 2 (Testar) fica
  // manual: precisa do humano confirmar que apitou/girou. Hibernação NÃO roda
  // mais aqui (virou Avançado, sob demanda).
  if(step==="gravar" && r && r.ok){
    setChip("gravar","run");
    const v = await runStep("validar");   if(!(v&&v.ok)){ setChip("gravar","fail"); return r; }
    if(!(await prepararRadio())){ setChip("gravar","fail"); return {ok:false}; }
    setChip("gravar","ok");
  }
  return r;
}
// Fase interna "preparar rádio" = provisionar (confere/conserta pelo ar/adota)
// + conectar (PONG). Usada tanto pelo passo 1 quanto pelo atalho "JÁ GRAVADA".
async function prepararRadio(){
  const p = await runStep("provisionar"); if(!(p&&p.ok)) return false;
  const c = await runStep("conectar");    if(!(c&&c.ok)) return false;
  return true;
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
  $("#login-state").style.color=on?"#DCFCE7":"#fff"; }

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
function fmtData(iso){ const m=/^(\d{4})-(\d{2})-(\d{2})/.exec(iso||""); return m?`${m[3]}/${m[2]}/${m[1]}`:(iso||"—"); }

/* ⚡ TOGGLE do modo de gravação (header). RÁPIDO = só Gravar/Validar rodam de
   verdade (demais passos pulados com aviso; cadastro BLOQUEADO) — para iterar
   firmware na mesa. COMPLETO = fluxo normal de produção com todos os testes. */
function setDevUI(on){
  const b=$("#dev-toggle"); if(!b) return;
  b.textContent = on ? "⚡ Modo de gravação: RÁPIDO (sem testes)"
                     : "✅ Modo de gravação: COMPLETO (com testes)";
  b.style.background = on ? "var(--amber)" : "";
  b.style.color      = on ? "#000" : "";
  b.style.fontWeight = on ? "700" : "";
  b.title = on
    ? "Só Gravar e Validar executam; os demais passos são PULADOS e o cadastro fica BLOQUEADO. Clique para voltar ao modo completo."
    : "Fluxo completo de produção (todos os testes). Clique para o modo rápido (iteração de firmware na bancada).";
}
$("#dev-toggle").onclick = async ()=>{
  const ligar = !$("#dev-toggle").textContent.includes("RÁPIDO");
  try{
    const r = await fetch("/api/dev-mode",{method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({on:ligar})}).then(r=>r.json());
    if(r && r.ok) setDevUI(r.dev);
  }catch(e){}
};

async function updateCheck(tent){
  let d;
  try{ d=await fetch("/api/update-check").then(r=>r.json()); }
  catch(e){ return; }                       // sem servidor/rede: ignora
  if(!d) return;
  // HEADER (controle de versão): versão/data/firmware já vêm de cara.
  if(d.current)  $("#bv-cur").textContent  = "v"+d.current;
  if(d.date)     $("#bv-date").textContent = fmtData(d.date);
  if(d.firmware) $("#bv-fw").textContent   = "v"+d.firmware;
  if($("#brand-ver")) $("#brand-ver").title =
    `Bancada v${d.current} (${fmtData(d.date)}) · firmware v${d.firmware}`+(d.notes?`\n${d.notes}`:"");
  const st=$("#bv-status");
  setDevUI(!!d.dev);                         // ⚡ estado inicial do toggle de modo
  if(!d.checado){                            // checagem ainda rodando: re-tenta
    if(st){ st.className="bv-check"; st.textContent="verificando atualização…"; st.onclick=null; }
    if((tent||0)<5) setTimeout(()=>updateCheck((tent||0)+1), 2500);
    return;
  }
  if(d.outdated && d.latest){                // versão nova disponível
    $("#ub-new").textContent = "v"+d.latest;
    $("#ub-cur").textContent = d.current;
    $("#update-banner").classList.add("on");
    document.body.classList.add("has-update");
    if(st){ st.className="bv-old"; st.textContent="⚠ v"+d.latest+" disponível — baixar";
      st.onclick=()=>fetch("/api/open-update",{method:"POST"}).catch(()=>{}); }
  } else if(d.latest){                        // está na última
    if(st){ st.className="bv-ok"; st.textContent="✓ atualizado"; st.onclick=null; }
  } else {                                    // checou mas não obteve a última (ex.: repo privado sem token)
    if(st){ st.className="bv-warn"; st.textContent="atualização não verificável"; st.onclick=null; }
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
            self._send(200, {**_UPDATE, "dev": DEV_MODE}); return
        if self.path == "/events":
            self._sse(); return
        self._send(404, {"erro": "nao encontrado"})

    def do_POST(self):
        try:
            b = self._body()
        except Exception:
            b = {}
        if self.path == "/api/dev-mode":
            # ⚡ Toggle do modo de gravação pela UI (09/07, pedido do Leonardo):
            # RÁPIDO (sem testes) = só Gravar/Validar executam, demais passos
            # pulados com aviso, cadastro bloqueado · COMPLETO = fluxo normal.
            # BANCADA_DEV=1 no ambiente continua valendo como estado inicial.
            global DEV_MODE
            DEV_MODE = bool(b.get("on"))
            LOG("⚡ modo de gravação RÁPIDO (sem testes): passos pulados, "
                "cadastro bloqueado" if DEV_MODE else
                "✅ modo de gravação COMPLETO (com testes) ativo", "hi")
            self._send(200, {"ok": True, "dev": DEV_MODE}); return
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
        # ⚡ MODO DEV: só Gravar/Validar rodam de verdade. Os demais passos são
        # PULADOS com aviso (nunca "aprovados" em silêncio) e Cadastrar é
        # BLOQUEADO — fechadura de iteração não entra no backend.
        if DEV_MODE and step not in ("gravar", "validar"):
            if step == "cadastrar":
                LOG("⚡ modo DEV: CADASTRO BLOQUEADO — rode a bancada sem "
                    "BANCADA_DEV p/ cadastrar de verdade", "err")
                return {"ok": False, "erro": "cadastro bloqueado no modo dev"}
            LOG(f"⚡ modo DEV: passo '{step}' PULADO (não executado, não testado)", "hi")
            return {"ok": True, "pulado": True}
        if step == "provisionar":
            r = act_provisionar(serial, mcu, b.get("mosfet", "8"))
            return {"ok": bool(r)}
        if step == "gravar":
            r = act_gravar(serial, mcu, b.get("mosfet", "8"))
            return {"ok": bool(r)}
        if step == "hibernar":
            r = act_hibernar_seguro(serial, mcu, b.get("mosfet", "8"))
            return {"ok": True, "hibernacao": bool(r and r.get("hibernacao"))}
        fn = {"gravar": act_gravar, "validar": act_validar, "conectar": act_conectar,
              "autoteste": act_autoteste, "cadastrar": act_cadastrar,
              "hib-on": act_ativar_hibernacao,
              "hib-off": act_desativar_hibernacao}.get(step)
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
    if DEV_MODE:
        print(">> ⚡ MODO DEV ativo (BANCADA_DEV=1): só Gravar/Validar executam; "
              "demais passos PULADOS; cadastro BLOQUEADO")
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
