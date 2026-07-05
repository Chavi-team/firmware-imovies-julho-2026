#!/usr/bin/env python3
"""
gerar_seed.py — Gera a imagem de EEPROM (seed.bin, 1024 bytes) de UMA fechadura.

Formato IDÊNTICO ao seedGenerator.py legado (mesmos endereços/algoritmo), para
que o firmware novo (chavi_fi) e o antigo leiam as mesmas seeds. A diferença é
que aqui NÃO geramos SerialNumber.h nem recompilamos nada: o .hex é universal e
o serial vai só na EEPROM (endereço 769).

Uso:
    python3 gerar_seed.py CH003FI002465 [saida.bin]

Seed (determinística do serial, igual backend DeviceSeedHelper.php):
    seed_k = int(sha256(serial + SECRET + k)[:8], 16) % 429496729   (k = 1..4)
    SECRET default = "CHAVI" (env SEED_SECRET sobrescreve)
"""
import os
import sys
from hashlib import sha256

SEED_MAX_RANGE = 429496729  # bug histórico em produção — NÃO alterar
SECRET = os.getenv("SEED_SECRET", "CHAVI")


def get_seed(serial_number: str, seed_number: int) -> int:
    s = f"{serial_number}{SECRET}{seed_number}"
    return int(sha256(s.encode()).hexdigest()[:8], 16) % SEED_MAX_RANGE


def montar_eeprom(serial_number: str) -> bytearray:
    if not serial_number.startswith("CH"):
        raise SystemExit(f"Serial deve começar com 'CH': {serial_number}")

    eeprom = bytearray(1024)

    # flags de estado / comportamento (mesmos bytes do seedGenerator.py legado)
    eeprom[1]   = 0x01   # setupSeedOk  -> pula modo setup
    eeprom[101] = 0x01   # warning sound
    eeprom[102] = 0x01   # light warning
    eeprom[104] = 0x01   # button
    eeprom[105] = 0x01   # auto close (nível default)
    eeprom[150] = 0x01   # setupProductionOk

    # serial sem "CH", 11 chars, em 769..779
    serial_11 = serial_number[2:]
    eeprom[769:769 + len(serial_11)] = serial_11.encode()

    # 4 seeds em 5,15,25,35 (u32 little-endian)
    for i in range(4):
        addr = 10 * i + 5
        seed = get_seed(serial_number, i + 1)
        eeprom[addr:addr + 4] = seed.to_bytes(4, "little")

    # telemetria NOVA (>=900): layout=1, contadores zerados
    eeprom[900] = 0x01

    return eeprom


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    serial = sys.argv[1].strip().upper()
    saida = sys.argv[2] if len(sys.argv) > 2 else "seed.bin"

    eeprom = montar_eeprom(serial)
    with open(saida, "wb") as f:
        f.write(eeprom)

    print(f"Serial : {serial}")
    for i in range(4):
        print(f"  seed{i + 1} = {get_seed(serial, i + 1)}")
    print(f"Gravado: {saida} ({len(eeprom)} bytes)")


if __name__ == "__main__":
    main()
