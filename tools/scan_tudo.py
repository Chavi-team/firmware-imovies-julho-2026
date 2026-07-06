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
    print(f">> Conectando em {addr}...")
    notif = []

    def cb(_c, data):
        txt = bytes(data).decode("utf-8", "replace").strip()
        if txt:
            notif.append(txt)
            print(f"   ⟵ {txt!r}")

    async with BleakClient(addr, timeout=15.0) as cli:
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
        for cmd in ("AT+VERS?", "AT+BAUD?", "AT+NAME?", "AT+MODE?", "AT+ROLE?"):
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
