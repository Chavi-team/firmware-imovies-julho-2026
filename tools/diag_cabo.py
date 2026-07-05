#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
diag_cabo.py — Diagnóstico CRU do cabo USB-TTL <-> fechadura.

Abre a porta, manda PING de 1 em 1 segundo e mostra TODO byte que chegar
(texto + hex). Use para descobrir onde o cabo está errado SEM depender da GUI.

Uso:
    ./.venv-bancada/bin/python diag_cabo.py            # acha a porta sozinho
    ./.venv-bancada/bin/python diag_cabo.py /dev/cu.usbserial-210

Enquanto ele roda: DESLIGUE e LIGUE a bateria da fechadura. Na hora do boot a
fechadura abre a janela de bancada por 20s — o PING vai entrar e ela deve:
  - dar 2 BIPES agudos (entrou em modo bancada), e
  - mandar 'BANCADA-PRONTA' e 'PONG' de volta (aparece aqui).

Leitura do resultado:
  • aparece BANCADA-PRONTA/PONG  -> CABO 100% OK (o problema era timing na GUI).
  • ouve 2 bipes mas NADA aparece -> falta o retorno: o fio BRANCO (RX do
      adaptador) não está no TX da placa, ou está sem contato.
  • NÃO ouve bipes e nada aparece -> o comando não chega: o fio VERDE (TX do
      adaptador) não está no RX da placa (inverta BRANCO<->VERDE e tente).
  • chega uma enxurrada de 0x00 sem parar -> pad errado / linha presa em 0V.
"""
import sys
import time

try:
    import serial
    import serial.tools.list_ports as lp
except ImportError:
    print("Rode pelo venv:  ./.venv-bancada/bin/python diag_cabo.py", file=sys.stderr)
    sys.exit(1)

BAUD = 2400


def achar_porta():
    chaves = ("usbserial", "usbmodem", "cp210", "ch340", "ftdi", "pl2303", "wch", "slab")
    for p in lp.comports():
        hay = f"{p.device} {p.description} {p.manufacturer}".lower()
        if any(k in hay for k in chaves):
            return p.device
    return None


def main():
    porta = sys.argv[1] if len(sys.argv) > 1 else achar_porta()
    if not porta:
        print("Nenhuma porta USB-TTL encontrada. Portas disponíveis:")
        for p in lp.comports():
            print("   ", p.device, "—", p.description)
        sys.exit(1)

    print(f">> Abrindo {porta} a {BAUD} baud")
    print(">> DESLIGUE e LIGUE a bateria da fechadura AGORA (janela de 20s).")
    print(">> Ctrl+C para sair.\n")

    with serial.Serial(porta, BAUD, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ultimo_ping = 0
        while True:
            agora = time.time()
            if agora - ultimo_ping >= 1.0:
                ser.write(b"PING\n")
                ser.flush()
                print("  ⟶ PING")
                ultimo_ping = agora
            dado = ser.read(128)
            if dado:
                txt = dado.decode("utf-8", errors="replace").strip()
                hexs = " ".join(f"{b:02X}" for b in dado)
                print(f"  ⟵ texto={txt!r}   hex=[{hexs}]")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n>> fim")
