#!/usr/bin/env python3
from hashlib import sha256
from os import getenv
import sys
import regex as re

seedMaxRange = 429496729
secret_key = getenv('SEED_SECRET')

def get_seed(serial_number, secret_key, seed_number):
    string = f'{serial_number}{secret_key}{seed_number}'

    digest = sha256()
    digest.update(string.encode())

    return int(digest.hexdigest()[:8], 16) % seedMaxRange

def create_header(serial_number):
    with open("templates/SerialNumber.h", "r") as template_file:
        template = template_file.read()

    template = template.replace("<(SERIAL_NUMBER)>", serial_number[2:])

    with open("firmware/include/SerialNumber.h", "w") as header:
        header.write(template)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Script para geração de seeds baseado em número de série.")
        print("\tO formato do número de série dever ser `CH000FI000000`.")

    serial_number = sys.argv[1]

    regex = r'^CH[0-9]{3}FI[0-9]{6}$'
    if not re.match(regex, serial_number):
        print("O número de série informado não tem o formato correto.")
        sys.exit(1)

    if not secret_key:
        print("SEED_SECRET não foi definido.")

    seeds = [0] * 4

    for i in range(4):
        seeds[i] = get_seed(serial_number, secret_key, i + 1)

    print(seeds)

    eeprom = bytearray(1024)

    eeprom[1] = 0x01  # Seta a flag setupSeedOk como 0x01 para que a FI nao entre em modo de configuracao
    eeprom[101] = 0x01
    eeprom[102] = 0x01
    eeprom[104] = 0x01
    eeprom[105] = 0x01
    eeprom[150] = 0x01
    eeprom[769:769+11] = serial_number[2:].encode()  # Salva numero de serie na EEPROM

    for i in range(len(seeds)):
        address = 10 * i + 5
        eeprom[address:address + 4] = seeds[i].to_bytes(4, byteorder='little')

    with open("seed.bin", "wb") as binary_file:
        binary_file.write(eeprom)

    create_header(serial_number)
    
    sys.exit(0)
