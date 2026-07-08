#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <FastLED.h>
#include <SoftwareSerial.h>

#define FW_VERSION "2.10.2"
#define FEATURE_SERIAL_DEBUG 1

#if FEATURE_SERIAL_DEBUG
  #define DBG_BAUD 9600
  #define DBG(...) Serial.print(__VA_ARGS__)
  #define DBGLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG(...)
  #define DBGLN(...)
#endif
// --- Pinos Comuns ---
#define PIN_BUTTON 2
#define PIN_WAKE 3
#define PIN_BLE_RX 4 // PD4
#define PIN_BLE_TX 5 // PD5
#define PIN_BUZZER 6 // PD6
#define PIN_BAT A1

// --- Pinos FI 1.5 (ATmega328PB) ---
#define PIN_MOTOR15_A 4 // PD4 (usado por SoftwareSerial, mas o pino físico é PB1) - CORREÇÃO: FI 1.5 usa PB1/PB2
#define PIN_MOTOR15_B 6 // PD6 (usado por Buzzer, mas o pino físico é PB2)
#define PIN_LEDS_WS2812 11 // PB3
#define NUM_LEDS 3
#define LED_BRIGHT 30

// --- Pinos FI 1.0 (ATmega328P) ---
#define PIN_MOTOR10_A 10 // PB2
#define PIN_MOTOR10_B 11 // PB3
#define PIN_LED10_1 7
#define PIN_LED10_2 8
#define PIN_LED10_3 9

#define EE_BOARD 0
#define EE_SERIAL 1
#define EE_CALIB 12
#define EE_CALIB_VERIF 13
#define EE_SEED01 14
#define EE_SEED02 18
#define EE_VERS_BLE 22
#define EE_MOD_FAM 23
#define EE_MOSFET 24
#define EE_HIB 25
#define EE_HIBERNA 26
#define EE_MOD_CFG 100

#define MOD_CFG_MAGIC 0xAA
#define FAM_DESCONHECIDA 0
#define FAM_1010 1
#define FAM_52 2

#define CMD_ABRIR 1UL
#define CMD_FECHAR 2UL
#define TOK_CALIB 190720UL

#define JANELA_MS 20000UL
#define JANELA_MAX 60000UL
#define JANELA_TST 180000UL
#define GAP_NOTIF_MS 150
#define MOTOR_TST_MS 1500UL
#define BTN_CURTO_MS 400
#define BTN_RESET_MS 10000UL

#define BAUD_MODULO 9600
#define AT_BAUD_CMD "AT+BAUD2"

#define MOTOR_ARRANQUE_MS 300
#define MOTOR_STALL_MA 300
#define MOTOR_RECUO_MS 900

uint8_t g_pinMotorA;
uint8_t g_pinMotorB;
uint8_t g_pinMosfet = 8;
bool placa10 = false;
bool inaOk = false;
bool moduloOk = false;
bool g_wakeHib = false;
bool g_hiberna = false;
bool g_sessaoConectada = false;
volatile bool acordouBtn = false;
volatile bool acordouBLE = false;
uint8_t g_moduloVers = 0;
uint8_t g_moduloFam = 0;
char serialFech[12] = {0};
uint8_t calibrationOk = 0;
uint32_t seed01 = 0;
uint32_t seed02 = 0;

CRGB leds[NUM_LEDS];
SoftwareSerial bluetooth(PIN_BLE_RX, PIN_BLE_TX);
Stream* io = &bluetooth;
Adafruit_INA219 ina219(0x45);

void atenderBotao();
void atenderApp();
void configModuloLeve();
void bleProvisionar();
uint8_t bleIdentificar();

void beep(uint16_t duracao, uint16_t freq) {
    tone(PIN_BUZZER, freq);
    delay(duracao);
    noTone(PIN_BUZZER);
}

void piscar(CRGB cor, uint8_t vezes) {
    if (placa10) {
        for (uint8_t i = 0; i < vezes; i++) {
            digitalWrite(PIN_LED10_2, HIGH); delay(100);
            digitalWrite(PIN_LED10_2, LOW); delay(100);
        }
        return;
    }
    for (uint8_t i = 0; i < vezes; i++) {
        fill_solid(leds, NUM_LEDS, cor); FastLED.show(); delay(50);
        fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show(); delay(100);
    }
}

