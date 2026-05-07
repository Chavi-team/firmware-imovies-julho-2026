import asyncio
from bleak import BleakClient
import bleak

# The address of your BLE device, which you can get after scanning.
device_address = "78:68:72:63:A6:B7"  # Replace with your BLE device's address

# Example UUIDs (these are just placeholders)
# You will need to replace these with the correct GATT service and characteristic UUIDs
service_uuid = "0000FFE0-0000-1000-8000-00805F9B34FB"  # Example service UUID
characteristic_uuid = "0000FFE1-0000-1000-8000-00805F9B34FB"  # Example characteristic UUID

# Function to send AT commands using GATT
async def send_at_command(device_address, at_command):
    try:
        # Connect to the BLE device
        async with BleakClient(device_address) as client:
            print(f"Connected to {device_address}")

            # Check if we are connected
            if not client.is_connected:
                print("Failed to connect")
                return

            # Send the AT command by writing it to the characteristic
            print(f"Sending AT command: {at_command}")
            await client.write_gatt_char(characteristic_uuid, at_command.encode())  # Send AT command as bytes
            
            # Optionally, read the response from the device (if the device supports it)
            # For example, you may have a response characteristic
            response = await client.read_gatt_char(characteristic_uuid)
            print(f"Received response: {response.decode()}")
            
    except Exception as e:
        print(f"Error: {e}")

# Discover devices and send AT command
async def discover_and_send_command():
    print("Discovering devices...")
    devices = await bleak.BleakScanner.discover()
    
    for device in devices:
        print(f"Device found: {device.name} ({device.address})")

        if device.address != device_address:
            continue
        
        # Send an AT command to the first discovered device (you can add more logic to select the right device)
        await send_at_command(device.address, "AT+NAME?\r\n")  # Replace with your AT command

# Main function to run the asyncio event loop
if __name__ == "__main__":
    asyncio.run(discover_and_send_command())
