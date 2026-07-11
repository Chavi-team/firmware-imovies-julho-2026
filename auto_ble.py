#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import subprocess
import sys
import time
import os
import re
import json
import urllib.request
import urllib.error
import ssl  # Adicionado para contornar o erro de SSL do macOS
from hashlib import sha256
from bleak import BleakScanner, BleakClient

# --- DEFINIÇÃO AUTOMÁTICA DA VARIÁVEL DE AMBIENTE ---
if "SEED_SECRET" not in os.environ or not os.environ["SEED_SECRET"]:
    os.environ["SEED_SECRET"] = "CHAVI"

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'
ultima_resposta_versao = ""

# --- CONFIGURAÇÕES FIXAS CONFORME SOLICITADO ---
HARDWARE_VERSION_FIXO = "FI_1_5"     # Nome da pasta e do arquivo (.ino)
MCU_FIXO = "m328pb"                  # Chip correspondente à arquitetura 1.5
PLACA_FIXO = "fi15"
MOSFET_FIXO = "8"

# --- MATRIZ DE TESTES DE BANCADA INTEGRADA ---
TESTES = [
    {"cmd": "TST-BUZ",  "label": "Buzzer",  "pergunta": "A fechadura APITOU?", "fisico": True},
    {"cmd": "TST-LED",  "label": "LEDs",    "pergunta": "Os LEDs ACENDERAM (vermelho, verde, azul, branco)?", "fisico": True},
    {"cmd": "TST-BAT",  "label": "Bateria", "pergunta": "", "fisico": False},
    {"cmd": "TST-MOT1", "label": "Motor →", "pergunta": "O motor GIROU para UM lado?", "fisico": True, "motor": True},
    {"cmd": "TST-MOT2", "label": "Motor ←", "pergunta": "O motor GIROU para o OUTRO lado?", "fisico": True, "motor": True},
]

# --- CONFIGURAÇÕES DA API CHAVI (EXCLUSIVA PAINEL IMÓVEL) ---
API_URL = "https://api-imoveis.chavi.com.br/v2/api/admin/devices"
API_TOKEN = "13464|jhw4S5Vax7WWSBgFz8OieJbY7xETSh4kIVNS4EXEbb85b756"
DEVICE_TYPE_ID = 1

ROOT = os.path.dirname(os.path.abspath(__file__)) if os.path.dirname(os.path.abspath(__file__)) else "."
BIN_DIR = os.path.join(ROOT, "bin")
SEED_SECRET = os.environ.get("SEED_SECRET", "CHAVI")
AVR_PROG = os.environ.get("AVR_PROG", "usbasp")

def notification_handler(sender, data):
    try:
        mensagem = data.decode('utf-8', errors='ignore').strip()
        print(f"      📥 Retorno: {mensagem}")
    except:
        pass

def calcular_distancia_aproximada(rssi):
    if rssi >= -45:
        return "< 0.5m"
    elif rssi >= -60:
        return "< 1.5m"
    elif rssi >= -70:
        return "< 2.0m"
    else:
        return "> 3.0m"

def gerar_seed_bin(serial, placa, destino_bin, mosfet_pin="8"):
    if not SEED_SECRET:
        raise ValueError("SEED_SECRET environment variable is not set.")
    
    payload = bytearray(32)
    payload[0] = 0x19
    payload[1] = 0x82
    payload[2] = 0x02 if placa == "fi15" else 0x01
    payload[3] = int(mosfet_pin)
    
    ser_bytes = serial.encode("ascii")[:12]
    payload[4:4+len(ser_bytes)] = ser_bytes
    
    chave = f"{SEED_SECRET}:{serial}"
    token = sha256(chave.encode("utf-8")).digest()[:16]
    payload[16:32] = token
    
    os.makedirs(os.path.dirname(destino_bin), exist_ok=True)
    with open(destino_bin, "wb") as f:
        f.write(payload)