void melodiaReset() {
    beep(100, 1000); delay(50); beep(100, 1200); delay(50);
    beep(100, 1500); delay(50); beep(300, 2000);
}

void melodiaRocky() {
    beep(150, 1000); delay(50); beep(150, 1000); delay(50);
    beep(300, 1200); delay(50); beep(300, 1000); delay(50);
    beep(600, 1500);
}

void sinalConectado() {
    beep(60, 2000); delay(40); beep(60, 2500);
    piscar(CRGB::Blue, 1);
}

void sinalModuloMudo() {
    if (!placa10) { fill_solid(leds, NUM_LEDS, CRGB::Red); FastLED.show(); }
    beep(200, 400); delay(100); beep(200, 400); delay(100);
    beep(200, 400); delay(100); beep(400, 300);
    if (!placa10) { fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show(); }
}

void diagBaudBipes() {
    delay(1000);
    beep(500, 600);
}

void fbComandoOk() {
    melodiaRocky();
    piscar(CRGB::Green, 1);
}

void resetMCU() {
    void (*ptrReset)() = 0;
    ptrReset();
}

void motorLiga(bool sentidoA) {
    if (sentidoA) {
        digitalWrite(g_pinMotorA, HIGH);
        digitalWrite(g_pinMotorB, LOW);
    } else {
        digitalWrite(g_pinMotorA, LOW);
        digitalWrite(g_pinMotorB, HIGH);
    }
}

void motorPara() {
    digitalWrite(g_pinMotorA, LOW);
    digitalWrite(g_pinMotorB, LOW);
}

void motorGiraMs(bool sentidoA, unsigned long ms) {
    motorLiga(sentidoA);
    delay(ms);
    motorPara();
}

void motorGira(bool sentidoA) {
    if (!inaOk) {
        motorGiraMs(sentidoA, MOTOR_TST_MS);
        return;
    }
    ina219.powerSave(false);
    motorLiga(sentidoA);
    delay(MOTOR_ARRANQUE_MS);
    
    unsigned long t0 = millis();
    bool travou = false;
    while (millis() - t0 < 3500UL) {
        float soma = 0;
        for (uint8_t i = 0; i < 25; i++) {
            soma += fabs(ina219.getCurrent_mA());
        }
        if ((soma / 25.0f) > MOTOR_STALL_MA) {
            travou = true;
            break;
        }
        delay(10);
    }
    motorPara();
    if (travou) {
        delay(100);
        motorGiraMs(!sentidoA, MOTOR_RECUO_MS);
    }
    ina219.powerSave(true);
}

void at(const char* cmd, uint16_t espera = 100) {
    bluetooth.println(cmd);
    delay(espera);
    while (bluetooth.available()) bluetooth.read();
}

void atMascara(const char* cmd, uint16_t val) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%s%X", cmd, val);
    at(buf, 80);
}

void configModuloLeve() {
    at("AT+NOTI1", 80);
    at("AT+FFE0", 80);
    at("AT+FFE1", 80);
}

uint8_t bleIdentificar() {
    while (bluetooth.available()) bluetooth.read();
    bluetooth.println("AT+VERS?");
    unsigned long t0 = millis();
    String r = "";
    while (millis() - t0 < 180) {
        if (bluetooth.available()) {
            char c = bluetooth.read();
            if (c >= 32 && c <= 126) r += c;
        }
    }
    if (r.length() == 0) return 0;
    
    uint8_t fam = FAM_DESCONHECIDA;
    uint8_t v = 0;
    if (r.indexOf("5.2") >= 0 || r.indexOf("Soft") >= 0) {
        fam = FAM_52; v = 3;
    } else if (r.indexOf("1010") >= 0 || r.indexOf("Smar") >= 0) {
        fam = FAM_1010; v = 1;
    }
    
    if (fam != FAM_DESCONHECIDA) {
        g_moduloFam = fam; g_moduloVers = v;
        EEPROM.update(EE_MOD_FAM, fam);
        EEPROM.update(EE_VERS_BLE, v);
        return v;
    }
    return 0;
}

