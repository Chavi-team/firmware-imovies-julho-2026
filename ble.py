#!/usr/bin/python3

import asyncio
import sys
from bleak import BleakClient, BleakScanner
import logging
# logging.basicConfig(level=logging.DEBUG)

characteristic = '0000FFE1-0000-1000-8000-00805F9B34FB'

async def notify_callback(sender, data):
    print(f'In : {data}')

async def send_message(device_identifier, messages):
    # Scan for devices if a name is given instead of a MAC address
    if ":" not in device_identifier:  # Likely a name, not a MAC
        print(f"Searching for device: {device_identifier}")
        devices = await BleakScanner.discover()
        target_device = next((d for d in devices if d.name == device_identifier), None)
        if not target_device:
            print("Device not found.")
            return
        device_identifier = target_device.address
    
    async with BleakClient(device_identifier) as client:

        await client.start_notify(characteristic, notify_callback)

        left = 0.033203125
        right =  0.03369140625
        middle = left + (right - left) / 2
        print(f'middle = {middle}')
        
        for message in messages:
            print(f'Out: {message}')
            await client.write_gatt_char(characteristic, message.encode("utf-8"), response=False)
            await asyncio.sleep(middle)

        # await asyncio.sleep(1)
        # await client.stop_notify(characteristic)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python script.py <MAC_or_NAME> <message>")
        sys.exit(1)
    
    device_id = sys.argv[1]
    msg = sys.argv[2:]
    asyncio.run(send_message(device_id, msg))
