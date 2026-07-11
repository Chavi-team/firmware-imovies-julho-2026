import asyncio
import subprocess
import sys
import time
import os
import re
import httpx
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

API_URL = "https://api-imoveis.chavi.com.br/v2/api/admin/devices" 
API_TOKEN = "13464|jhw4S5Vax7WWSBgFz8OieJbY7xETSh4kIVNS4EXEbb85b756"
DEVICE_TYPE_ID = 1

ultima_resposta_versao = ""
mac_capturado_at = None  
mac_hardware_scaneado = None 

def verificar_padrao_valido(mensagem):
    if not mensagem:
        return False
    msg_upper = mensagem.upper() if hasattr(mensagem, 'upper') else str(mensagem).upper()
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
    global mac_capturado_at, mac_hardware_scaneado
    mac_capturado_at = None 
    mac_hardware_scaneado = None
    
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
        alvo = " 🔥 [ALVO]" if rssi > -50 else ""
        print(f"{i:<3} | {rssi:<4}dBm | {device.address:<18} | {retorno[:30]:<30} | {nome}{alvo}")
    
    escolha = input("\nDigite o ID da placa para iniciar (ou 'v' para voltar/buscar novamente): ")
    if escolha.lower() == 'v': return "voltar"
    
    try:
        idx = int(escolha)
        raw_mac = candidatos_validos[idx][2]
        dispositivo_alvo = candidatos_validos[idx][0]
        
        mac_hardware_scaneado = dispositivo_alvo.address
        limpo = raw_mac.replace("OK+ADDR:", "").replace("\r", "").replace("\n", "").strip()
        
        if "ERRO" in limpo.upper() or not limpo:
            mac_capturado_at = None
        else:
            mac_capturado_at = limpo
            
        return dispositivo_alvo
    except:
        print("❌ Seleção inválida.")
        return None

async def configurar_ble(uuid, device_name_ble, befc, aftc, modo_busca=False):
    global ultima_resposta_versao
    ultima_resposta_versao = ""
    
    prefixo = "🔍 Testando" if modo_busca else "--- 🔵 Conectando"
    print(f"{prefixo}: {uuid} ---")
    
    commands = [
        "AT+BAUD0",     
        "AT+PWRM1",
        f"AT+BEFC{befc}", 
        f"AT+AFTC{aftc}", 
        f"AT+NAME{device_name_ble}",
        "AT+SHIELD1"
    ]
    
    try:
        tm_out = 6.0 if modo_busca else 12.0
        async with BleakClient(uuid, timeout=tm_out) as client:
            if not client.is_connected: return False
            
            await asyncio.sleep(1.5)
            await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
            await asyncio.sleep(0.5)
            
            await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+VERS?\r\n", response=False)
            await asyncio.sleep(1.0)
            
            if not verificar_padrao_valido(ultima_resposta_versao):
                await client.stop_notify(CHARACTERISTIC_UUID)
                return False

            if modo_busca:
                print("   ✅ Identificado! Enviando restante das configurações...")

            for cmd in commands:
                if modo_busca and cmd == "AT+VERS?": continue
                print(f"      ⚡ Enviando para Hardware: {cmd}...")
                cmd_bytes = f"{cmd}\r\n".encode('utf-8')
                
                try:
                    await client.write_gatt_char(CHARACTERISTIC_UUID, cmd_bytes, response=True)
                except:
                    await client.write_gatt_char(CHARACTERISTIC_UUID, cmd_bytes, response=False)
                
                await asyncio.sleep(0.8) 
            
            print("🔄 Invocando gravação permanente: AT+RESET...")
            try:
                await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+RESET\r\n", response=False)
                await asyncio.sleep(0.5)
                await client.stop_notify(CHARACTERISTIC_UUID)
            except:
                pass
                
            return True
                
    except Exception as e:
        print(f"❌ Erro interno no fluxo BLE: {e}")
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

