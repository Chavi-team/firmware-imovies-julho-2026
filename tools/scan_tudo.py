#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scan_tudo.py — Acha e identifica um módulo BLE "anônimo" (pós-AT+RENEW ele
volta ao nome de fábrica, que a bancada não procura).

MODO 1 — DIFERENCIAL (default): escaneia com a fechadura LIGADA, pede para
desligar, escaneia de novo e mostra QUEM SUMIU (= a fechadura).

    ./tools/.venv-bancada/bin/python tools/scan_tudo.py

MODO 2 — SONDA: conecta num endereço e o identifica: lista os serviços e, se
tiver a FFE1, manda AT+VERS? / AT+BAUD? / AT+NAME? PELO AR (AT remoto) e
mostra as respostas — diz na hora se é um módulo nosso e em que baud/nome está.

    ./tools/.venv-bancada/bin/python tools/scan_tudo.py --sonda B425BB6C-...
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"


async def varrer(rotulo):
    print(f">> Escaneando ({rotulo}, 12s)...")
    devs = await BleakScanner.discover(timeout=12.0, return_adv=True)
    vistos = {}
    for addr, (dev, adv) in devs.items():
        nome = adv.local_name or dev.name or "(sem nome)"
        vistos[dev.address] = (nome, adv.rssi)
    return vistos


async def diferencial():
    input(">> Deixe a fechadura LIGADA (e espere ~40s após o boot). ENTER para escanear...")
    com = await varrer("ligada")
    input(">> Agora DESLIGUE a fechadura (bateria/USBasp fora). ENTER para escanear de novo...")
    time.sleep(2)
    sem = await varrer("desligada")
    sumiram = {a: v for a, v in com.items() if a not in sem}
    print("\n=== SUMIRAM ao desligar (a fechadura está entre estes) ===")
    if not sumiram:
        print("  (nenhum — o módulo dela não estava anunciando nem ligada)")
    for a, (nome, rssi) in sorted(sumiram.items(), key=lambda x: -(x[1][1] or -199)):
        print(f"  {rssi:>5}  {nome:<24}  {a}")
    if sumiram:
        melhor = max(sumiram.items(), key=lambda x: (x[1][1] or -199))[0]
        print(f"\n>> Religue a fechadura e rode a sonda no mais forte:\n"
              f"   ./tools/.venv-bancada/bin/python tools/scan_tudo.py --sonda {melhor}")


async def sonda(addr):
    # Aceita UUID OU o NOME do anúncio (ex.: 001FI000123). CoreBluetooth só
    # conecta em quem foi visto num scan DESTE processo — acha primeiro
    # (com retentativas; anúncio pode ser intermitente).
    por_nome = "-" not in addr
    dev = None
    for tent in range(1, 4):
        print(f">> Procurando {'nome' if por_nome else 'endereço'} '{addr}' "
              f"(tentativa {tent}/3, 15s)...")
        if por_nome:
            dev = await BleakScanner.find_device_by_name(addr, timeout=15.0)
        else:
            dev = await BleakScanner.find_device_by_address(addr, timeout=15.0)
        if dev:
            break
    if not dev:
        print(">> Não apareceu no ar. Religue a fechadura, espere ~40s e tente de "
              "novo — ou rode o modo DIFERENCIAL primeiro para confirmar o endereço.")
        return
    notif = []

    def cb(_c, data):
        txt = bytes(data).decode("utf-8", "replace").strip()
        if txt:
            notif.append(txt)
            print(f"   ⟵ {txt!r}")

    # Conexão com retentativas (anúncio intermitente / primeiro connect falha).
    cli = None
    for tent in range(1, 4):
        try:
            print(f">> Conectando em {dev.address} (tentativa {tent}/3)...")
            cli = BleakClient(dev, timeout=20.0)
            await cli.connect()
            break
        except Exception as e:
            print(f"   falhou: {type(e).__name__}")
            cli = None
            await asyncio.sleep(2)
            dev = await BleakScanner.find_device_by_address(addr, timeout=12.0) or dev
    if cli is None or not cli.is_connected:
        print(">> NÃO CONECTA mesmo anunciando = anúncio NÃO-CONECTÁVEL (estado "
              "pós-RENEW/beacon). O conserto pelo ar não alcança — use o "
              "diagnóstico por BIPES do firmware (regrave e ouça o boot).")
        return
    async with cli:
        print(">> Conectado. Serviços/características:")
        ffe1 = False
        for svc in cli.services:
            u = svc.uuid.replace("-0000-1000-8000-00805f9b34fb", "")
            chars = ", ".join(c.uuid.replace("-0000-1000-8000-00805f9b34fb", "")
                              for c in svc.characteristics)
            print(f"   svc {u}: [{chars}]")
            ffe1 = ffe1 or any("ffe1" in c.uuid for c in svc.characteristics)
        if not ffe1:
            print(">> SEM ffe1 — não é um módulo das fechaduras. Tente outro endereço.")
            return
        await cli.start_notify(CHR_FFE1, cb)
        await asyncio.sleep(1.0)
        # BEFC?/AFTC?/PIO? são a joia da coroa: numa FI 1.0 VIVA (firmware
        # antigo) eles revelam a config EXATA que segura o trilho de energia
        # (mosfet) desta revisão de placa — que copiamos p/ as placas mortas.
        for cmd in ("AT+VERS?", "AT+BAUD?", "AT+NAME?", "AT+MODE?", "AT+ROLE?",
                    "AT+BEFC?", "AT+AFTC?", "AT+PIO?", "AT+STATUS?", "AT+SHIELD?"):
            print(f"   ⟶ {cmd}")
            await cli.write_gatt_char(CHR_FFE1, cmd.encode(), response=False)
            await asyncio.sleep(1.2)
        print("\n>> É UM MÓDULO NOSSO se respondeu 'Soft AT...'/OK+Get. O BAUD? diz a "
              "tabela (Get:0/2/6...); o NAME? confirma a identidade. Me mande esta saída.")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--sonda":
        asyncio.run(sonda(sys.argv[2]))
    else:
        asyncio.run(diferencial())
