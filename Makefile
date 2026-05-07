PROJECTS := FI_1_0 FI_1_0_400 FI_1_5 FI_1_5_400
HEX_FILES := $(addsuffix .hex, $(PROJECTS))

# Diretório de build
BUILD_DIR := build
BIN_DIR := bin

# Board
BOOTLOADER := bootloader=uart0
EEPROM := eeprom=keep
BAUDRATE := baudrate=default
BOD := BOD=2v7
LTO := LTO=Os_flto
CLOCK := clock=16MHz_external

PLATFORM := MiniCore:avr
BOARD := 328
CONFIG := $(BOOTLOADER),$(EEPROM),$(BAUDRATE),$(BOD),$(LTO),$(CLOCK)

FQBN := $(PLATFORM):$(BOARD):$(CONFIG)

# Arduino CLI
CLI_BIN := arduino-cli

# Regras de compilação

.PHONY: all bin_path upload_% install clean clear_eeprom fuse

all: $(HEX_FILES)

%.hex: bin_path
	@$(CLI_BIN) compile --build-property build.extra_flags="-Ifirmware/include" --build-path $(BUILD_DIR) firmware/$*/$*.ino
	@mv $(BUILD_DIR)/$*.ino.hex $(BIN_DIR)/$*.hex

bin_path:
	@mkdir -p $(BIN_DIR)

upload_%: FI_%.hex
	@if [[ "$<" == FI_1_0* ]]; then \
		echo "FI 1.0"; \
		avrdude -P usb -c usbasp  -p m328 -b 19200 -U flash:w:"bin/$<":a; \
	elif [[ "$<" == FI_1_5* ]]; then \
		echo "FI 1.5"; \
		avrdude -P usb -c usbasp  -p m328pb -b 19200 -U flash:w:"bin/$<":a; \
	fi

install:
	@$(CLI_BIN) core install $(PLATFORM)

clear_eeprom_%:
	@if [[ "$<" == FI_1_0* ]]; then \
		avrdude -P usb -c usbasp  -p m328 -b 19200 -U eeprom:w:"clear.eep":r
	elif [[ "$<" == FI_1_5* ]]; then \
		avrdude -P usb -c usbasp  -p m328pb -b 19200 -U eeprom:w:"clear.eep":r
	fi

fuse_%:
	@if [[ "$<" == FI_1_0* ]]; then \
		avrdude -P usb -c usbasp  -p m328 -b 19200 -U lfuse:w:0xFF:m -U hfuse:w:0xD7:m -U efuse:w:0xF7:m -U lock:w:0xCF:m
	elif [[ "$<" == FI_1_5* ]]; then \
		avrdude -P usb -c usbasp  -p m328pb -b 19200 -U lfuse:w:0xFF:m -U hfuse:w:0xD7:m -U efuse:w:0xF7:m -U lock:w:0xCF:m
	fi

clean:
	@rm -drf $(BUILD_DIR)
	@rm -drf $(BIN_DIR)
