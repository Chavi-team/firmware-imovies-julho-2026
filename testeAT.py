import asyncio
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

# === VARIÁVEIS DE CONFIGURAÇÃO ===
CANAL = "003"              # CH de 3 dígitos
FIRMWARE_ID = "002614"      # FI de 6 dígitos
MOSFET_BEFC = "020"         # Exemplo HEX: AT+BEFC020
MOSFET_AFTC = "028"         # Exemplo HEX: AT+AFTC028
# =======================================================

# FUNÇÃO QUE PEGA AS RESPOSTAS DO BLUETOOTH E MOSTRA NA TELA
def notification_handler(sender, data):
    try:
        mensagem = data.decode('utf-8', errors='ignore').strip()
        if mensagem:
            print(f"      📥 Retorno Módulo: {mensagem}")
    except:
        pass

async def main():
    print("\n🔭 Escaneando dispositivos...")
    devices_and_adv = await BleakScanner.discover(timeout=3.0, return_adv=True)
    
    if not devices_and_adv:
        print("❌ Nenhum dispositivo encontrado.")
        return

    candidatos = list(devices_and_adv.values())
    candidatos.sort(key=lambda x: x[1].rssi, reverse=True)

    print(f"\n  {'ID':<3} | {'RSSI':<7} | {'ENDEREÇO MAC':<18} | {'NOME BLE'}")
    print("-" * 65)
    for i, (device, adv) in enumerate(candidatos):
        print(f"{i:<3} | {adv.rssi:<5}dBm | {device.address:<18} | {device.name or '[Sem Nome]'}")

    escolha = input("\nDigite o ID para mandar toda a carga de comandos AT: ")
    try:
        idx = int(escolha)
        alvo = candidatos[idx][0]
    except:
        print("❌ Seleção inválida.")
        return

    print(f"\n🔵 Conectando a {alvo.address}...")
    
    nome_dispositivo = f"{CANAL}FI{FIRMWARE_ID}"
    
    comandos_at = [
        ("Testar Comunicação", "AT"),
        ("Desativar Senha Padrão", "AT+TYPE0"),
        ("Configurar Modo Operacional 2", "AT+MODE2"),
        ("Definir Papel como Slave", "AT+ROLE0"),
        ("Configurar Baud Rate para 9600", "AT+BAUD0"),
        ("Ativar Notificação de Conexão na UART", "AT+NOTI1"),
        ("Configurar Tempo Pré-Conexão (BEFC)", f"AT+BEFC{MOSFET_BEFC}"),
        ("Configurar Tempo Pós-Conexão (AFTC)", f"AT+AFTC{MOSFET_AFTC}"),
        ("Alterar Comportamento Rx (Delimitador)", "AT+UTIM0"),
        ("Forçar Delimitador Padrão (\\n)", "AT+DELI0"),
        ("Definir Novo Nome Bluetooth", f"AT+NAME{nome_dispositivo}"),
        ("Aumentar Potência de Rádio ao Máximo (Nível 7)", "AT+POWE7"),
    ]
    
    try:
        async with BleakClient(alvo.address, timeout=12.0) as client:
            if client.is_connected:
                print("✅ Conectado com sucesso!")
                
                # Aguarda estabilização interna inicial
                await asyncio.sleep(1.0)
                
                # LIGA AS NOTIFICAÇÕES PARA ESCUTAR O RETORNO
                await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
                await asyncio.sleep(0.5)
                
                # Dispara a lista sequencial de parametrização
                for descricao, cmd in comandos_at:
                    print(f"⚡ Enviando [{descricao}]: {cmd}...")
                    cmd_bytes = f"{cmd}\r\n".encode('utf-8')
                    await client.write_gatt_char(CHARACTERISTIC_UUID, cmd_bytes, response=False)
                    
                    # Aumentei para 0.8s para dar tempo da resposta chegar e aparecer na tela
                    await asyncio.sleep(0.8) 
                
                # Comando final de gravação e reinicialização
                print("🔄 Aplicando e Salvando tudo permanentemente: AT+RESET...")
                await client.write_gatt_char(CHARACTERISTIC_UUID, b"AT+RESET\r\n", response=False)
                await asyncio.sleep(0.5)
                
                # DESLIGA AS NOTIFICAÇÕES ANTES DE FECHAR
                await client.stop_notify(CHARACTERISTIC_UUID)
                
                print("\n🎉 TODOS OS COMANDOS INJETADOS COM SUCESSO!")
                print(f"O módulo foi parametrizado como {nome_dispositivo} em Potência Máxima.")
                print(f"Configurações aplicadas: BEFC={MOSFET_BEFC} | AFTC={MOSFET_AFTC}")
                
    except Exception as e:
        print(f"❌ Erro na injeção dos comandos: {e}")

if __name__ == "__main__":
    asyncio.run(main())