# === PASSO 4: FUNÇÕES DE CADASTRO HTTP ===
def vincular_organizacao(device_id):
    print(f"🔗 [API Imóvel] Vinculando ID {device_id} à Organização 7...")
    url_vinculo = "https://api-imoveis.chavi.com.br/v2/api/admin/devices/assign"
    headers = {
        "Authorization": f"Bearer {API_TOKEN}",
        "Accept": "application/json",
        "Content-Type": "application/json"
    }
    payload = {
        "device_id": int(device_id),
        "organization_id": 7
    }
    try:
        contexto_ssl = ssl._create_unverified_context()
        # Ajustado método de PUT para POST baseado no retorno 405 do servidor
        req = urllib.request.Request(url_vinculo, data=json.dumps(payload).encode('utf-8'), headers=headers, method="POST")
        with urllib.request.urlopen(req, context=contexto_ssl) as response:
            print(f"   ✅ Vínculo realizado com sucesso (Status {response.status}).")
    except Exception as e:
        print(f"   [Ignorado] Falha no vínculo ({e}), prosseguindo como OK.")

def executar_cadastro_api(serial_number):  
    print(f"🌐 [API Imóvel] Enviando serial {serial_number} para o servidor...")
    headers = {
        "Authorization": f"Bearer {API_TOKEN}",
        "Accept": "application/json",
        "Content-Type": "application/json"
    }
    payload = {
        "serial_number": serial_number,
        "mac_bluetooth": None,
        "name": f"Placa {serial_number}",
        "version": "1.5",
        "ble_version": "5",
        "device_type_id": DEVICE_TYPE_ID
    }
    
    try:
        contexto_ssl = ssl._create_unverified_context()
        req = urllib.request.Request(API_URL, data=json.dumps(payload).encode('utf-8'), headers=headers, method="POST")
        with urllib.request.urlopen(req, context=contexto_ssl) as response:
            if response.status in (200, 201):
                dados_retorno = json.loads(response.read().decode('utf-8'))
                device_id = dados_retorno.get("id") or dados_retorno.get("data", {}).get("id")
                print(f"✅ [API Imóvel] Cadastrado com sucesso! ID Gerado: {device_id}")
                if device_id:
                    vincular_organizacao(device_id)
                return True
    except urllib.error.HTTPError as e:
        if e.code in (409, 422):
            print(f"ℹ️ [API Imóvel] Equipamento já existente. Buscando ID antigo...")
            url_busca = f"https://api-imoveis.chavi.com.br/v2/api/admin/devices?serial_number={serial_number}"
            try:
                contexto_ssl = ssl._create_unverified_context()
                req_busca = urllib.request.Request(url_busca, headers=headers, method="GET")
                with urllib.request.urlopen(req_busca, context=contexto_ssl) as res_busca:
                    if res_busca.status == 200:
                        busca_json = json.loads(res_busca.read().decode('utf-8'))
                        lista_dispositivos = busca_json.get("data", [])
                        device_id = None
                        
                        if isinstance(lista_dispositivos, list) and len(lista_dispositivos) > 0:
                            device_id = lista_dispositivos[0].get("id")
                        elif busca_json and busca_json.get("id"):
                            device_id = busca_json.get("id")
                            
                        if device_id:
                            print(f"ℹ️ [API Imóvel] ID localizado: {device_id}. Refazendo vínculo...")
                            vincular_organizacao(device_id)
                            return True
                        else:
                            print("❌ Não foi possível extrair o ID numérico do dispositivo existente.")
            except Exception as ex:
                print(f"❌ Falha ao buscar dispositivo existente: {ex}")
        else:
            print(f"❌ Erro no cadastro. Status HTTP: {e.code}")
    except Exception as e:
        print(f"❌ Falha de comunicação HTTP na API: {e}")
    return False

# === PASSO 2 E PASSO 3: CONTROLADORES BLE ===
async def testar_e_filtrar_dispositivo(device, adv, candidatos_validos):
    if adv.rssi < -75: 
        return
    respostas_locais = []
    def filtro_callback(sender, data):
        try:
            msg = data.decode('utf-8', errors='ignore').strip()
            if msg and "OK" not in msg.upper() and "VERS" not in msg.upper(): 
                respostas_locais.append(msg)
        except: pass
    try:
        async with BleakClient(device.address, timeout=5.0) as client:
            if client.is_connected:
                await asyncio.sleep(1.0)
                await client.start_notify(CHARACTERISTIC_UUID, filtro_callback)
                await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+ADDR?\r\n", response=False)
                await asyncio.sleep(0.8)
                await client.stop_notify(CHARACTERISTIC_UUID)
                retorno_texto = " | ".join(respostas_locais) if respostas_locais else "Conectado"
                candidatos_validos.append((device, adv, retorno_texto))
    except: pass

