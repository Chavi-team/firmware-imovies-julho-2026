#!/usr/bin/env python3
"""
soak_test.py — TESTE DE ESTRESSE AUTOMATIZADO (pior caso de uso, sem humano).

Roda por N minutos exercitando a fechadura como a vida real faria — e pior —
lendo a TELEMETRIA do firmware (v2.18+) a cada ciclo. Substitui "achismo" por
números: quantos brown-outs, quantos cortes de energia efetivos, quanto a
tensão afunda no giro, quanto tempo leva para religar.

CENÁRIOS sorteados a cada ciclo (o "pior caso" é a mistura deles):
  · repouso CURTO  (5s)   — comando logo após o anterior, placa ainda acordada
  · repouso MÉDIO  (25s)  — bem na janela em que o firmware corta a energia
  · repouso LONGO  (70s)  — placa cortada há tempo, religa do zero
  · RAJADA          —      dois comandos seguidos (testa o anti-duplicata)
  · ABRIR / FECHAR  —      alterna o sentido

O QUE É MEDIDO POR CICLO (tudo do TST-INFO do firmware):
  religa_s   tempo até o 1º PONG após conectar (religamento da placa cortada)
  conf1/2_s  1ª confirmação (chegada do comando) e 2ª (motor PAROU)
  bat        tensão reportada na confirmação
  vcc/vccmin trilho agora / MENOR tensão vista durante o giro
  rst        motivo do último boot: P=power-on (religou pelo mosfet)
             B=BROWN-OUT (caiu a tensão) E=externo W=watchdog
  boots/bods/cuts  contadores acumulados (boots, brown-outs, cortes executados)

VEREDITOS AUTOMÁTICOS no fim:
  · falhas de comando (sem PONG / sem confirmação) — tem que ser ZERO
  · brown-outs (BODS) — tem que ser ZERO
  · corte de energia: CUTS x BOOTS provam se o corte está mesmo cortando
  · piora da bateria ao longo do teste

Uso:
    tools/.venv-bancada/bin/python tools/soak_test.py 003FI002910 [minutos]

⚠️ Feche o app do celular / Bluetooth do telefone desligado durante o teste.
"""
import asyncio
import csv
import random
import re
import sys
import time
from datetime import datetime

from bleak import BleakClient, BleakScanner

FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
REPOUSOS = [("curto", 5), ("medio", 25), ("longo", 70)]
PROBE_TETO = 25.0
CONF_TETO = 25.0


class Sessao:
    def __init__(self):
        self.buf = ""

    def cb(self, _s, data: bytearray):
        self.buf += data.decode("utf-8", errors="ignore")

    def confs(self):
        return len(re.findall(r"[12]0\d{2}", self.buf))

    def campo(self, nome, padrao=None):
        m = re.search(rf"{nome}:(-?\w+)", self.buf)
        return m.group(1) if m else padrao


async def achar(alvo, timeout=10.0):
    for _ in range(3):
        devs = await BleakScanner.discover(timeout=timeout)
        for d in devs:
            if (d.name or "").upper() == alvo.upper():
                return d.address
    return None


async def ciclo(addr, alvo, verbo, rajada, log):
    r = {"t": datetime.now().strftime("%H:%M:%S"), "verbo": verbo,
         "rajada": int(rajada), "religa_s": None, "conf1_s": None,
         "conf2_s": None, "bat": None, "vcc": None, "vccmin": None,
         "rst": None, "boots": None, "bods": None, "cuts": None, "erro": ""}
    try:
        async with BleakClient(addr, timeout=20.0) as c:
            s = Sessao()
            await c.start_notify(FFE1, s.cb)
            # bytes de sacrifício (iguais aos do app) + sonda paciente
            await c.write_gatt_char(FFE1, b"\n", response=False)
            await asyncio.sleep(0.15)
            t0 = time.monotonic()
            while time.monotonic() - t0 < PROBE_TETO and "PONG" not in s.buf:
                await c.write_gatt_char(FFE1, b"TST-PING", response=False)
                fim = time.monotonic() + 0.4
                while time.monotonic() < fim and "PONG" not in s.buf:
                    await asyncio.sleep(0.03)
            if "PONG" not in s.buf:
                r["erro"] = "SEM-PONG"
                await c.stop_notify(FFE1)
                log(f"  ✗ SEM PONG em {PROBE_TETO:.0f}s (o app daria F07)")
                return r
            r["religa_s"] = round(time.monotonic() - t0, 2)

            # verbo (+ rajada opcional: 2º comando 1,2s depois — anti-duplicata)
            s.buf = ""
            t1 = time.monotonic()
            await c.write_gatt_char(FFE1, verbo.encode(), response=False)
            if rajada:
                await asyncio.sleep(1.2)
                await c.write_gatt_char(FFE1, verbo.encode(), response=False)
            while time.monotonic() - t1 < CONF_TETO:
                n = s.confs()
                if r["conf1_s"] is None and n >= 1:
                    r["conf1_s"] = round(time.monotonic() - t1, 2)
                if n >= 2:
                    r["conf2_s"] = round(time.monotonic() - t1, 2)
                    break
                await asyncio.sleep(0.05)
            m = re.search(r"[12]0(\d)\.(\d\d)", s.buf)
            if m:
                r["bat"] = float(f"{m.group(1)}.{m.group(2)}")
            if r["conf1_s"] is None:
                r["erro"] = "SEM-CONFIRMACAO"
            elif r["conf2_s"] is None:
                r["erro"] = "SEM-FIM-DE-GIRO"

            # telemetria
            s.buf = ""
            await c.write_gatt_char(FFE1, b"TST-INFO", response=False)
            t2 = time.monotonic()
            while time.monotonic() - t2 < 12 and "FIM-INFO" not in s.buf:
                await asyncio.sleep(0.05)
            for k in ("VCC", "VCCMIN", "RST", "BOOTS", "BODS", "CUTS"):
                r[k.lower()] = s.campo(k)
            await c.stop_notify(FFE1)
    except Exception as e:
        r["erro"] = f"EXCECAO:{type(e).__name__}"
    log(f"  religa={r['religa_s']}s conf1={r['conf1_s']}s conf2={r['conf2_s']}s "
        f"bat={r['bat']}V vcc={r['vcc']} vccmin={r['vccmin']} rst={r['rst']} "
        f"boots={r['boots']} bods={r['bods']} cuts={r['cuts']} "
        f"{'⚠️ ' + r['erro'] if r['erro'] else '✓'}")
    return r


