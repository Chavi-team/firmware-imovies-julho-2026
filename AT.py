import asyncio
import re
from bleak import BleakScanner, BleakClient

# Configuração fixa do canal serial do chip
UART_UUID = "0000FFE1-0000-1000-8000-00805F9B34FB"

resposta_recebida = asyncio.Event()
ultimo_texto_rx = ""

def calcular_hex_befc_aftc(mosfet_pin):
    """Faz o cálculo exato dos bits baseados no pino do Mosfet informado."""
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
    except Exception:
        return "000", "000"

def filtro_nome_placa(nome_dispositivo):
    """Filtra placas para o Mac localizar pelo rádio antes de conectar"""
    if not nome_dispositivo:
        return False
    nome_upper = nome_dispositivo.upper()
    if "SOFT AT" in nome_upper or "CHAVIFI" in nome_upper or "MLT-BT05" in nome_upper:
        return True
    if re.search(r'\d+FI\d+', nome_upper):
        return True
    return False

def notification_handler(sender, data):
    """Captura o retorno da placa e aciona o evento de recebimento"""
    global ultimo_texto_rx
    resposta = data.decode('utf-8', errors='ignore').strip('\x00\r\n ')
    if resposta:
        ultimo_texto_rx = resposta
        resposta_recebida.set()

async def enviar_comandos_at():
    global ultimo_texto_rx
    print("\n" + "═"*50 + "\n  CONFIGURAÇÃO AT COM SELEÇÃO DE PLACA\n" + "═"*50)
    
    # 1. Dados de configuração
    ch_input = input("Digite o Canal (CH) (Ex: 1): ").strip()
    ch = ch_input.zfill(3)
    
    os_input = input("Digite o Número de Série (Até 6 dígitos): ").strip()
    fi = os_input.zfill(6)
    
    device_name_ble = f"{ch}FI{fi}"
    print(f"📌 Nome BLE a ser gravado: {device_name_ble}")

    mosfet_pin = input("Digite o número do pino do Mosfet (Ex: 8): ").strip()
    befc, aftc = calcular_hex_befc_aftc(mosfet_pin)
    print(f"⚙️  Valores calculados -> BEFC: {befc} | AFTC: {aftc}")

    COMMANDS = [
        "AT+SHIELD1", 
        "AT+BAUD0",      
        "AT+PWRM1",      
        "AT+MODE2", 
        f"AT+BEFC{befc}", 
        f"AT+AFTC{aftc}", 
        f"AT+NAME{device_name_ble}", 
        "AT+RESET"
    ]

    # 2. Busca e Menu de Seleção
    print("\n🔎 Varrendo ambiente para localizar placas próximas...")
    devices_and_adv = await BleakScanner.discover(timeout=5.0, return_adv=True)
    
    candidatos = []
    for device, adv in devices_and_adv.values():
        if filtro_nome_placa(device.name):
            candidatos.append((device, adv.rssi))
            
    if not candidatos:
        print("❌ Nenhuma placa compatível foi localizada no alcance do rádio.")
        return

    # Ordena pelo sinal mais forte (RSSI maior)
    candidatos.sort(key=lambda x: x[1], reverse=True)

    print(f"\n{'ID':<3} | {'RSSI':<8} | {'NOME ATUAL':<20} | {'ID INTERNO (macOS)'}")
    print("-" * 75)
    for idx, (dev, rssi) in enumerate(candidatos):
        print(f"{idx:<3} | {rssi:<4} dBm | {dev.name:<20} | {dev.address}")

    # Força a escolha do ID
    while True:
        escolha = input("\nDigite o ID da placa que deseja configurar: ").strip()
        try:
            dispositivo_alvo = candidatos[int(escolha)][0]
            break
        except (ValueError, IndexError):
            print("⚠️ Seleção inválida. Digite um número da lista.")

    print(f"\n🔗 Conectando a {dispositivo_alvo.name}...")
    try:
        async with BleakClient(dispositivo_alvo.address, timeout=12.0) as client:
            if client.is_connected:
                print("✅ Conectado! Ativando escuta de respostas...")
                await client.start_notify(UART_UUID, notification_handler)
                await asyncio.sleep(1.2)
                
                print("\n🚀 Enviando comandos e aguardando respostas individuais...")
                for cmd in COMMANDS:
                    print(f"📤 [Enviando]: {cmd:<25}", end="", flush=True)
                    
                    resposta_recebida.clear()
                    ultimo_texto_rx = ""
                    
                    comando_formatado = (cmd + "\r\n").encode('utf-8')
                    await client.write_gatt_char(UART_UUID, comando_formatado, response=False)
                    
                    try:
                        timeout_val = 1.0 if "RESET" in cmd else 2.0
                        await asyncio.wait_for(resposta_recebida.wait(), timeout=timeout_val)
                        print(f" -> 📥 [Placa]: {ultimo_texto_rx}")
                    except asyncio.TimeoutError:
                        print(" -> ⏳ (Sem resposta)")
                    
                    await asyncio.sleep(0.5)
                
                print("\n⚙️ Processo concluído com sucesso!")
                try:
                    await client.stop_notify(UART_UUID)
                except:
                    pass
                
    except Exception as e:
        print(f"❌ Erro na comunicação BLE: {e}")

if __name__ == "__main__":
    asyncio.run(enviar_comandos_at())