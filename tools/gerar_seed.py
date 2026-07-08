#!/usr/bin/env python3
"""
gerar_seed.py — Gera a imagem de EEPROM (seed.bin, 1024 bytes) de UMA fechadura.

Formato IDÊNTICO ao seedGenerator.py legado (mesmos endereços/algoritmo), para
que o firmware novo (chavi_fi) e o antigo leiam as mesmas seeds. A diferença é
que aqui NÃO geramos SerialNumber.h nem recompilamos nada: o .hex é universal e
o serial vai só na EEPROM (endereço 769).

Uso:
    python3 gerar_seed.py CH003FI002465 [saida.bin] [fi10|fi15] [mosfet]

Placa (byte 912 da EEPROM — o firmware universal decide os pinos por ele):
    fi15 (default) = FI 1.5: motor PB1/PB2, WS2812 no PB3
    fi10           = FI 1.0: motor PB2/PB3, LEDs discretos 7/8/9
    Sem o argumento, o canal CH001 assume fi10 (geração 1.0 de campo).

Pino do MOSFET (byte 914 da EEPROM): PIO do módulo BLE que chaveia o gate de
energia — dirige as máscaras BEFC/AFTC e o corte AT+PIOx0 da hibernação em
runtime. 90% das FIs = 8 (default); placas antigas usaram 6 ou 7.

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


def montar_eeprom(serial_number: str, placa: str = "fi15", mosfet: int = 8) -> bytearray:
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

    # placa (912): 1 = FI 1.0 (motor PB2/PB3, LEDs discretos); 0 = FI 1.5
    eeprom[912] = 0x01 if placa == "fi10" else 0x00

    # pino do MOSFET (914): PIO do módulo que chaveia o gate (4..9; 8 = frota)
    if not 4 <= mosfet <= 9:
        raise SystemExit(f"Pino do MOSFET inválido: {mosfet} (use 4..9; 90% das FIs = 8)")
    eeprom[914] = mosfet

    # 916 QUEIMADO (ex-variante sem MOSFET, removida na v2.11.1) — fica 0.

    return eeprom


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    serial = sys.argv[1].strip().upper()
    saida = sys.argv[2] if len(sys.argv) > 2 else "seed.bin"
    if len(sys.argv) > 3:
        placa = sys.argv[3].strip().lower()
        if placa not in ("fi10", "fi15"):
            raise SystemExit(f"Placa inválida: {placa} (use fi10 ou fi15)")
    else:
        # canal CH001 = geração FI 1.0 de campo; demais = FI 1.5
        placa = "fi10" if serial[2:5] == "001" else "fi15"
    mosfet = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    eeprom = montar_eeprom(serial, placa, mosfet)
    with open(saida, "wb") as f:
        f.write(eeprom)

    print(f"Serial : {serial}")
    print(f"Placa  : {'FI 1.0' if placa == 'fi10' else 'FI 1.5'}")
    print(f"MOSFET : PIO{mosfet}")
    for i in range(4):
        print(f"  seed{i + 1} = {get_seed(serial, i + 1)}")
    print(f"Gravado: {saida} ({len(eeprom)} bytes)")


if __name__ == "__main__":
    main()