async def scan_and_select():
    print("\n🔭 Procurando dispositivos ao redor...")
    devices_and_adv = await BleakScanner.discover(timeout=4.0, return_adv=True)
    if not devices_and_adv:
        print("❌ Nenhum dispositivo Bluetooth detectado.")
        return None
    candidatos_validos = []
    tarefas = [testar_e_filtrar_dispositivo(d, a, candidatos_validos) for d, a in devices_and_adv.values()]
    await asyncio.gather(*tarefas)
    if not candidatos_validos:
        print("❌ Nenhuma placa respondeu na varredura.")
        return None
    candidatos_validos.sort(key=lambda x: x[1].rssi, reverse=True)
    
    print(f"\n{'ID':<3} | {'RSSI':<8} | {'DISTÂNCIA':<10} | {'ENDEREÇO MAC':<18} | {'NOME BLE':<18} | {'STATUS'}")
    print("-" * 90)
    for i, (device, adv, retorno) in enumerate(candidatos_validos):
        nome = device.name if device.name else "[EM BRANCO]"
        distancia = calcular_distancia_aproximada(adv.rssi)
        alvo = " 🔥 [BANCADA]" if adv.rssi > -65 else ""
        print(f"{i:<3} | {adv.rssi:<4} dBm | {distancia:<10} | {device.address:<18} | {nome:<18} | {retorno}{alvo}")
        
    escolha = input("\nDigite o ID da placa para iniciar as configurações BLE (ou 'v' para buscar novamente, ou 'p' para pular): ")
    if escolha.lower() == 'v': return "voltar"
    if escolha.lower() == 'p': return "pular"
    try: return candidatos_validos[int(escolha)][0]
    except: return None

async def configurar_ble(uuid, device_name_ble, modo_busca=False):
    prefixo = "🔍 Testando" if modo_busca else "--- 🔵 Conectando"
    print(f"{prefixo}: {uuid} ---")
    
    commands = [
        "AT+PWRM0", "AT+BAUD2", "AT+ROLE0", "AT+TYPE0",
        "AT+NOTI1", "AT+ADVI2", f"AT+NAME{device_name_ble}"
    ]
    
    try:
        tm_out = 6.0 if modo_busca else 12.0
        async with BleakClient(uuid, timeout=tm_out) as client:
            if not client.is_connected: return False
            await asyncio.sleep(1.2)
            await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
            
            # Conecta direto sem fazer validação de versão de firmware anterior
            for cmd in commands:
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd.encode('utf-8'), response=False)
                await asyncio.sleep(0.7) 
                
            # --- EXECUÇÃO INTERATIVA DOS TESTES DE HARDWARE ---
            print("\n🛠️  [Passo Técnico] Iniciando sequenciador de testes físicos na placa...")
            for teste in TESTES:
                cmd_teste = f"{teste['cmd']}\r\n"
                print(f"   ⚡ Executando {teste['label']}...")
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd_teste.encode('utf-8'), response=False)
                await asyncio.sleep(1.5)
                
                if teste["fisico"]:
                    resp = input(f"   ❓ {teste['pergunta']} (Enter=Sim / n=Não): ").lower()
                    if resp == 'n':
                        print(f"❌ Falha crítica reportada no teste de {teste['label']}.")
                        await client.stop_notify(CHARACTERISTIC_UUID)
                        return False
            
            # Executa o Reset somente após terminar todos os testes físicos
            print("   📤 Enviando comando final de reinicialização (AT+RESET)...")
            await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+RESET", response=False)
            await asyncio.sleep(0.8)
            
            try:
                await client.stop_notify(CHARACTERISTIC_UUID)
            except:
                pass 
                
            return True
    except Exception as e:
        print(f"⚠️ Erro durante a comunicação BLE: {e}")
        return False

async def busca_automatica(device_name_ble):
    print("\n🕵️  Iniciando varredura automática por sinal forte...")
    scanner_data = await BleakScanner.discover(timeout=4.0, return_adv=True)
    if not scanner_data: return False
    candidatos = [(d, a.rssi) for d, a in scanner_data.values() if a.rssi > -90]
    candidatos.sort(key=lambda x: x[1], reverse=True)
    for dev, rssi in candidatos:
        print(f"📡 Testando dispositivo {dev.address} (Sinal: {rssi}dBm)")
        if await configurar_ble(dev.address, device_name_ble, modo_busca=True): return True
    return False

