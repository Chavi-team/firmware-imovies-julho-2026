import asyncio
from bleak import BleakScanner, BleakClient

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

ja_testados = set()

print(f"\n{'ID':<3} | {'RSSI':<7} | {'BLE MAC':<36} | {'NOME':<15} | {'VERSÃO':<20} | {'ADDR'}")
print("-" * 150)

async def enviar_comando(client, comando, callback_buffer):
    callback_buffer.clear()

    await client.write_gatt_char(
        CHARACTERISTIC_UUID,
        comando.encode(),
        response=False
    )

    await asyncio.sleep(1.0)

    return " | ".join(callback_buffer)

async def testar_dispositivo(device, adv):
    global ja_testados

    if device.address in ja_testados or adv.rssi < -100:
        return

    ja_testados.add(device.address)

    respostas = []

    def callback(sender, data):
        try:
            msg = data.decode('utf-8', errors='ignore').strip()
            if msg:
                respostas.append(msg)
        except:
            pass

    try:
        async with BleakClient(device.address, timeout=10.0) as client:

            if client.is_connected:

                await asyncio.sleep(1.0)

                await client.start_notify(
                    CHARACTERISTIC_UUID,
                    callback
                )

                # ===== VERSÃO =====
                versao = await enviar_comando(
                    client,
                    "AT+VERS?\r\n",
                    respostas
                )

                if not versao:
                    versao = await enviar_comando(
                        client,
                        "AT+VERS?",
                        respostas
                    )

                # ===== ADDR =====
                addr = await enviar_comando(
                    client,
                    "AT+ADDR?\r\n",
                    respostas
                )

                if not addr:
                    addr = await enviar_comando(
                        client,
                        "AT+ADDR?",
                        respostas
                    )

                await client.stop_notify(CHARACTERISTIC_UUID)

                nome = device.name if device.name else "N/A"

                print(
                    f"{len(ja_testados):<3} | "
                    f"{adv.rssi:<4}dBm | "
                    f"{device.address:<36} | "
                    f"{nome[:15]:<15} | "
                    f"{versao[:20]:<20} | "
                    f"{addr}"
                )

    except Exception:
        ja_testados.discard(device.address)

async def main():
    print("🚀 Monitorando BLE...\n")

    while True:
        dispositivos = await BleakScanner.discover(
            timeout=4.0,
            return_adv=True
        )

        tarefas = [
            testar_dispositivo(d, a)
            for d, a in dispositivos.values()
        ]

        if tarefas:
            await asyncio.gather(*tarefas)

        await asyncio.sleep(0.5)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSaindo...")