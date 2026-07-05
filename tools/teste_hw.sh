#!/usr/bin/env bash
#
# teste_hw.sh — Grava o firmware de DIAGNÓSTICO (só pisca/bipa em loop).
# Serve para saber se o problema é ENERGIA/PLACA ou o firmware normal.
# Mantém fuses e EEPROM (grava só o flash).
#
# Uso:  ./tools/teste_hw.sh            # 328PB (default)
#       ./tools/teste_hw.sh m328p      # 328
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$HERE/teste_hw"
OUT="/tmp/chavi_teste_hw"
MCU="${1:-m328pb}"
FQBN="MiniCore:avr:328:bootloader=no_bootloader,clock=8MHz_internal,BOD=2v7,LTO=Os_flto"

echo ">> Compilando o firmware de diagnóstico..."
arduino-cli compile -b "$FQBN" --build-path "$OUT" "$SKETCH"

echo ">> Gravando SÓ o flash (mantém fuses/eeprom)..."
avrdude -P usb -c usbasp -p "$MCU" -b 19200 -B 8 \
    -U flash:w:"$OUT/teste_hw.ino.hex":i

echo ">> OK. Ligue a bateria e observe: pisca/bipa em loop = MCU vivo."
echo "   (Para voltar ao firmware normal, grave pela bancada de novo.)"