bool bleVivo() {
    while (bluetooth.available()) bluetooth.read();
    bluetooth.println("AT");
    unsigned long t0 = millis();
    while (millis() - t0 < 100) {
        if (bluetooth.available()) return true;
    }
    return false;
}

void bleProvisionar() {
    const long TODOS[] = {9600, 2400, 38400, 19200, 57600, 4800};
    const uint8_t N = sizeof(TODOS) / sizeof(long);

    if (placa10) {
        for (uint8_t i = 0; i < N; i++) {
            bluetooth.begin(TODOS[i]);
            delay(30);
            for (uint8_t k = 0; k < 6; k++) at("AT", 40);
            at("AT+PWRM0", 120);
            at("AT+PIO41", 60); at("AT+PIO51", 60); at("AT+PIO71", 60);
            at("AT+PIO81", 60); at("AT+PIO91", 60);
            at("AT+BEFCFF7", 80); at("AT+AFTCFFF", 80);
        }
    }

    for (uint8_t i = 0; i < N; i++) {
        bluetooth.begin(TODOS[i]);
        delay(30);
        for (uint8_t k = 0; k < 3; k++) at("AT", 40);
        at("AT+PWRM0", 120);
        at(AT_BAUD_CMD, 250);
        at("AT+RESET", 150);
        delay(600);
    }
    bluetooth.begin(BAUD_MODULO);
    delay(1500);
    while (bluetooth.available()) bluetooth.read();

    at("AT+SHIELD1");
    at(AT_BAUD_CMD);
    at("AT+PWRM0");
    at("AT+ROLE0");
    at("AT+IMME0");
    at("AT+ADTY0");
    at("AT+ADVI2");
    bleIdentificar();
    
    if (serialFech[0]) {
        char nm[24];
        snprintf(nm, sizeof(nm), "AT+NAME%s", serialFech);
        const long bd[] = {2400, 9600, 38400, 19200, 57600, 4800};
        for (uint8_t i = 0; i < sizeof(bd) / sizeof(long); i++) {
            bluetooth.begin(bd[i]); delay(30);
            at("AT", 50); at("AT+PWRM0", 100); at(nm, 180);
        }
        bluetooth.begin(BAUD_MODULO); delay(100);
    }
    configModuloLeve();
    at("AT+START", 150);
    
    if (bleVivo()) EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    at("AT+RESET", 150);
    delay(1500);
    while (bluetooth.available()) bluetooth.read();
    configModuloLeve();
}

void isrBtn() { acordouBtn = true; }
void isrBLE() { acordouBLE = true; }

void enviaLinha(const char* s) { io->print(s); io->print('\n'); io->flush(); delay(12); }

void envia11Duplo() { enviaLinha("11"); delay(GAP_NOTIF_MS); enviaLinha("11"); }

void enviaStatus(bool sentidoA) {
    float vb = analogRead(PIN_BAT) * (5.0f / 1024.0f);
    char num[16];
    dtostrf((sentidoA ? 1000.0f : 2000.0f) + vb, 0, 2, num);
    enviaLinha(num);
}

#define ANTIDUP_MS 6000UL
unsigned long g_ultimoAcionamentoMs = 0;

bool acionamentoDuplicado() {
    unsigned long agora = millis();
    if (g_ultimoAcionamentoMs && agora - g_ultimoAcionamentoMs < ANTIDUP_MS) return true;
    return false;
}

void acionar(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    enviaStatus(sentidoA);
    if (acionamentoDuplicado()) return;
    g_ultimoAcionamentoMs = millis();
    motorGira(sentidoA);
    g_ultimoAcionamentoMs = millis();
}

void acionarVerbo(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    if (acionamentoDuplicado()) {
        enviaStatus(sentidoA);
        return;
    }
    g_ultimoAcionamentoMs = millis();
    enviaStatus(sentidoA);
    delay(60);
    motorGira(sentidoA);
    g_ultimoAcionamentoMs = millis();
    delay(120);
    enviaStatus(sentidoA);
    fbComandoOk();
}

void calibAceitar() {
    beep(60, 2200); beep(60, 2600);
    delay(1150);
    envia11Duplo();
}

