/*
 * teste_hw.ino — DIAGNÓSTICO DE HARDWARE (energia/placa), nada mais.
 *
 * Só pisca os LEDs (vermelho→verde→azul) e bipa, em loop ETERNO. SEM BLE, SEM
 * sono, SEM watchdog, SEM config de módulo. Não depende de bateria estar boa
 * para "acordar" nada — se o MCU tem energia, ele roda isto e você VÊ/OUVE.
 *
 * Interpretação ao LIGAR A BATERIA:
 *   - LED pisca / bipa em loop        -> MCU tem energia e roda. A placa está
 *                                        viva; o problema do firmware normal é
 *                                        sono/config/BLE, não energia.
 *   - dá 1 flash/bip e PARA           -> algo reseta (watchdog externo?) ou a
 *                                        energia cai depois do inrush.
 *   - NADA (nem LED, nem bip)         -> não chega energia ao MCU: bateria
 *                                        fraca/contato, ou o conversor não parte.
 *                                        Teste com bateria NOVA/cheia.
 *
 * Também imprime "HW-ALIVE n=<contador>" na UART de hardware (PD1/TX, 9600) —
 * se tiver o USB-TTL, dá pra ver o MCU vivo mesmo sem LED/buzzer.
 *
 * Grava só o FLASH (mantém fuses/eeprom): use tools/teste_hw.sh
 */
#include <FastLED.h>

#define PIN_BUZZER PIN_PD6
#define PIN_LEDS   PIN_PB3
#define NUM_LEDS   3

CRGB leds[NUM_LEDS];
unsigned long n = 0;

void setup() {
    Serial.begin(9600);                       // UART hardware p/ USB-TTL opcional
    pinMode(PIN_BUZZER, OUTPUT);
    // periféricos possivelmente atrás do MOSFET: garante OUTPUT
    pinMode(PIN_LEDS, OUTPUT);
    FastLED.addLeds<WS2812B, PIN_LEDS, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(60);
    Serial.println(F("HW-TESTE boot"));
    tone(PIN_BUZZER, 1800, 200); delay(220); noTone(PIN_BUZZER);  // bip de boot
}

void loop() {
    const CRGB cores[3] = {CRGB::Red, CRGB::Green, CRGB::Blue};
    fill_solid(leds, NUM_LEDS, cores[n % 3]);
    FastLED.show();
    tone(PIN_BUZZER, 2000, 80); delay(120); noTone(PIN_BUZZER);
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    Serial.print(F("HW-ALIVE n=")); Serial.println(n++);
    delay(500);
}
