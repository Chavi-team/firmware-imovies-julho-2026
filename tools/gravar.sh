#!/usr/bin/env bash
#
# gravar.sh — Grava UMA fechadura com o firmware novo, sem recompilar por device.
#
# Fluxo (rápido, 1 comando):
#   1. compila o .hex UNIVERSAL uma vez (cacheado em bin/)
#   2. gera o seed.bin desta fechadura (serial -> EEPROM)
#   3. avrdude USBasp: fuses + lock + flash (.hex universal) + eeprom (seed.bin)
#
# O firmware autoconfigura o módulo BLE no 1º boot (baud + AT*), então NÃO há
# passo AT manual. O serial vira o nome BLE em runtime (lido da EEPROM).
#
# Uso:
#   ./gravar.sh CH003FI002465            # 328PB (FI_1_5), default
#   ./gravar.sh CH003FI002465 m328p      # 328 (FI_1_0)
#
# Requisitos: arduino-cli (com MiniCore), avrdude, USBasp conectado, python3.
set -euo pipefail

SERIAL="${1:?uso: ./gravar.sh CHxxxFIyyyyyy [mcu]}"
MCU="${2:-m328pb}"

HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH_DIR="$HERE/../chavi_fi"
BIN_DIR="$HERE/../bin"
HEX="$BIN_DIR/chavi_fi.ino.hex"
SEED_BIN="$BIN_DIR/seed_${SERIAL}.bin"

mkdir -p "$BIN_DIR"

# 1. Compila o .hex universal (só se ainda não existe / mudou o fonte).
if [[ ! -f "$HEX" || "$SKETCH_DIR/chavi_fi.ino" -nt "$HEX" ]]; then
    echo ">> Compilando firmware universal (uma vez)..."
    arduino-cli compile \
        --profile chavi_fi \
        --build-path "$BIN_DIR" \
        "$SKETCH_DIR"
fi

# 2. Gera o seed.bin desta fechadura (placa deriva do MCU: 328PB=FI1.5,
#    328/328P=FI1.0 — o byte 912 diz ao firmware universal quais pinos usar).
if [[ "$MCU" == "m328pb" ]]; then PLACA="fi15"; else PLACA="fi10"; fi
echo ">> Gerando EEPROM (seeds + serial + placa $PLACA) de $SERIAL..."
python3 "$HERE/gerar_seed.py" "$SERIAL" "$SEED_BIN" "$PLACA"

# 3. Grava fuses + lock + flash + eeprom. Domínio de clock PROVADO em bancada
#    (v2.1.0/2.7.6 = PONG ok): OSCILADOR RC INTERNO 8MHz, o mesmo p/ os 2 chips.
#      lfuse : 0xE2 = RC interno 8MHz, CKDIV8 off (8MHz cheios). NÃO usa o cristal.
#      hfuse : 0xD7 = EESAVE liga (seeds sobrevivem ao chip-erase), sem bootloader
#      efuse : 0xF7 (valor da v2.7.6). BOD 2.7V fica p/ reintroduzir à parte.
#      lock  : 0xCF
#    ⚠️ A placa TEM cristal 16MHz (schema), mas o "2400 slow" (AT+BAUD0) casou
#    com o SoftwareSerial a 8MHz; 16MHz é otimização a validar isoladamente.
LFUSE=0xE2; EFUSE=0xF7
echo ">> Gravando via USBasp (RC interno 8MHz; lfuse=$LFUSE efuse=$EFUSE)..."
avrdude -P usb -c usbasp -p "$MCU" -b 19200 -B 8 \
    -U lfuse:w:$LFUSE:m \
    -U hfuse:w:0xD7:m \
    -U efuse:w:$EFUSE:m \
    -U lock:w:0xCF:m \
    -U eeprom:w:"$SEED_BIN":r \
    -U flash:w:"$HEX":i

echo ">> OK: $SERIAL gravada. Ela vai autoconfigurar o BLE no 1º boot."
echo -e "\a"