void calibGirar() {
    beep(60, 2200);
    delay(1150);
    envia11Duplo();
    motorGira(true);
}

void calibDemoDireto() {
    beep(60, 2200);
    motorGira(true);
    delay(120);
    envia11Duplo();
}

void calibSalvar(uint8_t aberto) {
    calibrationOk = aberto;
    EEPROM.update(EE_CALIB, aberto);
    EEPROM.update(EE_CALIB_VERIF, 1);
    beep(60, 2600);
    delay(1000);
    enviaLinha("11");
    delay(GAP_NOTIF_MS);
    enviaLinha(aberto ? "2" : "1");
    motorGira(false);
    if (aberto) { delay(300); motorGira(true); }
    beep(80, 2000); beep(80, 2400);
}

void testeLeds() {
    if (placa10) {
        const uint8_t pins[3] = {PIN_LED10_1, PIN_LED10_2, PIN_LED10_3};
        for (uint8_t i = 0; i < 3; i++) {
            digitalWrite(pins[i], HIGH); delay(350); digitalWrite(pins[i], LOW);
        }
        for (uint8_t i = 0; i < 3; i++) digitalWrite(pins[i], HIGH);
        delay(350);
        for (uint8_t i = 0; i < 3; i++) digitalWrite(pins[i], LOW);
        return;
    }
    const CRGB cores[4] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White};
    for (uint8_t c = 0; c < 4; c++) {
        fill_solid(leds, NUM_LEDS, cores[c]);
        FastLED.show();
        delay(350);
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

void motorTesteCorrente(bool sentidoA, const char* fim) {
    char buf[20];
    if (inaOk) {
        ina219.powerSave(false);
        motorLiga(sentidoA);
        delay(120);
        float soma = 0; uint16_t n = 0;
        unsigned long t0 = millis();
        while (millis() - t0 < (MOTOR_TST_MS - 120)) {
            soma += fabs(ina219.getCurrent_mA()); n++;
        }
        motorPara();
        ina219.powerSave(true);
        char num[10];
        dtostrf(n ? soma / n : 0, 0, 0, num);
        snprintf(buf, sizeof(buf), "CORRENTE:%smA", num);
        enviaLinha(buf);
    } else {
        motorGiraMs(sentidoA, MOTOR_TST_MS);
        enviaLinha("CORRENTE:SEM-INA");
    }
    enviaLinha(fim);
}

void enviaBateria() {
    float v = analogRead(PIN_BAT) * (5.0f / 1024.0f);
    char buf[16], num[8];
    dtostrf(v, 0, 2, num);
    snprintf(buf, sizeof(buf), "BAT:%s", num);
    enviaLinha(buf);
}

void enviaInfo() {
    char buf[32];
    snprintf(buf, sizeof(buf), "SER:%s", serialFech[0] ? serialFech : "(fabrica)");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "CAL:%u", calibrationOk);
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "SEEDS:%s", (seed01 && seed02) ? "OK" : "VAZIAS");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "MOD:%s", moduloOk ? "OK" : "SEM-AT");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "MODF:%s",
             g_moduloFam == FAM_52 ? "5.2" : g_moduloFam == FAM_1010 ? "1010" : "?");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "INA:%s", inaOk ? "OK" : "SEM");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "PLACA:%s", placa10 ? "1.0" : "1.5");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "MOSFET:%u", g_pinMosfet);
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "WAKE:v%02u", g_moduloVers);
    enviaLinha(buf);
    enviaLinha("VER:" FW_VERSION);
    enviaLinha("FIM-INFO");
}

