#ifndef _INTERRUPT_H
#define _INTERRUPT_H

enum InterruptSource {
  NONE, PUSH_BUTTON, BLUETOOTH
};

#define PIN_MODE_A 200
#define PIN_MODE_B 201
#define PIN_MODE_C 202
#define PIN_VALUE_A 203
#define PIN_VALUE_B 204
#define PIN_VALUE_C 205

/*
 * Salva o valor e a direção dos pinos I/O na EEPROM.
 * Chamar esse método antes de preparar o microcontrolador para dormir.
 */
void savePinState() {
  EEPROM.write(PIN_MODE_A, DDRB);
  EEPROM.write(PIN_MODE_B, DDRC);
  EEPROM.write(PIN_MODE_C, DDRD);

  EEPROM.write(PIN_VALUE_A, PORTB);
  EEPROM.write(PIN_VALUE_B, PORTC);
  EEPROM.write(PIN_VALUE_C, PORTD);
}

/*
 * Recupera o valor e a direção dos pinos I/O da EEPROM.
 * Chamar esse método logo após o microcontrolador acordar.
 */
void restorePinState() {
  DDRB = EEPROM.read(PIN_MODE_A);
  DDRC = EEPROM.read(PIN_MODE_B);
  DDRD = EEPROM.read(PIN_MODE_C);

  PORTB = EEPROM.read(PIN_VALUE_A);
  PORTC = EEPROM.read(PIN_VALUE_B);
  PORTD = EEPROM.read(PIN_VALUE_C);
}

/*
 * Define todos os pinos como entradas e seus valores como baixo para reduzir o consumo.
 * Chamar esse método após salvar o estado atual dos pinos e antes de colocar o microcontrolador para dormir.
 */
void clearPinState() {
  for (uint8_t pin = 0; pin < NUM_DIGITAL_PINS; pin++) {
    pinMode(pin, INPUT);
    digitalWrite(pin, LOW);
  }
}

#endif