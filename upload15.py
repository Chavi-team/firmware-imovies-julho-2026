#!/usr/bin/env python3
import os
import subprocess
import sys
from hashlib import sha256

# Configuração de Segurança
secret_key = os.getenv('SEED_SECRET', 'CHAVI')
seedMaxRange = 429496729

def get_seed(serial_number, secret_key, seed_number):
    string = f'{serial_number}{secret_key}{seed_number}'
    return int(sha256(string.encode()).hexdigest()[:8], 16) % seedMaxRange

def calculate_hex_commands(mosfet_pin):
    try:
        m_pin = int(mosfet_pin)
        mosfet_bit = m_pin - 3
        pin6_bit = 3
        bits_befc = [0] * 12
        bits_aftc = [0] * 12
        if 0 <= mosfet_bit < 12:
            bits_befc[mosfet_bit] = 1
            bits_aftc[mosfet_bit] = 1
        bits_befc[pin6_bit] = 0
        bits_aftc[pin6_bit] = 1
        befc_hex = f"{int(''.join(map(str, bits_befc[::-1])), 2):03X}"
        aftc_hex = f"{int(''.join(map(str, bits_aftc[::-1])), 2):03X}"
        return befc_hex, aftc_hex
    except: return "000", "000"

def main():
    while True:
        print("\n" + "═"*60 + "\n  PROCESSO DE PRODUÇÃO CHAVI V1.5\n" + "═"*60)
        
        while True:
            ch = input("Canal (CH) [ex: 002]: ").zfill(3)
            fi = input("Firmware ID (FI) [ex: 002857]: ").zfill(6)
            hw_in = input("Hardware Version (apenas números, ex: 15): ")
            mosfet = input("Mosfet Pin (padrão 8): ") or "8"
            
            device_name_ble = f"{ch}FI{fi}"
            device_id_full = f"CH{ch}FI{fi}"
            firmware_folder = f"FI{hw_in}400"
            
            print(f"\n📢 CONFIRMAÇÃO:")
            print(f"   ID Serial: {device_id_full}")
            print(f"   Nome BLE:  {device_name_ble}")
            print(f"   Pasta:     firmware/{firmware_folder}")
            print(f"   Mosfet:    Pino {mosfet}")
            
            if input("\nDados corretos? (Enter=Sim / n=Não): ").lower() == '': 
                break

        # --- GERAÇÃO DE SEEDS ---
        seeds = [get_seed(device_id_full, secret_key, i + 1) for i in range(4)]
        befc, aftc = calculate_hex_commands(mosfet)

        # --- HEADER (SerialNumber1.h) ---
        # Criando a pasta include dentro da pasta do firmware específico
        include_dir = os.path.abspath(f"firmware/{firmware_folder}/include")
        os.makedirs(include_dir, exist_ok=True)
        header_path = os.path.join(include_dir, "SerialNumber1.h")
        
        with open(header_path, "w") as f:
            f.write("// ARQUIVO GERADO AUTOMATICAMENTE\n#ifndef SERIAL_NUMBER_H\n#define SERIAL_NUMBER_H\n\n")
            f.write(f"#define SERIAL \"{device_name_ble}\"\n")
            f.write(f"#define MY_PIN_MOSFET {mosfet}\n\n")
            for i, s in enumerate(seeds):
                f.write(f"#define SEED_{i+1} {s}UL\n")
            f.write(f"\n#define AT_NAME   \"AT+NAME{device_name_ble}\\r\"\n")
            f.write(f"#define AT_BEFC   \"AT+BEFC{befc}\\r\"\n")
            f.write(f"#define AT_AFTC   \"AT+AFTC{aftc}\\r\"\n")
            f.write("#define AT_SHIELD \"AT+SHIELD1\\r\"\n#define AT_BAUD \"AT+BAUD0\\r\"\n#define AT_PWRM \"AT+PWRM1\\r\"\n#endif\n")

        print(f"\n✅ Header SerialNumber1.h preparado em: {include_dir}")

        # --- COMPILAÇÃO E GRAVAÇÃO ---
        try:
            ino_path = os.path.abspath(f"firmware/{firmware_folder}/{firmware_folder}.ino")
            
            if not os.path.exists(ino_path):
                print(f"\n❌ ERRO: Arquivo {ino_path} não encontrado!")
                continue

            print(f"📦 Compilando {firmware_folder}...")
            
            # O SEGREDO ESTÁ AQUI: Passar o caminho da pasta 'include' para o compilador
            subprocess.run([
                "arduino-cli", "compile", 
                "--fqbn", "arduino:avr:uno",
                "--build-path", "bin",
                "--build-property", f"compiler.cpp.extra_flags=\"-I{include_dir}\"",
                ino_path
            ], check=True)

            print("🚀 Gravando via USBASP...")
            hex_path = f"bin/{firmware_folder}.ino.hex"
            
            subprocess.run([
                "avrdude", "-P", "usb", "-c", "usbasp", "-p", "m328pb", "-e",
                "-U", "flash:w:" + hex_path + ":i"
            ], check=True)

            print(f"\n✨ SUCESSO: Placa {device_id_full} finalizada!")
            print("\a") 

        except subprocess.CalledProcessError as e:
            print(f"\n❌ ERRO NA EXECUÇÃO: {e}")
        except Exception as e:
            print(f"\n❌ ERRO INESPERADO: {e}")

        if input("\n--- PRÓXIMA PLACA? (Enter=Sim / s=Sair): ").lower() == 's':
            break

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nSaindo...")