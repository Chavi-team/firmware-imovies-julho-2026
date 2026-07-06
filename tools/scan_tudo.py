#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scan_tudo.py — Lista TUDO que está anunciando por BLE (nome, MAC/UUID, RSSI e
serviços), por 15s. Para achar módulos que voltaram ao NOME DE FÁBRICA
(JDY-xx / HMSoft / BT05 / MLT / sem nome) depois de um AT+RENEW — a bancada
normal só procura pelo serial.

Uso:  ./tools/.venv-bancada/bin/python tools/scan_tudo.py
Dica: rode 2x — uma com a fechadura LIGADA e outra DESLIGADA — o que aparecer
e sumir é ela.
"""
import asyncio

from bleak import BleakScanner


async def main():
    print(">> Escaneando TUDO por 15s...")
    devs = await BleakScanner.discover(timeout=15.0, return_adv=True)
    linhas = []
    for addr, (dev, adv) in devs.items():
        nome = adv.local_name or dev.name or "(sem nome)"
        uuids = ",".join(u.replace("-0000-1000-8000-00805f9b34fb", "").replace("0000", "")
                         for u in (adv.service_uuids or [])) or "-"
        linhas.append((adv.rssi or -199, f"{adv.rssi:>5}  {nome:<24}  svc:[{uuids}]  {dev.address}"))
    for _, l in sorted(linhas, reverse=True):
        print(l)
    print(f">> {len(linhas)} dispositivos. Procure por JDY/HMSoft/BT05/MLT/'(sem nome)' "
          "com svc ffe0 e RSSI forte (perto da bancada).")


if __name__ == "__main__":
    asyncio.run(main())