async def vincular_organizacao(client, device_id, headers):
    print(f"🔗 [Passo 2] Vinculando dispositivo ID {device_id} à Organização 7...")
    URL_VINCULO = "https://api-imoveis.chavi.com.br/v2/api/admin/devices/assign"
    
    payload_vinculo = {
        "device_id": int(device_id),
        "organization_id": 7
    }
    
    res_vinculo = await client.patch(URL_VINCULO, json=payload_vinculo, headers=headers, timeout=10.0)
    print(f"   📡 [DEBUG] Status do Servidor: {res_vinculo.status_code}")
    print(f"   📡 [DEBUG] Resposta: {res_vinculo.text}")

    if res_vinculo.status_code in [200, 201, 204]:
        print("🎉 SUCESSO TOTAL! Equipamento associado com sucesso à Organização 7 (Chavi | DEMO)!")
        return True
    else:
        print(f"❌ Falha ao vincular à empresa. Status: {res_vinculo.status_code}")
        return False

async def cadastrar_fechadura_api(serial_number, name, mac_bluetooth):
    global mac_hardware_scaneado
    print(f"\n🌐 [Passo 1] Enviando comando para API Chavi ({serial_number})...")
    
    mac_valido = None
    if mac_bluetooth and "ERRO" not in str(mac_bluetooth).upper() and str(mac_bluetooth) != "None":
        mac_valido = str(mac_bluetooth).replace(":", "").upper() 
    elif mac_hardware_scaneado:
        mac_valido = str(mac_hardware_scaneado).replace(":", "").upper()
        
    headers = {
        "Authorization": f"Bearer {API_TOKEN}",
        "Accept": "application/json",
        "Content-Type": "application/json"
    }
    
    payload_cadastro = {
        "serial_number": serial_number, 
        "mac_bluetooth": mac_valido,   
        "name": name,                   
        "version": "1.5",
        "ble_version": "5",            
        "device_type_id": DEVICE_TYPE_ID
    }
    
    try:
        async with httpx.AsyncClient() as client:
            response = await client.post(API_URL, json=payload_cadastro, headers=headers, timeout=10.0)
            
            if response.status_code in [200, 201]:
                dados_retorno = response.json()
                device_id = dados_retorno.get('id') or dados_retorno.get('data', {}).get('id')
                print(f"✅ [API] Fechadura cadastrada globalmente! ID Gerado: {device_id}")
                if device_id:
                    await vincular_organizacao(client, device_id, headers)
                return True
                
            elif response.status_code in [409, 422]:
                print(f"⚠️ [API] Equipamento já existente no banco geral. Localizando ID para atualizar organização...")
                URL_BUSCA = f"https://api-imoveis.chavi.com.br/v2/api/admin/devices?serial_number={serial_number}"
                res_busca = await client.get(URL_BUSCA, headers=headers, timeout=10.0)
                
                if res_busca.status_code == 200:
                    busca_json = res_busca.json()
                    lista_dispositivos = busca_json.get('data', [])
                    
                    if isinstance(lista_dispositivos, list) and len(lista_dispositivos) > 0:
                        device_id = lista_dispositivos[0].get('id')
                    elif isinstance(busca_json, dict) and 'id' in busca_json:
                        device_id = busca_json.get('id')
                    else:
                        device_id = None
                        
                    if device_id:
                        print(f"🔍 ID encontrado no sistema: {device_id}. Prosseguindo para correção de escopo.")
                        await vincular_organizacao(client, device_id, headers)
                        return True
                    else:
                        print("❌ Não foi possível extrair o ID numérico do dispositivo existente através da listagem.")
                        return False
                else:
                    print(f"❌ Falha ao tentar buscar ID do dispositivo existente. Status GET: {res_busca.status_code}")
                    return False
            else:
                print(f"⚠️ [API] Erro desconhecido no cadastro. Status: {response.status_code}")
                return False
    except Exception as e:
        print(f"❌ [API] Falha de comunicação HTTP: {e}")
        return False