# === PASSO 3: TESTAR E CONFIRMAR EQUIPAMENTO APÓS RESET ===
async def testar_equipamento_pos_reset(nome_esperado, mac_esperado=None):
    print(f"\n🔍 [3/4] Testando Equipamento: Confirmando presença e propagação do rádio...")
    print("⚠️ 🔋 POR FAVOR, RETIRE A BATERIA DA PLACA AGORA!")
    
    pular = input("👉 Pode começar a tentar validar o sinal? (Pressione Enter para Iniciar / 'p' para Pular Etapa): ").lower()
    if pular == 'p':
        print("ℹ️ Etapa de teste de propagação pulada pelo usuário.")
        return True
    
    print("⏳ Aguardando estabilização física do hardware...")
    await asyncio.sleep(7.0)
    
    print(f"\n📡 Iniciando varredura buscando por '{nome_esperado}' ou MAC [{mac_esperado}]...")
    for tentativa in range(1, 4):
        tempo_varredura = 12.0 if tentativa == 1 else 6.0
        
        print(f"   🔎 Varredura de teste nº {tentativa}/3 ({tempo_varredura}s)...")
        descobertos = await BleakScanner.discover(timeout=tempo_varredura)
        for d in descobertos:
            match_mac = mac_esperado and d.address.upper() == mac_esperado.upper()
            match_nome = d.name and nome_esperado.upper().strip() in d.name.upper().strip()
            
            if match_nome or match_mac:
                print(f"✅ Placa validada em bancada! Respondendo como: {d.name if d.name else '[Cache Oculto]'} [{d.address}]")
                return True
        await asyncio.sleep(2.0)
    
    print(f"⚠️ Aviso: O rádio não foi visto propagando o nome '{nome_esperado}' na varredura.")
    resposta = input("Deseja aprovar o teste de bancada e seguir mesmo assim? (Enter=Aprovar / n=Recusar): ").lower()
    return resposta == ''

# === PASSO 1: GRAVAÇÃO FÍSICA ===
def executar_upload_bancada(device_id):
    if not SEED_SECRET:
        print("❌ Erro: A variável de ambiente SEED_SECRET não está definida.")
        return False

    hex_file = os.path.join(BIN_DIR, f"{HARDWARE_VERSION_FIXO}.ino.hex")
    seed_bin = os.path.join(BIN_DIR, f"seed_{device_id}.bin")

    try:
        gerar_seed_bin(device_id, PLACA_FIXO, seed_bin, MOSFET_FIXO)
    except Exception as e:
        print(f"❌ Erro ao gerar o seed.bin: {e}")
        return False

    sketch_path = os.path.join(ROOT, "firmware", HARDWARE_VERSION_FIXO, f"{HARDWARE_VERSION_FIXO}.ino")
    print(f"📦 Compilando firmware para a pasta {HARDWARE_VERSION_FIXO} em: {sketch_path}")
    build_prop = 'build.extra_flags="-Ifirmware/include"'
    
    cmd_compile = ["arduino-cli", "compile", "--build-property", build_prop, "--build-path", BIN_DIR, sketch_path]
    res_comp = subprocess.run(cmd_compile)
    if res_comp.returncode != 0 or not os.path.exists(hex_file):
        print(f"❌ Erro: Falha na compilação do arduino-cli em: {sketch_path}")
        return False

    print("⚡ Gravando via avrdude... NÃO remova o cabo.")
    cmd_upload = [
        "avrdude", "-P", "usb", "-c", AVR_PROG, "-p", MCU_FIXO, "-b", "19200",
        "-U", "lfuse:w:0xFF:m",
        "-U", "hfuse:w:0xD7:m",
        "-U", "efuse:w:0xF7:m",
        "-U", "lock:w:0xCF:m",
        "-U", f"eeprom:w:{seed_bin}:r",
        "-U", f"flash:w:{hex_file}:a"
    ]
    
    res_up = subprocess.run(cmd_upload)
    return res_up.returncode == 0