void testeBancada(const String& t) {
    if (t.startsWith("TST-PING")) {
        enviaLinha("PONG"); delay(GAP_NOTIF_MS); enviaLinha("PONG");
        return;
    }
    if (t.startsWith("TST-BUZ"))  {
        beep(120, 1500); beep(120, 2000); beep(180, 2500);
        enviaLinha("OK-BUZ"); return;
    }
    if (t.startsWith("TST-LED"))  { testeLeds(); enviaLinha("OK-LED"); return; }
    if (t.startsWith("TST-MOT1")) { enviaLinha("OK-MOT1"); delay(20); motorTesteCorrente(true,  "FIM-MOT1"); return; }
    if (t.startsWith("TST-MOT2")) { enviaLinha("OK-MOT2"); delay(20); motorTesteCorrente(false, "FIM-MOT2"); return; }
    if (t.startsWith("TST-BAT"))  { enviaBateria(); return; }
    if (t.startsWith("TST-INFO")) { enviaInfo(); return; }
    if (t.startsWith("TST-ROCKY")) { melodiaRocky(); enviaLinha("OK-ROCKY"); return; }
    if (t.startsWith("TST-UART")) {
        for (uint8_t i = 0; i < 5; i++) {
            char buf[28];
            snprintf(buf, sizeof(buf), "UART%u:0123456789ABCDEF", i);
            enviaLinha(buf);
            delay(GAP_NOTIF_MS);
        }
        enviaLinha("FIM-UART");
        return;
    }
    if (t.startsWith("TST-ECO")) {
        char buf[40];
        snprintf(buf, sizeof(buf), "ECO:%s", t.c_str() + 7);
        enviaLinha(buf);
        return;
    }
    if (t.startsWith("TST-HIB-ON")) {
        EEPROM.update(EE_HIBERNA, 1); g_hiberna = true;
        enviaLinha("OK-HIB-ON");
        return;
    }
    if (t.startsWith("TST-HIB-OFF")) {
        EEPROM.update(EE_HIBERNA, 0); g_hiberna = false;
        enviaLinha("OK-HIB-OFF");
        return;
    }
    if (t.startsWith("TST-HIB")) {
        enviaLinha("OK-HIB");
        delay(400);
        at("AT+DROP", 500);
        delay(500);
        if (digitalRead(PIN_WAKE) == HIGH) {
            enviaLinha("HIB-FALHOU-DROP");
            return;
        }
        atMascara("AT+BEFC", 0);
        at("AT+PIO60", 200);
        EEPROM.update(EE_HIB, 1);
        { char pio[12];
          snprintf(pio, sizeof(pio), "AT+PIO%X0", g_pinMosfet);
          at(pio, 60); }
        delay(3000);
        EEPROM.update(EE_HIB, 0);
        beep(160, 400); beep(160, 400); beep(160, 400);
        return;
    }
    if (t.startsWith("TST-ALL"))  {
        enviaLinha("OK-BUZ-INI"); beep(120, 1500); beep(120, 2000); beep(180, 2500); enviaLinha("OK-BUZ");
        testeLeds(); enviaLinha("OK-LED");
        enviaLinha("OK-MOT1-INI"); motorGiraMs(true,  MOTOR_TST_MS); enviaLinha("OK-MOT1");
        delay(400);
        enviaLinha("OK-MOT2-INI"); motorGiraMs(false, MOTOR_TST_MS); enviaLinha("OK-MOT2");
        enviaBateria();
        enviaLinha("FIM-TST");
        return;
    }
    enviaLinha("ERR-CMD");
}

