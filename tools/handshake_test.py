#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
handshake_test.py — Replica EXATAMENTE o handshake do app-imoveis e mostra
cada notificação BLE crua (conteúdo + fronteira de pacote + timestamp).

Uso (FECHE a bancada antes — só um programa pode segurar a conexão BLE):
    ./tools/.venv-bancada/bin/python tools/handshake_test.py CH003FI003066 abrir
    ./tools/.venv-bancada/bin/python tools/handshake_test.py CH003FI003066 fechar

Fluxo (igual ao app): conecta -> assina notify FFE1 -> escreve o desafio N
(número, sem terminador) -> espera as 2 notificações de salto -> calcula os
tokens (LFSR taps 31/21/1/0) -> escreve tokenA, tokenB, comando (3 writes,
gap 150ms) -> espera a confirmação (1000/2000+bateria).
"""
import asyncio
import sys
import time
from hashlib import sha256

from bleak import BleakClient, BleakScanner

CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
SEED_MAX = 429496729
DESAFIO = 1523890                # fixo p/ reproduzir
CMD = {"abrir": "2", "fechar": "1"}   # app-imoveis: abrir="2"


def seed(serial, k):
    return int(sha256(f"{serial}CHAVI{k}".encode()).hexdigest()[:8], 16) % SEED_MAX


def lfsr(a, salto):
    a &= 0xFFFFFFFF
    for _ in range(salto):
        y = ((a >> 31) ^ (a >> 21) ^ (a >> 1) ^ a) & 1
        a = ((a << 1) | y) & 0xFFFFFFFF
    return a


async def main():
    serial = sys.argv[1].strip().upper() if len(sys.argv) > 1 else "CH003FI003066"
    acao = sys.argv[2].lower() if len(sys.argv) > 2 else "abrir"
    alvo = serial[2:]
    s1, s2 = seed(serial, 1), seed(serial, 2)
    print(f">> {serial}  seeds: {s1} / {s2}   desafio: {DESAFIO}   ação: {acao} (cmd={CMD[acao]})")

    print(f">> Escaneando por '{alvo}' (10s)... (fechadura precisa estar anunciando)")
    dev = await BleakScanner.find_device_by_name(alvo, timeout=10.0)
    if not dev:
        print("!! Não encontrada. Religue a bateria / feche a bancada e tente de novo.")
        return
    print(f">> Encontrada {dev.address} — conectando...")

    notifs = []
    t0 = [None]

    def cb(_c, data: bytearray):
        el = (time.monotonic() - t0[0]) * 1000 if t0[0] else 0
        notifs.append(bytes(data))
        print(f"   ⟵ NOTIF #{len(notifs)} @ {el:7.0f}ms  {len(data):2d} bytes  {bytes(data)!r}")

    async with BleakClient(dev) as cli:
        await cli.start_notify(CHR_FFE1, cb)
        print(">> Conectado + notify. Aguardando 1,5s (MCU acorda; deve dar 1 bipe)...")
        await asyncio.sleep(1.5)

        t0[0] = time.monotonic()
        print(f"   ⟶ desafio: {DESAFIO!r}")
        await cli.write_gatt_char(CHR_FFE1, str(DESAFIO).encode(), response=False)

        # espera os 2 saltos (timeout do app: 3s no calibrar, 6s no abrir)
        t_lim = time.monotonic() + 5.0
        while time.monotonic() < t_lim and len(notifs) < 2:
            await asyncio.sleep(0.05)

        print(f">> {len(notifs)} notificação(ões) em 5s.")
        if len(notifs) < 2:
            print("!! MENOS DE 2 NOTIFICAÇÕES = exatamente o F05 do app.")
            print("   (se veio 1 com os dois números dentro = saltos COLADOS)")
            return

        # parse por FRONTEIRA DE NOTIFICAÇÃO (como o app): 1 campo por notif
        try:
            vA = int(notifs[0].decode(errors="replace").strip().split()[0])
            vB = int(notifs[1].decode(errors="replace").strip().split()[0])
        except Exception as e:
            print(f"!! notificações não são números limpos: {e}")
            return
        saltoA = vA - DESAFIO - s1
        saltoB = vB - DESAFIO - s2
        print(f">> saltoA = {vA} - N - seed1 = {saltoA}")
        print(f">> saltoB = {vB} - N - seed2 = {saltoB}")
        if not (0 <= saltoA <= 9999 and 0 <= saltoB <= 9999):
            print("!! SALTO FORA DE 0..9999 — é isto que congela/erra o app "
                  "(seed errada ou notificação trocada/colada).")
            return

        tokA, tokB = lfsr(s1, saltoA), lfsr(s2, saltoB)
        print(f">> tokens: {tokA} / {tokB} — enviando tokens + comando '{CMD[acao]}'...")
        n_antes = len(notifs)
        for campo in (str(tokA), str(tokB), CMD[acao]):
            print(f"   ⟶ {campo!r}")
            await cli.write_gatt_char(CHR_FFE1, campo.encode(), response=False)
            await asyncio.sleep(0.15)

        t_lim = time.monotonic() + 6.0
        while time.monotonic() < t_lim and len(notifs) == n_antes:
            await asyncio.sleep(0.05)
        conf = [n.decode(errors="replace").strip() for n in notifs[n_antes:]]
        print(f">> confirmação: {conf if conf else 'NENHUMA (motor girou?)'}")
        print(">> FIM. O motor GIROU?  (confirmação esperada: 1000/2000+bateria, ex. '1004.09')")


if __name__ == "__main__":
    asyncio.run(main())
