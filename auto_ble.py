import asyncio
import subprocess
import sys
import time
import os
import re
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

ultima_resposta_versao = ""

def verificar_padrao_valido(mensagem):
    if not mensagem:
        return False
    msg_upper = mensagem.upper()
    if "SOFT AT" in msg_upper or "CHAVIFI" in msg_upper:
        return True
    if re.search(r'\d+FI\d+', msg_upper):
        return True
    return False

def notification_handler(sender, data):
    global ultima_resposta_versao
    try:
        mensagem = data.decode('utf-8', errors='ignore').strip()
        print(f"      📥 Retorno: {mensagem}")
        if verificar_padrao_valido(mensagem):
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

async def testar_e_filtrar_dispositivo(device, adv, candidatos_validos):
    if adv.rssi < -75: 
        return

    respostas_locais = []
    def filtro_callback(sender, data):
        try:
            msg = data.decode('utf-8', errors='ignore').strip()
            if msg:
                respostas_locais.append(msg)
        except:
            pass

    try:
        async with BleakClient(device.address, timeout=5.0) as client:
            if client.is_connected:
                await asyncio.sleep(1.0)
                await client.start_notify(CHARACTERISTIC_UUID, filtro_callback)

                # ===== PRIMEIRO ESTE COMANDO =====
                await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+ADDR?\r\n", response=False)
                await asyncio.sleep(0.8)
                
                if not respostas_locais:
                    await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+ADDR?", response=False)
                    await asyncio.sleep(0.8)

                await client.stop_notify(CHARACTERISTIC_UUID)

                if respostas_locais:
                    retorno_texto = " | ".join(respostas_locais)
                    candidatos_validos.append((device, adv, retorno_texto))
    except:
        pass

async def scan_and_select():
    print("\n🔭 Procurando dispositivos (Filtro: Todos que responderem a AT+ADDR?)...")
    devices_and_adv = await BleakScanner.discover(timeout=4.0, return_adv=True)
    
    if not devices_and_adv:
        print("❌ Nenhum dispositivo Bluetooth detectado.")
        return None
    
    candidatos_validos = []
    tarefas = []

    for device, adv in devices_and_adv.values():
        tarefas.append(testar_e_filtrar_dispositivo(device, adv, candidatos_validos))
    
    await asyncio.gather(*tarefas)

    if not candidatos_validos:
        print("❌ Nenhuma placa respondeu ao comando AT+ADDR?.")
        return None
    
    candidatos_validos.sort(key=lambda x: x[1].rssi, reverse=True)

    print(f"\n{'ID':<3} | {'RSSI':<7} | {'ENDEREÇO MAC':<18} | {'RETORNO AT+ADDR?':<30} | {'NOME BLE'}")
    print("-" * 115)
    
    for i, (device, adv, retorno) in enumerate(candidatos_validos):
        nome = device.name if device.name else "[EM BRANCO]"
        rssi = adv.rssi
        alvo = " 🔥 [ALVO]" if rssi > -30 else ""
        print(f"{i:<3} | {rssi:<4}dBm | {device.address:<18} | {retorno[:30]:<30} | {nome}{alvo}")
    
    escolha = input("\nDigite o ID da placa para iniciar o upload (ou 'v' para voltar/buscar novamente): ")
    if escolha.lower() == 'v': return "voltar"
    
    try:
        idx = int(escolha)
        return candidatos_validos[idx][0]
    except:
        print("❌ Seleção inválida.")
        return None

async def configurar_ble(uuid, device_name_ble, befc, aftc, modo_busca=False):
    global ultima_resposta_versao
    ultima_resposta_versao = ""
    
    prefixo = "🔍 Testando" if modo_busca else "--- 🔵 Conectando"
    print(f"{prefixo}: {uuid} ---")
    
    commands = [
        "AT+SHIELD1", "AT+BAUD2", "AT+PWRM7",
        "AT+MODE2", "AT+TCON000000",
        f"AT+BEFC{befc}", f"AT+AFTC{aftc}", 
        f"AT+NAME{device_name_ble}", "AT+RESET"
    ]
    
    try:
        tm_out = 6.0 if modo_busca else 12.0
        async with BleakClient(uuid, timeout=tm_out) as client:
            if not client.is_connected: return False
            
            await asyncio.sleep(1.2)
            await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
            
            await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+VERS?", response=False)
            await asyncio.sleep(1.0)
            
            if not verificar_padrao_valido(ultima_resposta_versao):
                await client.stop_notify(CHARACTERISTIC_UUID)
                return False

            if modo_busca:
                print("   ✅ Identificado! Enviando restante das configurações...")

            for cmd in commands:
                if modo_busca and cmd == "AT+VERS?": continue
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd.encode('utf-8'), response=False)
                await asyncio.sleep(0.7) 
            
            await asyncio.sleep(0.5)
            await client.stop_notify(CHARACTERISTIC_UUID)
            return verificar_padrao_valido(ultima_resposta_versao)
                
    except:
        return False

async def busca_automatica(device_name_ble, befc, aftc):
    print("\n🕵️  Iniciando varredura automática por sinal forte...")
    scanner_data = await BleakScanner.discover(timeout=4.0, return_adv=True)
    
    if not scanner_data:
        print("❌ Nenhum sinal Bluetooth detectado.")
        return False

    candidatos = []
    for addr in scanner_data:
        device, adv = scanner_data[addr]
        if adv.rssi > -90:
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
    
    # Variáveis persistentes para não precisar redigitar
    manter_dados = False
    ch, fi, hw_in, mosfet = "", "", "", ""
    device_name_ble, device_id_upload, firmware_name = "", "", ""
    befc_hex, aftc_hex = "000", "000"

    while True:
        if not manter_dados:
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

        # Roda o scan de seleção
        target = await scan_and_select()
        
        # Se você digitou 'v' ou deu erro no scan
        if target == "voltar" or target is None:
            opcao = input("\n[Enter/s] Buscar Novamente / [n] Mudar dados do Firmware: ").lower()
            if opcao == 'n':
                manter_dados = False  # Vai pedir os inputs de CH, FI de novo
            else:
                manter_dados = True   # Pula direto pro scan mantendo o que já foi digitado
            continue

        # Se passou e achou o target, reinicia o fluxo de dados para a PRÓXIMA placa após concluir
        manter_dados = False

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