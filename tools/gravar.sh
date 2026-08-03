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
#   ./gravar.sh CH003FI002465            # 328PB (FI_1_5), MOSFET no PIO8 (default)
#   ./gravar.sh CH003FI002465 m328p      # 328 (FI_1_0)
#   ./gravar.sh CH003FI002465 m328pb 7   # placa com o gate do MOSFET no PIO7
#
# PINO DO MOSFET (3º argumento, default 8): PIO do módulo BLE que chaveia o
# gate de energia. 90% das FIs = 8; placas antigas usaram 6 ou 7. Vai no byte
# 914 da EEPROM — o firmware universal calcula BEFC/AFTC/AT+PIOx0 em runtime.
# ⭐ v2.13: use 12 para o MOSFET AUTOMÁTICO (placa v2.7/retrofit 2024): gate no
# pino FÍSICO 12 do módulo = PIO2 (inendereçável por AT) — o firmware provisiona
# AT+PWRM1 e o corte/religa fica por conta do auto-sleep do módulo.
#
# Requisitos: arduino-cli (com MiniCore), avrdude, USBasp conectado, python3.
set -euo pipefail

SERIAL="${1:?uso: ./gravar.sh CHxxxFIyyyyyy [mcu] [mosfet]}"
MCU="${2:-m328pb}"
MOSFET="${3:-8}"

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
#    328/328P=FI1.0 — o byte 912 diz ao firmware universal quais pinos usar;
#    o byte 914 leva o pino do MOSFET).
if [[ "$MCU" == "m328pb" ]]; then PLACA="fi15"; else PLACA="fi10"; fi
echo ">> Gerando EEPROM (seeds + serial + placa $PLACA + mosfet PIO$MOSFET) de $SERIAL..."
python3 "$HERE/gerar_seed.py" "$SERIAL" "$SEED_BIN" "$PLACA" "$MOSFET"

# 3. Grava fuses + lock + flash + eeprom. CRISTAL EXTERNO 16MHz (a placa TEM —
#    schema): OBRIGATÓRIO p/ SoftwareSerial a 9600 (baud de fábrica do módulo)
#    ser confiável. O RC de 8MHz não fala 9600 bem -> não convertia o módulo.
#      lfuse : 16MHz cristal, CKDIV8 off. 328PB=0xFF (low-power crystal — o PB
#              NÃO TEM full-swing; 0xF7 num PB deixa o chip SEM CLOCK!),
#              328/328P=0xF7 (full-swing, mais imune a ruído de motor)
#      hfuse : 0xD7 = EESAVE liga (seeds sobrevivem ao chip-erase), sem bootloader
#      efuse : ⭐ BROWN-OUT LIGADO em 2,7V (0xFD) nos DOIS chips (datasheet
#              DS40001906C §33): sem BOD, bateria afundando durante o motor =
#              execução errática + EEPROM (SEEDS!) corrompida.
#              ⚠️ NUNCA usar 0xF4 (BOD 4,3V): provado em bancada (07/07/2026,
#              CH003FI002584) que o trilho real fica abaixo de 4,3V e o MCU
#              entra em RESET PERPÉTUO — grava por ISP normal, mas nunca boota.
#              A frota legada que funciona há anos roda até SEM BOD (efuse 0xF7
#              lido da FI de produção 002FI001767); 2,7V já é um upgrade.
#      lock  : 0xCF
if [[ "$MCU" == "m328pb" ]]; then LFUSE=0xFF; else LFUSE=0xF7; fi
EFUSE=0xFD
echo ">> Gravando via USBasp (cristal 16MHz; lfuse=$LFUSE efuse=$EFUSE/BOD ligado)..."
avrdude -P usb -c usbasp -p "$MCU" -b 19200 -B 8 \
    -U lfuse:w:$LFUSE:m \
    -U hfuse:w:0xD7:m \
    -U efuse:w:$EFUSE:m \
    -U lock:w:0xCF:m \
    -U eeprom:w:"$SEED_BIN":r \
    -U flash:w:"$HEX":i

echo ">> OK: $SERIAL gravada. Ela vai autoconfigurar o BLE no 1º boot."
echo -e "\a"
