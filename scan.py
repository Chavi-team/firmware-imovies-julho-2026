import asyncio
from bleak import BleakScanner, BleakClient

# UUID padrão do BLE1010
CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

async def configurar_ble_com_respostas(address):
    print(f"🔗 Conectando a {address}...")
    
    # Buffer para capturar as respostas que chegam via notificação
    respostas_recebidas = []

    def callback(sender, data):
        msg = data.decode('utf-8', errors='ignore').strip()
        if msg:
            print(f"   📥 [RESPOSTA DO MÓDULO]: {msg}")
            respostas_recebidas.append(msg)

    try:
        async with BleakClient(address, timeout=20.0) as client:
            print(f"✅ Conectado: {client.is_connected}")

            # Tenta ativar as notificações para ver as respostas
            try:
                await client.start_notify(CHARACTERISTIC_UUID, callback)
                print("📢 Escuta de respostas ativada.\n")
            except Exception as e:
                print(f"⚠️ Não foi possível ler as respostas (Erro de Criptografia): {e}")
                print("O script continuará enviando os comandos mesmo assim...\n")

            # Lista de comandos baseada exatamente no seu setupBLE01
            commands = [
                ("Testando comunicação", "AT\r"),
                ("Desativando Senha", "AT+TYPE0\r"),
                ("Modo Recebe Dados", "AT+MODE2\r"),
                ("Modo Slave", "AT+ROLE0\r"),
                ("Baud Rate 9600", "AT+BAUD2\r"),
                ("Ativando Notificações", "AT+NOTI1\r"),
                ("Pre-connection", "AT+BEFC000\r"),
                ("Pos-connection", "AT+AFTC008\r"),
                ("Estado PIO6", "AT+PIO60\r"),
                ("DEFININDO NOME", "AT+NAMECHAVIFI\r"),
                ("Solicitando MAC", "AT+ADDR?\r"),
                ("RESET FINAL", "AT+RESET\r")
            ]

            for descricao, cmd in commands:
                print(f"🚀 Enviando: {descricao} ({cmd.strip()})")
                await client.write_gatt_char(CHARACTERISTIC_UUID, cmd.encode(), response=False)
                
                # Aguarda um pouco mais para dar tempo de o módulo processar e responder
                await asyncio.sleep(1.5) 

            await client.stop_notify(CHARACTERISTIC_UUID)
            print("\n✅ Sequência finalizada.")

    except Exception as e:
        print(f"❌ Erro crítico: {e}")

async def main():
    print("🔍 Procurando dispositivos...")
    devices = await BleakScanner.discover(timeout=5.0)
    
    for i, d in enumerate(devices):
        nome = d.name if d.name else "N/A"
        print(f"[{i}] {nome} - {d.address}")

    if not devices: return

    escolha = input("\nEscolha o número do dispositivo: ")
    await configurar_ble_com_respostas(devices[int(escolha)].address)

if __name__ == "__main__":
    asyncio.run(main())