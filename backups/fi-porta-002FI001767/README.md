# Backup byte-a-byte — FI da porta do Leonardo (002FI001767)

Extraído por USBasp em 07/07/2026. Fechadura de PRODUÇÃO com firmware LEGADO
da esteira (nome BLE hardcoded na flash), funcionando perfeitamente há anos —
é a REFERÊNCIA do comportamento legado.

- Chip: ATmega328PB (assinatura 1E 95 16)
- Fuses: lfuse 0xFF · hfuse 0xD7 · efuse **0xF7 (BOD DESLIGADO)** · lock 0xCF
- Módulo BLE: rev 05 (EEPROM 768 = 0x05)
- Receita AT do firmware legado (strings da flash): DELI3 NOTI1 ROLE0 TYPE0
  **PWRM1** (auto-sleep — o segredo de bateria do legado: módulo dorme p/
  sempre, acorda só por conexão) PIO61 **PIO80 STATUS8** (MCU liga SÓ durante
  a conexão) NAME<serial> VERS? ADDR? DROP

Restaurar (NUNCA rodar por engano — apaga o estado atual!):
  avrdude -P usb -c usbasp -p m328pb -b 19200 -B 8 \
    -U lfuse:w:0xFF:m -U hfuse:w:0xD7:m -U efuse:w:0xF7:m -U lock:w:0xCF:m \
    -U flash:w:fi_porta_flash.bin:r -U eeprom:w:fi_porta_eeprom.bin:r