async def main():
    global mac_capturado_at
    if "SEED_SECRET" not in os.environ:
        print("\n⚠️ AVISO: SEED_SECRET não definida!")
    
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
                serial_api = f"CH{ch}FI{fi}" 
                nome_formatado_api = f"FI {int(fi)}"
                
                device_id_upload = f"CH{ch}FI{fi}"
                firmware_name = f"FI_{hw}_400" if mosfet else f"FI_{hw}"
                
                print(f"\nCONFIRMAÇÃO: BLE: {device_name_ble} | FW: {firmware_name} | API Serial: {serial_api} | Nome API: {nome_formatado_api}")
                if input("Dados corretos? (Enter=Sim / n=Não): ").lower() == '': break
            
            befc_hex, aftc_hex = calculate_hex_commands(mosfet)

        print("\n📋 Escolha o modo de operação para esta placa:")
        print("   [1] Fazer TUDO (Firmware + BLE + Cadastro API)")
        print("   [2] Apenas Firmware")
        print("   [3] Apenas Bluetooth (Manda Comandos AT)")
        print("   [4] Apenas Cadastrar na API")
        modo = input("Digite a opção desejada (Padrão = 1): ").strip()
        if modo not in ['1', '2', '3', '4']:
            modo = '1'

        # CORREÇÃO: Opção 4 isolada no topo para evitar qualquer scan
        if modo == '4':
            await cadastrar_fechadura_api(serial_number=serial_api, name=nome_formatado_api, mac_bluetooth=None)
            input("\n--- PRÓXIMA PLACA? (Pressione Enter) ---")
            manter_dados = False
            continue

        # CORREÇÃO: Opção 2 isolada aqui. Roda o firmware físico e encerra o ciclo imediatamente!
        if modo == '2':
            print(f"\n🚀 [2] Gravando Apenas Firmware...")
            try:
                subprocess.run(["./upload", device_id_upload, firmware_name, mosfet], check=True)
                print("✅ Upload de Firmware concluído com sucesso!")
            except Exception as e:
                print(f"❌ Erro no upload físico: {e}")
            
            input("\n--- PRÓXIMA PLACA? (Pressione Enter) ---")
            manter_dados = False
            continue

        # Fluxos das opções [1] e [3] que obrigatoriamente precisam de comunicação BLE
        target = await scan_and_select()
        if target == "voltar" or target is None:
            opcao = input("\n[Enter/s] Buscar Novamente / [n] Mudar dados do Firmware: ").lower()
            manter_dados = False if opcao == 'n' else True
            continue

        manter_dados = False

        # Aqui cai apenas se for a opção [1], gravando o firmware antes de parametrizar o BLE
        if modo == '1':
            print(f"\n🚀 [1] Gravando Firmware...")
            try:
                subprocess.run(["./upload", device_id_upload, firmware_name, mosfet], check=True)
                print("✅ Upload concluído.")
            except:
                print("❌ Erro no upload físico.")
                if input("Tentar Bluetooth mesmo assim? (s/n): ").lower() != 's': continue

            print("⏳ Aguardando rádio reiniciar...")
            await asyncio.sleep(3)

        # Processamento BLE comum para as opções [1] e [3]
        if modo in ['1', '3']:
            while True:
                sucesso = await configurar_ble(target.address, device_name_ble, befc_hex, aftc_hex)
                
                if sucesso:
                    print(f"\n🎉 SUCESSO: {device_name_ble} configurado via BLE!")
                    print("\a")
                    
                    if modo == '1':
                        await cadastrar_fechadura_api(
                            serial_number=serial_api, 
                            name=nome_formatado_api, 
                            mac_bluetooth=mac_capturado_at
                        )
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
                            
                            if modo == '1':
                                await cadastrar_fechadura_api(
                                    serial_number=serial_api, 
                                    name=nome_formatado_api, 
                                    mac_bluetooth=mac_capturado_at
                                )
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