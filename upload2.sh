#!/bin/bash

set -e

# Default values
channel=""
firmware_id=""
hardware_version=""
mosfet=""
mac_address=""

# Parse command line flags
while [[ $# -gt 0 ]]; do
  case "$1" in
    -ch)
      channel=$(printf "%03d" "$2")
      shift 2
      ;;
    -fi)
      firmware_id=$(printf "%06d" "$2")
      shift 2
      ;;
    -hw)
      hardware_version="$2"
      shift 2
      ;;
    -mosfet)
      mosfet="$2"
      shift 2
      ;;
    -mac)
      mac_address="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1"
      echo "Usage: $0 -ch <channel> -fi <firmware_id> -hw <hardware_version> -mosfet <mosfet>"
      exit 1
      ;;
  esac
done

# Validate required arguments
if [[ -z "$channel" || -z "$firmware_id" || -z "$hardware_version" || -z "$mac_address" ]]; then
  echo "Missing required arguments."
  echo "Usage: $0 -ch <channel> -fi <firmware_id> -hw <hardware_version> -mosfet <mosfet> -mac <mac_address>"
  exit 1
fi

# Construct device ID and firmware name
device_id="CH${channel}FI${firmware_id}"

# Construct firmware name based on presence of -mosfet
if [[ -n "$mosfet" ]]; then
  firmware_name="FI_${hardware_version}_400"
else
  firmware_name="FI_${hardware_version}"
fi

# Original logic (adjust below as needed for your script)
echo "Device ID: $device_id"
echo "Firmware Name: $firmware_name"

./upload "$device_id" "$firmware_name" "$mosfet"
echo -e "\a"

ble_cmd="./ble.py $mac_address AT+SHIELD1 AT+BAUD0 AT+PWRM1"

if [[ -n "$mosfet" ]]; then
  mosfet_pin=$mosfet

  # Valid range check
  if ((mosfet_pin < 3 || mosfet_pin > 14)); then
    echo "Invalid -mosfet pin: must be between 3 and 14"
    exit 1
  fi

  # Convert pin number to bit index (pin 3 -> index 0)
  mosfet_bit=$((mosfet_pin - 3))
  pin6_bit=3  # Pin 6 corresponds to bit 3

  # Initialize all bits to 0
  bits_befc=(0 0 0 0 0 0 0 0 0 0 0 0)
  bits_aftc=(0 0 0 0 0 0 0 0 0 0 0 0)

  # Set mosfet pin ON in both
  bits_befc[$mosfet_bit]=1
  bits_aftc[$mosfet_bit]=1

  # Pin 6 OFF in BEFC, ON in AFTC
  bits_befc[$pin6_bit]=0
  bits_aftc[$pin6_bit]=1

  # Join bits directly left-to-right (pin 3 to 14, no reverse)
  befc_bin=$(IFS=; echo "${bits_befc[*]}")
  aftc_bin=$(IFS=; echo "${bits_aftc[*]}")

  # Reverse strings for correct LSB->MSB ordering
  befc_bin_rev=$(echo "$befc_bin" | rev)
  aftc_bin_rev=$(echo "$aftc_bin" | rev)

  # Convert binary to uppercase hex
  befc_hex=$(printf "%03X" "$((2#$befc_bin_rev))")
  aftc_hex=$(printf "%03X" "$((2#$aftc_bin_rev))")

  ble_cmd+=" AT+BEFC${befc_hex} AT+AFTC${aftc_hex}"
fi

ble_cmd+=" AT+VERS?"
eval "$ble_cmd"

#echo -e "\a"
#echo -e "\a"

# read -n 1 -s -r -p "Reinsert the battery and then press any key to continue"
# echo

# ~/PycharmProjects/ble/main.py ${device_id} reset configure calibrate open close
# ~/PycharmProjects/ble/main.py ${device_id} calibrate
