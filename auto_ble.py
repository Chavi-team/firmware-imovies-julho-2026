import asyncio
import subprocess
import sys
import time
import os
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

ultima_resposta_versao = ""

def notification_handler(sender, data):
    global ultima_resposta_versao
    try:
        mensagem = data.decode('utf-8', errors='ignore').strip()
        print(f"      📥 Retorno: {mensagem}")
        if "Soft AT" in mensagem:
            ultima_resposta_versao = mensagem
    except:
        pass

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
    print("\n🔭 Procurando dispositivos próximos (Sinal > -65dBm)...")
    # Scan rápido de 3 segundos
    devices_and_adv = await BleakScanner.discover(timeout=3.0, return_adv=True)
    
    if not devices_and_adv:
        print("❌ Nenhum dispositivo encontrado.")
        return None
    
    # Criamos uma lista apenas com quem tem sinal razoável
    # E já ordenamos do maior RSSI para o menor
    candidatos = []
    for device, adv in devices_and_adv.values():
        if adv.rssi > -62:  # Filtro de RSSI
            candidatos.append((device, adv))
    
    # Ordenação: o mais perto (ex: -40dBm) fica no topo, o mais longe (ex: -70dBm) fica embaixo
    candidatos.sort(key=lambda x: x[1].rssi, reverse=True)

    if not candidatos:
        print("❌ Nenhuma placa com sinal forte encontrada.")
        return None

    print(f"\n{'ID':<3} | {'RSSI':<7} | {'ENDEREÇO MAC':<18} | {'NOME'}")
    print("-" * 70)
    
    for i, (device, adv) in enumerate(candidatos):
        nome = device.name if device.name else "[EM BRANCO]"
        rssi = adv.rssi
        # Marca visual para sinal excelente
        alvo = " 🔥 [ALVO PRÓXIMO]" if rssi > -50 else ""
        print(f"{i:<3} | {rssi:<4}dBm | {device.address:<18} | {nome}{alvo}")
    
    escolha = input("\nDigite o ID (o '0' deve ser a placa na sua mão): ")
    if escolha.lower() == 'v': return "voltar"
    
    try:
        idx = int(escolha)
        return candidatos[idx][0]
    except:
        return None

async def configurar_ble(uuid, device_name_ble, befc, aftc, modo_busca=False):
    global ultima_resposta_versao
    ultima_resposta_versao = ""
    
    prefixo = "🔍 Testando" if modo_busca else "--- 🔵 Conectando"
    print(f"{prefixo}: {uuid} ---")
    
    commands = [
        "AT+SHIELD1", "AT+BAUD0", "AT+PWRM1", "AT+POWE4","AT+ADVI5",    
        f"AT+BEFC{befc}", f"AT+AFTC{aftc}", 
        f"AT+NAME{device_name_ble}", "AT+NAME?"
    ]
    
    try:
        tm_out = 6.0 if modo_busca else 12.0
        async with BleakClient(uuid, timeout=tm_out) as client:
            if not client.is_connected: return False
            
            # --- AJUSTE PARA MAC (EVITA POP-UP DE EMPARELHAMENTO) ---
            # Espera 1.2s para a conexão estabilizar antes de pedir notificações
            await asyncio.sleep(1.2)
            
            await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
            
            # No modo busca, enviamos logo o VERS? para não perder tempo
            await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+VERS?", response=False)
            await asyncio.sleep(1.0)
            
            if "Soft AT" not in ultima_resposta_versao:
                await client.stop_notify(CHARACTERISTIC_UUID)
                return False

            if modo_busca:
                print("   ✅ Identificado! Enviando restante das configurações...")

            for cmd in commands:
                # Evita enviar o VERS? duas vezes se já foi validado acima
                if modo_busca and cmd == "AT+VERS?": continue
                
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd.encode('utf-8'), response=False)
                await asyncio.sleep(0.7) 
            
            await asyncio.sleep(0.5)
            await client.stop_notify(CHARACTERISTIC_UUID)
            return "Soft AT" in ultima_resposta_versao
                
    except:
        return False

async def busca_automatica(device_name_ble, befc, aftc):
    print("\n🕵️  Iniciando varredura por sinal forte...")
    scanner_data = await BleakScanner.discover(timeout=4.0, return_adv=True)
    
    if not scanner_data:
        print("❌ Nenhum sinal Bluetooth detectado.")
        return False

    candidatos = []
    for addr in scanner_data:
        device, adv = scanner_data[addr]
        if adv.rssi > -65:
            candidatos.append((device, adv.rssi))
    
    candidatos.sort(key=lambda x: x[1], reverse=True)

    if not candidatos:
        print("❌ Nenhuma placa encontrada com sinal forte o suficiente.")
        return False

    for dev, rssi in candidatos:
        print(f"📡 Testando dispositivo {dev.address} (Sinal: {rssi}dBm)")
        sucesso = await configurar_ble(dev.address, device_name_ble, befc, aftc, modo_busca=True)
        if sucesso:
            return True
            
    return False

async def main():
    if "SEED_SECRET" not in os.environ:
        print("\n⚠️ AVISO: SEED_SECRET não definida!")
    
    while True:
        print("\n" + "═"*60 + "\n  PROCESSO DE GRAVAÇÃO CHAVI\n" + "═"*60)
        
        while True:
            ch = input("Canal (CH): ").zfill(3)
            fi = input("Firmware ID (FI): ").zfill(6)
            hw_in = input("Hardware Version: ")
            hw = f"{hw_in[0]}_{hw_in[1]}" if (len(hw_in)==2 and "_" not in hw_in) else hw_in
            mosfet = input("Mosfet Pin: ")
            
            device_name_ble = f"{ch}FI{fi}"
            device_id_upload = f"CH{ch}FI{fi}"
            firmware_name = f"FI_{hw}_400" if mosfet else f"FI_{hw}"
            
            print(f"\nCONFIRMAÇÃO: BLE: {device_name_ble} | FW: {firmware_name}")
            if input("Dados corretos? (Enter=Sim / n=Não): ").lower() == '': break

        befc_hex, aftc_hex = calculate_hex_commands(mosfet)
        
        target = None
        while target is None:
            target = await scan_and_select()
            if target == "voltar": break
        if target == "voltar": continue

        print(f"\n🚀 [1/2] Gravando Firmware...")
        try:
            subprocess.run(["./upload", device_id_upload, firmware_name, mosfet], check=True)
            print("✅ Upload concluído.")
        except:
            print("❌ Erro no upload físico.")
            if input("Tentar Bluetooth mesmo assim? (s/n): ").lower() != 's': continue

        print("⏳ Aguardando rádio reiniciar...")
        await asyncio.sleep(3)

        while True:
            sucesso = await configurar_ble(target.address, device_name_ble, befc_hex, aftc_hex)
            
            if sucesso:
                print(f"\n🎉 SUCESSO: {device_name_ble} configurado!")
                print("\a")
                break
            else:
                print(f"\n❌ FALHA NA CONEXÃO.")
                opcao = input("[r] RECONECTAR / [s] BUSCA AUTOMÁTICA / [p] PULAR: ").lower()
                
                if opcao == 'r':
                    continue
                elif opcao == 's':
                    if await busca_automatica(device_name_ble, befc_hex, aftc_hex):
                        print(f"\n🎉 SUCESSO via busca automática!")
                        print("\a")
                        break
                    else:
                        print("❌ Placa não encontrada na busca automática.")
                break
        
        input("\n--- PRÓXIMA PLACA? (Pressione Enter) ---")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSaindo...")