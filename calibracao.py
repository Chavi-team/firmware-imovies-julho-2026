#!/usr/bin/python

from seedGenerator import get_seed
import sys
from bleak import BleakScanner, BleakClient
import asyncio
import time
from secrets import randbelow
from pandas import read_csv, concat, DataFrame
from datetime import datetime

CHARACTERISTIC_UUID = '0000FFE1-0000-1000-8000-00805F9B34FB'

incoming_messages = []

seeds = [0] * 4

def get_tokens(saltos):
    tokens = []

    for i in range(4):
        tokens.append(getpass_do_lolis(saltos[i % 2], seeds[i]))

    return tokens


async def authenticate(client, token_index):
    random_number = randbelow(2 ** 31)
    await send_message(client, f'{random_number}')

    await asyncio.sleep(2)

    saltos = map(int, incoming_messages)
    saltos = list(map(lambda x: x - random_number, saltos))
    saltos[0] -= seeds[0]
    saltos[1] -= seeds[1]

    tokens = get_tokens(saltos)
    print(tokens)

    await send_message(client, f'{tokens[token_index[0]]}')
    await send_message(client, f'{tokens[token_index[1]]}')

def notify_callback(sender, data):
    print(f"{now()} >> {data}")
    incoming_messages.append(data.decode('ascii').strip())

def now():
    return datetime.now().strftime('%Y-%m-%d %H:%M:%S')


async def send_message(client, message):
    await client.write_gatt_char(CHARACTERISTIC_UUID, message.encode('ascii'))
    print(f'{now()} << {message}')

    await asyncio.sleep(1)

async def abrir(client):
    await authenticate(client, [0, 1])
    await send_message(client, '1')

async def fechar(client):
    await authenticate(client, [0, 1])
    await send_message(client, '2')

async def resetar(client):
    await authenticate(client, [2, 3])
    await send_message(client, '140197')

async def calibrar(client):
    await authenticate(client, [2, 3])
    await send_message(client, '190720')
    await send_message(client, 'CALIBRACAO-FI')
    await asyncio.sleep(15)
    await send_message(client, 'PORTA-ABERTA')

async def configurar(client):
    for seed in seeds:
        await send_message(client, f'{seed}')
    await send_message(client, str(int(time.time())))

async def send_and_receive_data(device_name, action, result_callback=None, error_callback=None):
    try:
        devices = await BleakScanner.discover()
        device = None
        for d in devices:
            if d.name == device_name:
                device = d
                break

        if device is None:
            print(f"{now()} -- Device with name {device_name} not found.")
            return

        print(f"{now()} -- Found device: {device.name}, {device.address}")

        async with BleakClient(device.address) as client:
            print(f"{now()} -- Connected")
            await asyncio.sleep(3)
            await client.start_notify(CHARACTERISTIC_UUID, notify_callback)
            await action(client)
            
            await client.disconnect()

        if result_callback:
            result_callback(action, incoming_messages[-1])

        incoming_messages.clear()
    except Exception as error:
        if error_callback:
            error_callback(action, error)
        else:
            pass

def getpass_do_lolis(difference, seed):
    b = [0] * 32  # Create a list of 32 zeroes
    b[0] = 1
    y = [0, 0, 0, 0]

    # Create the bit mask
    for index in range(1, 32):
        b[index] = b[index - 1] << 1

    for j in range(difference):
        # Generate the y values based on the current seed and bitmask
        y[0] = 1 if (b[31] & seed) else 0
        y[1] = 1 if (b[21] & seed) else 0
        y[2] = 1 if (b[1] & seed) else 0
        y[3] = 1 if (b[0] & seed) else 0

        # Update the seed with XOR of y values, ensuring 32-bit wrapping
        seed = (seed << 1) | (y[0] ^ y[1] ^ y[2] ^ y[3])

    # Ensure the result is a 32-bit unsigned integer
    return seed & 0xFFFFFFFF

def error_callback(action, error):
    # errors = read_csv('/home/radicheski/erros.csv')
    # errors = concat([errors, DataFrame([[now(), action, error]], columns=errors.columns)])
    # errors.to_csv('/home/radicheski/erros.csv', index=False)
    pass

def result_callback(action, result):
    # resultados = read_csv('/home/radicheski/resultados.csv')
    # resultados = concat([resultados, DataFrame([[now(), action, result]], columns=resultados.columns)])
    # resultados.to_csv('/home/radicheski/resultados.csv', index=False)
    pass

if __name__ == '__main__':
    serial_number = sys.argv[1]
    secret_key = 'CHAVI'

    for i in range(4):
        seeds[i] = get_seed(serial_number, secret_key, i + 1)

    # asyncio.run(send_and_receive_data(serial_number[2:], calibrar, result_callback, error_callback))
    asyncio.run(send_and_receive_data('CHAVIFI', configurar))
    asyncio.run(asyncio.sleep(10))
    # asyncio.run(send_and_receive_data(serial_number[2:], fechar, result_callback, error_callback))
    #asyncio.run(send_and_receive_data(serial_number[2:], fechar))
    # asyncio.run(asyncio.sleep(60))