async def main():
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    alvo = sys.argv[1].strip().upper()
    minutos = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
    rnd = random.Random(20260802)          # sequência reproduzível

    csv_path = f"soak_{alvo}_{datetime.now().strftime('%H%M')}.csv"
    print(f"SOAK TEST · {alvo} · {minutos:.0f} min · saída: {csv_path}")

    def log(msg):
        print(msg, flush=True)

    addr = await achar(alvo)
    if not addr:
        print("✗ fechadura não encontrada no ar"); raise SystemExit(1)
    print(f"encontrada: {addr}")

    # zera a telemetria para o teste começar do zero
    try:
        async with BleakClient(addr, timeout=20.0) as c:
            s = Sessao()
            await c.start_notify(FFE1, s.cb)
            await asyncio.sleep(1.0)
            await c.write_gatt_char(FFE1, b"TST-ZERA", response=False)
            await asyncio.sleep(1.5)
            print(f"telemetria zerada: {'OK-ZERA' in s.buf}")
            await c.stop_notify(FFE1)
    except Exception as e:
        print(f"(não consegui zerar a telemetria: {e})")

    fim = time.monotonic() + minutos * 60
    linhas, n = [], 0
    while time.monotonic() < fim:
        n += 1
        nome, espera = rnd.choice(REPOUSOS)
        verbo = "ABRIR" if n % 2 else "FECHAR"
        rajada = (n % 5 == 0)
        print(f"\n[{n}] {verbo}{' +RAJADA' if rajada else ''} "
              f"(repouso anterior: {nome}/{espera}s) "
              f"— restam {int((fim - time.monotonic()) / 60)} min")
        r = await ciclo(addr, alvo, verbo, rajada, log)
        r["repouso"] = espera
        linhas.append(r)
        with open(csv_path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(linhas[0].keys()))
            w.writeheader(); w.writerows(linhas)
        if time.monotonic() >= fim:
            break
        print(f"  … repouso de {espera}s (a fechadura deve CORTAR a energia)")
        await asyncio.sleep(espera)

    # ---------------- veredito ----------------
    print("\n" + "=" * 62)
    print(f"RESULTADO — {len(linhas)} ciclos em {minutos:.0f} min")
    falhas = [r for r in linhas if r["erro"]]
    print(f"  falhas de comando: {len(falhas)}/{len(linhas)}"
          + (" ✅" if not falhas else " ❌"))
    for f_ in falhas[:8]:
        print(f"     [{f_['t']}] {f_['verbo']} -> {f_['erro']}")

    def nums(k):
        return [float(r[k]) for r in linhas if r.get(k) not in (None, "", "None")]

    rel, c2 = nums("religa_s"), nums("conf2_s")
    if rel:
        print(f"  religamento (até o PONG): min {min(rel):.2f}s · "
              f"máx {max(rel):.2f}s · média {sum(rel)/len(rel):.2f}s")
    if c2:
        print(f"  giro completo: min {min(c2):.2f}s · máx {max(c2):.2f}s")
    vmin = nums("vccmin")
    if vmin:
        v = [x for x in vmin if x > 0]
        if v:
            print(f"  trilho no giro (VCCMIN): mínimo {min(v):.0f}mV "
                  + ("✅ folgado" if min(v) > 4300 else "⚠️ perto do brown-out"))
    bats = nums("bat")
    if bats:
        print(f"  bateria: início {bats[0]:.2f}V · fim {bats[-1]:.2f}V · "
              f"queda {bats[0]-bats[-1]:.2f}V")
    ult = linhas[-1]
    print(f"  contadores finais: BOOTS={ult['boots']} BODS={ult['bods']} "
          f"CUTS={ult['cuts']}")
    try:
        bo, bd, ct = int(ult["boots"]), int(ult["bods"]), int(ult["cuts"])
        print(f"  brown-outs: {bd} " + ("✅ nenhum" if bd == 0 else "❌ HÁ QUEDA DE TENSÃO"))
        if ct == 0:
            print("  corte de energia: ❌ o firmware NUNCA executou o corte")
        elif bo >= ct * 0.7:
            print(f"  corte de energia: ✅ funcionando ({ct} cortes -> {bo} religamentos)")
        else:
            print(f"  corte de energia: ⚠️ comandado {ct}x mas só {bo} boots — "
                  "o módulo pode estar ignorando o comando")
    except (TypeError, ValueError):
        print("  (contadores incompletos — firmware sem telemetria v2.18?)")
    print(f"\nCSV: {csv_path}")


asyncio.run(main())
