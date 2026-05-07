import asyncio
import subprocess
import sys
import time
import os
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

def notification_handler(sender, data):
    mensagem = data.decode('utf-8', errors='ignore').strip()
    print(f"   📥 Retorno: {mensagem}")

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

async def scan_and_select():
    print("\n🔭 Escaneando... (Dispositivos próximos aparecerão com <--- ALVO?)")
    devices_and_adv = await BleakScanner.discover(timeout=4.0, return_adv=True)
    if not devices_and_adv:
        print("❌ Nenhum dispositivo encontrado.")
        return None
    
    list_devices = list(devices_and_adv.values())
    print(f"\n{'ID':<3} | {'RSSI':<7} | {'ENDEREÇO MAC':<18} | {'NOME'}")
    print("-" * 65)
    
    for i, (device, adv) in enumerate(list_devices):
        nome = device.name if device.name else "Desconhecido"
        rssi = adv.rssi
        # Se o sinal for mais forte que -55, grandes chances de ser a placa na sua frente
        alvo = " <--- ALVO? 🔥" if rssi > -55 else ""
        print(f"{i:<3} | {rssi:<4}dBm | {device.address:<18} | {nome}{alvo}")
    
    escolha = input("\nDigite o ID ou 'r' para repetir / 'v' para voltar: ")
    if escolha.lower() == 'r': return None
    if escolha.lower() == 'v': return "voltar"
    try:
        return list_devices[int(escolha)][0]
    except: return None

async def configurar_ble(uuid, device_name_ble, befc, aftc):
    print(f"\n--- 🔵 Conectando ao Bluetooth: {device_name_ble} ---")
    commands = [
        "AT+SHIELD1", "AT+BAUD0", "AT+PWRM1", 
        f"AT+BEFC{befc}", f"AT+AFTC{aftc}", 
        f"AT+NAME{device_name_ble}", "AT+VERS?"
    ]
    try:
        async with BleakClient(uuid) as client:
            if not client.is_connected: return False
            print("🔗 Conectado! Ativando notificações...")
            await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
            for cmd in commands:
                print(f"📤 Enviando: {cmd}")
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd.encode('utf-8'), response=False)
                await asyncio.sleep(0.6) 
            await client.stop_notify(CHARACTERISTIC_UUID)
            return True
    except Exception as e:
        print(f"❌ Erro BLE: {e}")
        return False

async def main():
    if "SEED_SECRET" not in os.environ:
        print("\n⚠️ AVISO: SEED_SECRET não configurada!")
    
    while True:
        print("\n" + "═"*50)
        print("  PROCESSO DE GRAVAÇÃO CHAVI")
        print("═"*50)
        
        while True:
            ch = input("Canal (CH): ").zfill(3)
            fi = input("Firmware ID (FI): ").zfill(6)
            hw_in = input("Hardware Version (ex: 10 ou 15): ")
            hw = f"{hw_in[0]}_{hw_in[1]}" if (len(hw_in)==2 and "_" not in hw_in) else hw_in
            mosfet = input("Mosfet Pin (3-14): ")
            
            device_name_ble = f"{ch}FI{fi}"
            device_id_upload = f"CH{ch}FI{fi}"
            firmware_name = f"FI_{hw}_400" if mosfet else f"FI_{hw}"
            
            print(f"\nCONFIRMAÇÃO:")
            print(f"📡 NOME BLE:  {device_name_ble}")
            print(f"📂 FIRMWARE:  {firmware_name}")
            print(f"🆔 UPLOAD ID: {device_id_upload}")
            if input("Correto? (Enter=OK / n=Corrigir): ").lower() == '': break

        befc_hex, aftc_hex = calculate_hex_commands(mosfet)
        target = None
        while target is None:
            target = await scan_and_select()
            if target == "voltar": break
        if target == "voltar": continue

        print(f"\n🚀 Executando ./upload {device_id_upload}...")
        try:
            subprocess.run(["./upload", device_id_upload, firmware_name, mosfet], check=True)
            print("✅ Upload concluído.")
        except:
            print("❌ Erro no upload.")
            if input("Tentar Bluetooth? (s/n): ") != 's': continue

        if await configurar_ble(target.address, device_name_ble, befc_hex, aftc_hex):
            print(f"\n🎉 SUCESSO: {device_name_ble} configurado!")
            print("\a")
        
        input("\n--- PRÓXIMA PLACA? (Pressione Enter) ---")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSaindo...")