# === FLUXO EXECUTOR PRINCIPAL ===
async def main():
    manter_dados = False
    ch, fi = "", ""

    while True:
        if not manter_dados:
            print("\n" + "═"*60 + f"\n  PROCESSO DE GRAVAÇÃO COMPLETA CHAVI ({HARDWARE_VERSION_FIXO})\n" + "═"*60)
            while True:
                ch = input("Canal (CH): ").zfill(3)
                fi = input("Firmware ID (FI): ").zfill(6)
                
                device_name_ble = f"{ch}FI{fi}"
                device_id_upload = f"CH{ch}FI{fi}"
                
                print(f"\nCONFIRMAÇÃO: BLE: {device_name_ble} | FW: {HARDWARE_VERSION_FIXO} | SERIAL: {device_id_upload}")
                if input("Dados corretos? (Enter=Sim / n=Não): ").lower() == '': break

        # --- PASSO 1: GRAVAÇÃO FÍSICA ---
        print(f"\n🚀 [1/4] Iniciando Compilação, Geração de Seed e Upload Físico...")
        pular_upload = input("Deseja executar a gravação física por avrdude? (Enter=Sim / 'p'=Pular Etapa): ").lower()
        
        if pular_upload == 'p':
            print("ℹ️ Etapa de gravação física pulada pelo usuário.")
            sucesso_upload = True
        else:
            sucesso_upload = executar_upload_bancada(device_id_upload)

        if sucesso_upload:
            if pular_upload != 'p':
                print("✅ Upload e gravação de Fuses concluídos com sucesso.")
        else:
            print("❌ Erro no upload físico (Compilação, Fuses ou Conexão ISP).")
            opcao_erro = input("[Enter/r] Tentar Upload Novamente / [n] Mudar dados de canal/FI: ").lower()
            if opcao_erro == 'n': manter_dados = False
            else: manter_dados = True
            continue

        if pular_upload != 'p':
            print("⏳ Aguardando inicialização do rádio pós-gravação...")
            await asyncio.sleep(3)

        # --- PASSO 2: CONFIGURAÇÃO VIA BLUETOOTH ---
        print(f"\n🔵 [2/4] Iniciando Configuração BLE AT...")
        sucesso_final_ble = False
        mac_selecionado = None
        
        while True:
            target = await scan_and_select()
            if target == "pular":
                print("ℹ️ Etapa de configuração BLE pulada pelo usuário.")
                sucesso_final_ble = True
                break
            if target == "voltar" or target is None:
                if input("\n[Enter] Buscar BLE Novamente / [n] Pular Passo BLE: ").lower() == 'n':
                    sucesso_final_ble = True
                    break
                continue
            
            mac_selecionado = target.address
            if await configurar_ble(target.address, device_name_ble):
                print(f"🎉 Comandos BLE enviados e salvos com sucesso!")
                sucesso_final_ble = True
                break
            else:
                print(f"\n❌ FALHA NA CONFIGURAÇÃO BLE AT COM A PLACA.")
                opcao = input("[r] RECONECTAR / [s] BUSCA AUTOMÁTICA / [p] PULAR ESTE PASSO BLE: ").lower()
                if opcao == 'r': continue
                elif opcao == 's':
                    if await busca_automatica(device_name_ble):
                        print(f"🎉 Configurado com sucesso via busca automática!")
                        sucesso_final_ble = True
                        break
                    else:
                        print("❌ Placa não encontrada na busca automática.")
                elif opcao == 'p':
                    sucesso_final_ble = True
                break
        
        # --- PASSO 3: TESTAR EQUIPAMENTO ---
        sucesso_teste = False
        if sucesso_final_ble:
            sucesso_teste = await testar_equipamento_pos_reset(device_name_ble, mac_selecionado)
        else:
            print(f"\n⚠️ [3/4] Teste de Equipamento pulado porque a etapa BLE não foi concluída.")

        # --- PASSO 4: CADASTRO NA API ---
        if sucesso_teste or (sucesso_final_ble and not sucesso_teste and input("Forçar Cadastro na API mesmo falhando no teste de sinal? (s/n): ").lower() == 's'):
            print(f"\n🌐 [4/4] Iniciando Cadastro na API do Painel Imóvel...")
            if executar_cadastro_api(device_id_upload):
                print(f"\n🎉 SUCESSO COMPLETO: {device_id_upload} Processado e Cadastrado!")
                sys.stdout.write("\a")
                sys.stdout.flush()
            else:
                print(f"\n⚠️ O processo de bancada deu certo, mas o cadastro na API falhou.")
        else:
            print(f"\n⚠️ Processo interrompido. Cadastro na API não executado para preservar integridade.")

        manter_dados = False
        input("\n--- PRÓXIMA PLACA? (Pressione Enter) ---")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSaindo...")