void atenderApp() {
    io = &bluetooth;
    DBGLN(F("[app] acordou - ouvindo (20s)"));
    bluetooth.setTimeout(150);
    unsigned long t0 = millis();
    unsigned long tAbs = millis();
    unsigned long janela = JANELA_MS;
    uint8_t step = 0;
    while (millis() - t0 < janela && millis() - tAbs < JANELA_MAX) {
        if (digitalRead(PIN_BUTTON) == LOW) { atenderBotao(); t0 = millis(); }
        if (digitalRead(PIN_WAKE) == HIGH) t0 = millis();
        else if (step != 0) step = 0;
        if (bluetooth.available() <= 0) continue;
        int pk = bluetooth.peek();
        if (pk == '\n' || pk == '\r' || pk == ' ' || pk == '\t') { bluetooth.read(); continue; }

        if ((pk >= 'A' && pk <= 'Z') || (pk >= 'a' && pk <= 'z')) {
            String txt = bluetooth.readString(); txt.trim(); txt.toUpperCase();
            DBG(F("[app] txt: ")); DBGLN(txt);
            if (txt.startsWith("OK+CONN")) {
                if (!g_sessaoConectada) { g_sessaoConectada = true; sinalConectado(); }
                continue;
            }
            if (txt.startsWith("OK+LOST")) { g_sessaoConectada = false; continue; }
            if (txt.startsWith("TST-"))        { t0 = millis(); janela = JANELA_TST; testeBancada(txt); continue; }
            if (txt.startsWith("ABRIR"))       { acionarVerbo(CMD_ABRIR);  return; }
            if (txt.startsWith("FECHAR"))      { acionarVerbo(CMD_FECHAR); return; }
            if (txt.indexOf("PORTA-ABERTA")  >= 0) { calibSalvar(1); return; }
            if (txt.indexOf("PORTA-FECHADA") >= 0) { calibSalvar(0); return; }
            if (txt.indexOf("CALIB-DEMO")    >= 0) { t0 = millis(); calibDemoDireto(); continue; }
            if (txt.indexOf("CALIBRACAO-FI") >= 0) { t0 = millis(); calibGirar(); continue; }
            continue;
        }

        unsigned long v = (unsigned long)bluetooth.parseInt();
        if (v == 0) continue;
        DBG(F("[app] num: ")); DBGLN(v);
        if (v == CMD_ABRIR || v == CMD_FECHAR) { acionar(v); return; }
        if (v == TOK_CALIB) { t0 = millis(); calibAceitar(); step = 1; continue; }
        if (v <= 2100000UL) {
            t0 = millis();
            unsigned long rA = random(1, 9999), rB = random(1, 9999);
            char buf[16];
            snprintf(buf, sizeof(buf), (g_moduloVers == 3) ? "%lu\n" : "%lu", rA + v + seed01);
            enviaLinha(buf);
            delay(GAP_NOTIF_MS);
            snprintf(buf, sizeof(buf), (g_moduloVers == 3) ? "%lu\n" : "%lu", rB + v + seed02);
            enviaLinha(buf);
            step = 1; continue;
        }
    }
}

void atenderBotao() {
    delay(30);
    if (digitalRead(PIN_BUTTON) != LOW) return;
    unsigned long t0 = millis();
    unsigned long seg = 0;
    while (digitalRead(PIN_BUTTON) == LOW) {
        unsigned long dur = millis() - t0;
        if (dur >= BTN_RESET_MS) {
            melodiaReset();
            piscar(CRGB::Red, 3);
            EEPROM.update(EE_MOD_CFG, 0);
            resetMCU();
        }
        unsigned long s = dur / 1000;
        if (s >= 3 && s != seg) { seg = s; beep(40, (uint16_t)(1200 + s * 150)); }
        delay(10);
    }
    unsigned long dur = millis() - t0;
    if (dur < BTN_CURTO_MS) {
        static bool s = false; s = !s;
        beep(50, s ? 2400 : 1800);
        motorGira(s);
    } else {
        beep(120, 500);
    }
}

void dormir() {
    motorPara();
    if (g_hiberna && !placa10 && digitalRead(PIN_WAKE) == LOW) {
        at("AT+DROP", 200);
        at("AT+PIO60", 100);
        char pio[12];
        snprintf(pio, sizeof(pio), "AT+PIO%X0", g_pinMosfet);
        at(pio, 60);
        delay(150);
    }
    acordouBLE = false; acordouBtn = false;
    g_sessaoConectada = false;
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrBtn, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_WAKE), isrBLE, RISING);
    set_sleep_mode(SLEEP_MODE_IDLE);
    cli();
    sleep_enable();
    sei();
    sleep_cpu();
    sleep_disable();
    detachInterrupt(digitalPinToInterrupt(PIN_BUTTON));
    detachInterrupt(digitalPinToInterrupt(PIN_WAKE));
}

