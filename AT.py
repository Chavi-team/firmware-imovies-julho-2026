import asyncio
import sys
from bleak import BleakScanner, BleakClient

UART_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'
RSSI_THRESHOLD = -70 

# SEQUÊNCIA DE RESGATE (Da base para o topo)
CONFIG_SEQUENCE = [
    "AT",               # 1. Acorda o chip
    "AT+RENEW",         # 2. RESET DE FÁBRICA (Limpa lixos de memória)
    "AT+BAUD2",         # 3. Garante 9600 bps (conforme seu define)
    "AT+TYPE0",         # 4. Remove senhas/autenticação que bloqueiam o Mac
    "AT+MODE0",         # 5. Modo de transmissão simples (mais estável para teste)
    "AT+ROLE0",         # 6. Força modo Escravo
    "AT+NAMECHAVIFIPR", # 7. Renomeia
    "AT+ADVI5",         # 8. Intervalo de rádio estável
    "AT+PIO60",         # 9. Configuração dos pinos
    "AT+RESET"          # 10. Reinicia para aplicar TUDO
]

resposta_recebida = asyncio.Event()

async def main():
    while True:
        print(f"\n🔭 Buscando BLE1010 (RSSI > {RSSI_THRESHOLD}dBm)...")
        devices_and_adv = await BleakScanner.discover(timeout=4.0, return_adv=True)
        candidatos = [(d, a.rssi) for d, a in devices_and_adv.values() if a.rssi >= RSSI_THRESHOLD]

        if not candidatos:
            print("⚠️ Nada encontrado."); input(); continue
        
        candidatos.sort(key=lambda x: x[1], reverse=True)
        print(f"\n{'ID':<3} | {'RSSI':<8} | {'MAC ADDRESS':<38} | {'NOME'}")
        print("-" * 90)
        for idx, (dev, rssi) in enumerate(candidatos):
            print(f"{idx:<3} | {rssi:<4} dBm | {dev.address:<38} | {dev.name or 'Desconhecido'}")

        escolha = input("\nID para RECUPERAR (ou ENTER para novo scan): ").strip()
        if not escolha: continue
        try:
            alvo = candidatos[int(escolha)][0]
        except: continue

        try:
            print(f"🔗 Tentando RESGATE em {alvo.address}...")
            async with BleakClient(alvo.address, timeout=20.0) as client:
                print("🔍 Mapeando serviços...")
                _ = client.services 
                
                def notification_handler(sender, data):
                    texto = data.decode('utf-8', errors='ignore').strip('\x00\r\n ')
                    if texto:
                        print(f"\n   [RX]: {texto}")
                        resposta_recebida.set()

                await client.start_notify(UART_UUID, notification_handler)
                print("✅ Conectado e Notificações Ativas. Aguardando 1s...")
                await asyncio.sleep(1.0)

                print("\n🚀 Iniciando Sequência de Resgate...")
                for cmd in CONFIG_SEQUENCE:
                    if not client.is_connected:
                        print("\n❌ Conexão caiu durante a recuperação!")
                        break
                    
                    print(f"📤 {cmd:<18}", end=" ", flush=True)
                    resposta_recebida.clear()
                    
                    try:
                        # Enviamos com \r\n para garantir que o firmware entenda o fim do comando
                        await client.write_gatt_char(UART_UUID, (cmd + "\r\n").encode(), response=False)
                        
                        try:
                            # 3 segundos para comandos críticos (RENEW e RESET)
                            timeout_val = 4.0 if "RENEW" in cmd or "RESET" in cmd else 2.0
                            await asyncio.wait_for(resposta_recebida.wait(), timeout=timeout_val)
                            print("✔️  OK")
                        except asyncio.TimeoutError:
                            print("⏳ (Sem resposta)")
                        
                        await asyncio.sleep(1.2) # Tempo para o chip gravar na Flash
                        
                    except Exception as e:
                        print(f"Erro no envio: {e}")
                        break

                print("\n⌨️  TERMINAL MANUAL LIBERADO (sair para voltar)")
                while client.is_connected:
                    line = await asyncio.get_event_loop().run_in_executor(None, sys.stdin.readline)
                    cmd = line.strip()
                    if cmd.lower() == 'sair': break
                    if not cmd: continue
                    await client.write_gatt_char(UART_UUID, (cmd + "\r\n").encode(), response=False)

        except Exception as e:
            print(f"\n❌ Falha: {e}")
            input("Pressione ENTER para tentar novamente...")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSaindo...")