void setup() {
    placa10 = (EEPROM.read(EE_BOARD) == 1);

    // Configuração de pinos baseada na placa
    if (placa10) {
        g_pinMotorA = PIN_MOTOR10_A; // PB2
        g_pinMotorB = PIN_MOTOR10_B; // PB3
        pinMode(PIN_LED10_1, OUTPUT); digitalWrite(PIN_LED10_1, LOW);
        pinMode(PIN_LED10_2, OUTPUT); digitalWrite(PIN_LED10_2, LOW);
        pinMode(PIN_LED10_3, OUTPUT); digitalWrite(PIN_LED10_3, LOW);
    } else {
        g_pinMotorA = 9; // PB1 para FI 1.5
        g_pinMotorB = 10; // PB2 para FI 1.5
    }

    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(g_pinMotorA, OUTPUT);
    pinMode(g_pinMotorB, OUTPUT);
    pinMode(PIN_WAKE, INPUT);
    motorPara();

    beep(70, 2600);

#if FEATURE_SERIAL_DEBUG
    Serial.begin(DBG_BAUD);
    DBGLN(F("\n[boot] chavi_fi " FW_VERSION));
#endif

    randomSeed(analogRead(A0) ^ micros());

    if (placa10) {
        // LEDs já configurados acima
    } else {
        FastLED.addLeds<WS2812B, PIN_LEDS_WS2812, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(LED_BRIGHT);
        fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();
    }

    for (uint8_t i = 0; i < 11; i++) {
        uint8_t c = EEPROM.read(EE_SERIAL + i);
        if (c == 0 || c == 0xFF) { serialFech[i] = 0; break; }
        serialFech[i] = c; serialFech[i + 1] = 0;
    }
    calibrationOk = EEPROM.read(EE_CALIB);
    if (calibrationOk > 1) calibrationOk = 0;
    EEPROM.get(EE_SEED01, seed01);
    EEPROM.get(EE_SEED02, seed02);

    inaOk = ina219.begin();
    if (inaOk) ina219.powerSave(true);
    g_moduloVers = EEPROM.read(EE_VERS_BLE);
    if (g_moduloVers == 0xFF || g_moduloVers > 40) g_moduloVers = 0;
    g_moduloFam = EEPROM.read(EE_MOD_FAM);
    if (g_moduloFam > FAM_52) g_moduloFam = FAM_DESCONHECIDA;
    g_pinMosfet = EEPROM.read(EE_MOSFET);
    if (g_pinMosfet < 4 || g_pinMosfet > 9) g_pinMosfet = 8;
    g_wakeHib = (EEPROM.read(EE_HIB) == 1);
    if (g_wakeHib) EEPROM.update(EE_HIB, 0);
    g_hiberna = (EEPROM.read(EE_HIBERNA) == 1);

    DBG(F("[boot] hiberna=")); DBGLN(g_hiberna);
    DBG(F("[boot] serial=")); DBG(serialFech[0] ? serialFech : "(fabrica)");
    DBG(F(" calib=")); DBG(calibrationOk);
    DBG(F(" seeds=")); DBG((seed01 && seed02) ? F("ok") : F("VAZIAS"));
    DBG(F(" versBLE=")); DBGLN(g_moduloVers);

    bluetooth.begin(BAUD_MODULO);
    if (g_wakeHib) {
        DBGLN(F("[boot] wake da hibernacao - atendendo direto"));
        return;
    }

    if (digitalRead(PIN_WAKE) == HIGH) {
        DBGLN(F("[boot] conectado - provisionamento adiado"));
    } else {
        configModuloLeve();
        moduloOk = (bleIdentificar() != 0);
        for (uint8_t t = 0; !moduloOk && t < 2 && digitalRead(PIN_WAKE) == LOW; t++) {
            DBG(F("[boot] 9600 falhou - sweep passada ")); DBGLN(t + 1);
            bleProvisionar();
            moduloOk = (bleIdentificar() != 0);
        }
    }
    for (uint8_t t = 0; !moduloOk && t < 5; t++) { delay(250); moduloOk = (bleIdentificar() != 0); }
    if (moduloOk) EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    DBG(F("[boot] moduloOk=")); DBGLN(moduloOk);

    if (moduloOk) {
        piscar(CRGB::Green, 2);
    } else {
        sinalModuloMudo();
        diagBaudBipes();
    }
    DBGLN(F("[boot] PRONTA - dormindo"));
}

void loop() {
    static bool primeiraVolta = true;
    if (primeiraVolta || g_wakeHib) {
        primeiraVolta = false;
        g_wakeHib = false;
        atenderApp();
    }
    dormir();
    DBG(F("[wake] btn=")); DBG(acordouBtn); DBG(F(" ble=")); DBGLN(acordouBLE);
    if (acordouBtn) atenderBotao();
    atenderApp();
}