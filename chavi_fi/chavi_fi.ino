/*
 * chavi_fi.ino — Firmware BYPASS das fechaduras Chavi FI (setor imobiliário).
 *
 * Filosofia: confiabilidade > segurança (decisão do cliente). Não valida seed,
 * não valida token, aceita tudo — mas fala o MESMO protocolo dos ~1000 apps em
 * campo (desafio -> 2 saltos -> 3 writes -> comando), então nada muda no app.
 *
 * BAUD DO MÓDULO = 2400, FIXO (frota de produção). A esteira antiga de
 * provisionamento (Firmware-Antigo/src/at.js) configura todo módulo com
 * AT+BAUD0 (= 2400 nos clones "Soft AT 5.2"), então as fechaduras em produção
 * já falam 2400. Módulo NOVO de fábrica pode vir em 9600: a config completa do
 * 1º boot (bleProvisionar) converge o módulo para 2400 ÀS CEGAS (manda
 * AT+BAUD0+AT+RESET em cada baud candidato — os clones não respondem "OK" a um
 * "AT" pelado, então detecção por resposta não é confiável). Depois do 1º boot
 * o baud está gravado no módulo (NVM) e os boots seguintes só fazem a config
 * leve (sem reset — reset a cada boot derruba a conexão e cospe lixo).
 *
 * FEEDBACK SONORO/VISUAL (o instalador entende o que está acontecendo):
 *   ligar bateria: 1 bipe curto ......... "estou vivo" (se não tocar = energia)
 *   fim do boot OK: MELODIA (fanfarra do Rocky) + 3 piscadas VERDES = PRONTA
 *   fim do boot com módulo mudo: 4 bipes GRAVES + 2 piscadas VERMELHAS
 *   conectou o celular: 1 bipe curto agudo (acordou por BLE; se conectar e NÃO
 *     bipar, o pino de wake não subiu — módulo/solda)
 *   comando executado (abrir/fechar): o giro do motor é o feedback
 *   botão: bipe a cada marco (ver BOTÃO abaixo)
 *
 * BOTÃO FÍSICO (PD2):
 *   toque CURTO (<0,8s) ........ aciona o motor em TOGGLE (alterna o sentido a
 *                                cada toque) — destranca/tranca sem celular
 *   segurar 3s+ ................ bipes de contagem (1 por segundo, subindo)
 *   segurar 10s ................ RESET TOTAL: melodia descendente, apaga a
 *                                config do módulo (re-provisiona o rádio no
 *                                boot) e reinicia o MCU por watchdog — efeito
 *                                de "tirar e recolocar a bateria". NÃO apaga
 *                                serial/seeds/calibração.
 *   soltar entre 0,8s e 10s .... cancela (1 bipe grave), não faz nada
 *
 * Comandos de BANCADA (texto via FFE1, usados pela GUI tools/bancada.py):
 *   TST-PING  -> "PONG"                       (comunicação fim-a-fim ok)
 *   TST-BUZ   -> toca o buzzer, "OK-BUZ"
 *   TST-LED   -> acende os 3 WS2812 R/G/B/branco, "OK-LED"
 *   TST-MOT1  -> gira o motor sentido A, "OK-MOT1".."FIM-MOT1"
 *   TST-MOT2  -> gira o motor sentido B, "OK-MOT2".."FIM-MOT2"
 *   TST-BAT   -> "BAT:x.xx" (ADC da bateria)
 *   TST-INFO  -> SER/CAL/SEEDS/MOD/WAKE/VER, "FIM-INFO"
 *   TST-ALL   -> roda tudo em sequência, "FIM-TST"
 *
 * TIMING DA CALIBRAÇÃO (pegadinha do app): depois de escrever (tokens+190720,
 * ou CALIBRACAO-FI) o app espera 1000ms ANTES de armar o listener de
 * notificação — e o stream não tem histórico. Resposta enviada cedo demais se
 * PERDE e vira CALIBRACAOERROR. Por isso as respostas "11" da calibração são
 * seguradas ~1,2-1,5s e enviadas EM DOBRO (o app espera 2 notificações).
 *
 * ATmega328/328PB @ 8MHz interno (MiniCore). Módulo BLE "Soft AT 5.2" em
 * SoftwareSerial, com AT+DELI3: cada write BLE chega na UART terminado em
 * '\n', e cada linha que enviamos vira UMA notificação (o '\n' é consumido).
 */
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include "LowPower.h"
#include <FastLED.h>

#define FW_VERSION   "2.3.3"

// ---- HIBERNAÇÃO PROFUNDA via MOSFET (arquitetura do FI_1_0_400) --------------
// Nesta placa o trilho dos periféricos E DO MCU é chaveado por um MOSFET cujo
// gate é o PIO8 do módulo BLE (PIO8 ALTO = eletrônica LIGADA):
//   AT+PIO80  -> corta o trilho NA HORA (o MCU DESLIGA; consumo ~zero)
//   AT+AFTC028 -> ao CONECTAR o módulo religa o PIO8 -> o MCU dá boot e atende
//   AT+BEFC020 -> ao DESCONECTAR religa também (o MCU boota, faz manutenção e
//                 corta de novo) — é o ciclo do FI_1_0_400 de produção.
// Vantagem extra: acorda por CONEXÃO sem depender do pino de wake PD3.
// Custo: com o trilho cortado o BOTÃO FÍSICO não funciona (MCU desligado).
// ✅ PROVADO em bancada (05/07 12:42, CH003FI003066 v2.2.2): TST-HIB cortou
// (silêncio) e a reconexão religou o MCU com PONG imediato. LIGADO.
#define FEATURE_HIBERNA_MOSFET  1

// ---- pinos (iguais ao FI_1_5/_400 que funciona em campo) ----
#define PIN_BLE_RX   PIN_PD4
#define PIN_BLE_TX   PIN_PD5
#define PIN_BUZZER   PIN_PD6
#define PIN_LEDS     PIN_PB3
#define PIN_MOTOR_A  PIN_PB1   // = pinTurn01 do FI_1_5 (rotateMotor01)
#define PIN_MOTOR_B  PIN_PB2   // = pinTurn02 do FI_1_5 (rotateMotor02)
#define PIN_BUTTON   PIN_PD2
#define PIN_WAKE     PIN_PD3
#define PIN_BAT      A1        // PC1, divisor da bateria

// BAUD do módulo BLE = 2400, FIXO (ver cabeçalho). A esteira de produção grava
// AT+BAUD0 (=2400 nesses clones); 2400 também é o baud mais robusto para o
// SoftwareSerial no oscilador interno de 8MHz (9600 perde byte).
#define BAUD_MODULO  2400

#define MOTOR_MS     1000      // tempo de giro do motor (abrir/fechar real)
#define MOTOR_TST_MS 450       // pulso curto do motor no TESTE de bancada
                               // (menos energia de stall -> menos brownout)
#define JANELA_MS    20000     // ocioso E desconectado: dorme após isso
#define JANELA_TST   60000     // janela estendida durante testes de bancada
#define JANELA_MAX   600000UL  // teto absoluto acordada (10 min) — mesmo conectada

// Gap entre NOTIFICAÇÕES consecutivas. ⚠️ ESTE lote de módulo ("Soft BLE 5.2")
// IGNORA o '\n' e fatia as notificações por TEMPO: writes a <~50ms colam numa
// notificação só (o app espera 2 -> F05). Com 60ms já separava (v2.2.x);
// 150ms dá folga e ainda fica bem dentro do timeout de 3s do app.
#define GAP_NOTIF_MS 150

#define BTN_CURTO_MS   800     // até aqui = toque curto (toggle do motor)
#define BTN_RESET_MS   10000   // segurar até aqui = reset total

#define NUM_LEDS     3
#define LED_BRIGHT   50        // brilho dos WS2812 (0-255) — baixo p/ poupar corrente

// ---- EEPROM (compat com gerar_seed.py / seedGenerator.py legado) ----
#define EE_CALIB        3      // calibrationOk (sentido de giro)
#define EE_CALIB_VERIF  4      // verifierCalibration (calibração feita)
#define EE_SEED01       5
#define EE_SEED02       15
#define EE_VERS_BLE     768    // versão do módulo lida no provisionamento (3/4/0)
#define EE_SERIAL       769    // 11 chars sem "CH"
#define EE_MOD_CFG      910    // 0xC9 = módulo já provisionado (baud+config+nome)
#define MOD_CFG_MAGIC   0xC9
#define EE_HIB          911    // 1 = desligou hibernando (boot seguinte = wake)

// ---- protocolo ----
#define CMD_ABRIR   1
#define CMD_FECHAR  2
#define TOK_CALIB   190720UL   // token de calibração (mesmo do FI_1_5)

// ---- debug serial (UART de HARDWARE, PD1=TX / PD0=RX — o módulo BLE usa a
// PD4/PD5 do SoftwareSerial, não conflita). Ligue um USB-TTL:
//   cabo PRETO (GND)  -> GND da placa       (sempre primeiro!)
//   cabo BRANCO (RX)  -> pad TX da placa
//   cabo VERMELHO     -> NÃO LIGA (a placa se alimenta da bateria)
// Terminal a 9600: tools/.venv-bancada/bin/pyserial-miniterm <porta> 9600
// Só imprime (não lê) — custo mínimo; pode ficar ligado em produção se quiser.
#define FEATURE_SERIAL_DEBUG 0
#define DBG_BAUD 9600
#if FEATURE_SERIAL_DEBUG
  #define DBG(...)   Serial.print(__VA_ARGS__)
  #define DBGLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG(...)
  #define DBGLN(...)
#endif

SoftwareSerial bluetooth(PIN_BLE_TX, PIN_BLE_RX);  // (TX, RX)
CRGB leds[NUM_LEDS];
unsigned long seed01 = 0, seed02 = 0;
uint8_t calibrationOk = 0;
uint8_t g_moduloVers = 0;      // 3 (ver.03), 4 (ver.04+) ou 0 (não leu)
char serialFech[12] = {0};
volatile bool acordouBLE = false, acordouBtn = false;
bool moduloOk = false;
bool g_wakeHib = false;        // este boot foi um "acordar da hibernação"
void atenderBotao();           // usada pelo atenderApp (definida mais abaixo)

// Canal de RESPOSTA (sempre BLE neste build; Stream* mantido p/ um futuro
// modo cabo em placa com pads acessíveis).
Stream* io = &bluetooth;

// Após um reset por watchdog o WDT continua ARMADO em 15ms — sem isto o MCU
// entra em loop infinito de reset. Roda antes do main() (seção .init3).
void wdt_init(void) __attribute__((naked, used, section(".init3")));
void wdt_init(void) { MCUSR = 0; wdt_disable(); }

// Reset REAL do MCU (periféricos e registradores voltam ao estado de power-on,
// como tirar a bateria) — nada de salto-para-zero do firmware antigo.
void resetMCU() { wdt_enable(WDTO_15MS); while (1) {} }

// ---- feedback sonoro/visual --------------------------------------------------
void beep(uint16_t ms, uint16_t freq) { tone(PIN_BUZZER, freq, ms); delay(ms); noTone(PIN_BUZZER); }

// LEDs de status (WS2812). Só usados em janelas com a UART ociosa (boot e
// testes) — FastLED.show desliga IRQ e corromperia um RX BLE em andamento.
void ledCor(const CRGB& c) { fill_solid(leds, NUM_LEDS, c); FastLED.show(); }
void piscar(const CRGB& c, uint8_t vezes, uint16_t ms = 120) {
    for (uint8_t i = 0; i < vezes; i++) {
        ledCor(c); delay(ms);
        ledCor(CRGB::Black); delay(80);
    }
}

// Fanfarra do Rocky (Gonna Fly Now) — a melodia de "INICIALIZAÇÃO 100% OK".
// Curta (~1,8s) p/ não segurar o boot nem drenar bateria.
void melodiaRocky() {
    static const uint16_t f[]  = {392, 523, 659, 0, 392, 523, 698, 0, 659, 698, 784};
    static const uint16_t ms[] = {140, 140, 320, 70, 140, 140, 320, 70, 130, 130, 520};
    for (uint8_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (f[i]) tone(PIN_BUZZER, f[i], ms[i]);
        delay(ms[i] + 20);
    }
    noTone(PIN_BUZZER);
}

// Boot OK: melodia + 3 piscadas verdes (pode conectar/usar).
void sinalPronto() { melodiaRocky(); piscar(CRGB::Green, 3); }

// Boot com módulo BLE mudo: 4 bipes graves + 2 piscadas vermelhas (triagem).
void sinalModuloMudo() {
    for (uint8_t i = 0; i < 4; i++) beep(160, 400);
    piscar(CRGB::Red, 2, 200);
}

// Reset total pelo botão: melodia DESCENDENTE (o contrário da de pronta).
void melodiaReset() {
    static const uint16_t f[] = {784, 659, 523, 392};
    for (uint8_t i = 0; i < 4; i++) beep(120, f[i]);
}

// ---- motor: gira por um tempo e para. Nada mais. ----
void motorPara() { digitalWrite(PIN_MOTOR_A, LOW); digitalWrite(PIN_MOTOR_B, LOW); }
void motorGiraMs(bool sentidoA, uint16_t ms) {
    if (sentidoA) { digitalWrite(PIN_MOTOR_A, HIGH); digitalWrite(PIN_MOTOR_B, LOW); }
    else          { digitalWrite(PIN_MOTOR_A, LOW);  digitalWrite(PIN_MOTOR_B, HIGH); }
    delay(ms);
    motorPara();
}
void motorGira(bool sentidoA) { motorGiraMs(sentidoA, MOTOR_MS); }

// ---- módulo BLE (sempre a 2400) ----------------------------------------------

// Manda um comando AT e descarta a resposta (não dependemos dela — os clones
// nem sempre respondem). O delay dá tempo do módulo processar.
void at(const char* c, uint16_t w = 150) {
    bluetooth.print(c);
    delay(w);
    while (bluetooth.available()) bluetooth.read();
}

// Testa se o módulo responde AT no baud atual. Manda 4× — em módulos com
// economia de energia o 1º AT só ACORDA (não responde); os seguintes respondem.
// Alguns clones não respondem nada a um "AT" pelado: é só triagem, um "mudo"
// aqui NÃO reprova a fechadura (a conexão BLE real é a prova final).
bool bleResponde() {
    for (uint8_t t = 0; t < 4; t++) {
        while (bluetooth.available()) bluetooth.read();
        bluetooth.print("AT");
        unsigned long t0 = millis();
        while (millis() - t0 < 250) {
            if (bluetooth.available()) return true;
        }
    }
    return false;
}

// "Vivo" de verdade: alguns clones não respondem NADA a um "AT" pelado, mas
// respondem a uma consulta. Testa AT e depois AT+VERS? — qualquer byte = vivo.
bool bleVivo() {
    if (digitalRead(PIN_WAKE) == HIGH) return true;  // conectado = vivo (e AT seria tunelado)
    if (bleResponde()) return true;
    while (bluetooth.available()) bluetooth.read();
    bluetooth.print("AT+VERS?");
    unsigned long t0 = millis();
    while (millis() - t0 < 450) {
        if (bluetooth.available()) return true;
    }
    return false;
}

// Lê AT+VERS? e devolve a geração do módulo: 3 (ver.03), 4 (ver.04+) ou 0
// (não respondeu). Igual ao CheckVersBLE do FI_1_5.
uint8_t bleLerVersao() {
    for (uint8_t t = 0; t < 3; t++) {
        while (bluetooth.available()) bluetooth.read();
        bluetooth.print("AT+VERS?");
        char resp[48] = {0};
        uint8_t n = 0;
        unsigned long tt = millis();
        while (millis() - tt < 450) {
            if (bluetooth.available() && n < sizeof(resp) - 1) resp[n++] = bluetooth.read();
        }
        if (strstr(resp, "ver.03") || strstr(resp, "ver.3")) return 3;
        if (strstr(resp, "ver."))  return 4;   // achou versão e não é 03
    }
    return 0;                                  // não leu
}

// Config LEVE — roda em TODO boot (auto-cura de drift), SEM reset, SEM nome,
// SEM mexer no baud: rápida e não derruba conexão nenhuma.
// BEFC020/AFTC028 = valores da esteira de produção p/ ESTA placa (MOSFET no
// PIO8 do módulo + wake no PIO6): PIO8 alto sempre (alimenta os periféricos),
// PIO6 baixo antes / alto depois da conexão (borda que acorda o MCU no PD3).
void configModuloLeve() {
    // Com um cliente CONECTADO o módulo está em modo túnel: não interpreta AT
    // e ainda REPASSA cada comando como notificação — o app receberia
    // "AT+..." no meio do handshake e extrairia números-lixo. Pula.
    if (digitalRead(PIN_WAKE) == HIGH) { DBGLN(F("[cfg] conectado - pula config")); return; }
    at("AT+TYPE0");    // sem pareamento
    at("AT+MODE2");    // túnel de dados (repassa os bytes pro MCU)
    at("AT+ROLE0");    // slave
    at("AT+DELI3");    // delimitador '\n' nos 2 sentidos
    at("AT+NOTI1");    // notify ligado
    at("AT+BEFC020");  // MOSFET(PIO8)=1, wake(PIO6)=0 antes da conexão
    at("AT+AFTC028");  // MOSFET(PIO8)=1, wake(PIO6)=1 depois -> borda de wake
    at("AT+PIO60");    // repouso arma a próxima borda de wake
    if (g_moduloVers == 3) at("AT+STATUS8");  // módulos ver.03: wake por STATUS
}

// Config COMPLETA — só na 1ª vez após gravar (flag EE_MOD_CFG; o seed.bin zera
// o byte, então toda regravação re-provisiona). Espelha a esteira at.js:
// SHIELD1 -> BAUD0(2400) -> PWRM1 -> config -> NAME -> RESET.
void bleProvisionar() {
    DBGLN(F("[prov] provisionamento completo do modulo (1o boot)"));
    beep(50, 1200); beep(50, 1200);            // "configurando o rádio, aguarde"
    // 1) Converge o módulo p/ 2400 ÀS CEGAS: manda AT+BAUD0+AT+RESET em cada
    //    baud candidato. No baud real o módulo obedece; nos outros é lixo
    //    ignorado. (Clones não respondem "OK" a "AT" pelado -> detecção por
    //    resposta dá falso-negativo; às cegas é determinístico.)
    if (!bleVivo()) {                          // já está em 2400? pula a varredura
        DBGLN(F("[prov] mudo a 2400 -> varrendo bauds (AT+BAUD0+RESET as cegas)"));
        const long cands[] = {9600, 38400, 19200, 57600, 4800};
        for (uint8_t i = 0; i < sizeof(cands) / sizeof(long); i++) {
            DBG(F("[prov] tentando baud ")); DBGLN(cands[i]);
            bluetooth.begin(cands[i]);
            delay(30);
            at("AT", 120);                     // acorda (PWRM)
            at("AT+BAUD0", 250);               // -> 2400
            at("AT+RESET", 150);
            delay(600);                        // módulo reinicia
        }
        bluetooth.begin(BAUD_MODULO);
        delay(100);
    }

    // 2) Config completa a 2400 (mesmo lote AT da esteira de produção).
    at("AT+SHIELD1");
    at("AT+BAUD0");                            // reafirma (só vale após reset)
    at("AT+PWRM1");                            // sem auto-sleep do módulo
    g_moduloVers = bleLerVersao();
    DBG(F("[prov] AT+VERS? -> geracao ")); DBGLN(g_moduloVers);
    EEPROM.update(EE_VERS_BLE, g_moduloVers);
    configModuloLeve();
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+NAME%s", serialFech[0] ? serialFech : "CHAVIFI");
    at(cmd, 250);
    // Flag ANTES do reset final: se a bateria afundar durante o reset/espera,
    // o provisionamento não fica re-rodando (pesado) em todo boot.
    EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    at("AT+RESET", 150);
    delay(900);                                // BAUD/NAME valem após o reset
    while (bluetooth.available()) bluetooth.read();
}

void isrBtn() { acordouBtn = true; }
void isrBLE() { acordouBLE = true; }

// Cada linha vira UMA notificação no app (o módulo DELI3 fatia pelo '\n').
void enviaLinha(const char* s) { io->print(s); io->print('\n'); io->flush(); delay(12); }

// "11" em dobro: o app espera 2 notificações — com 2 linhas o completer fecha
// na hora. (Uma linha só também passa, mas só depois do timeout de 5s.)
void envia11Duplo() { enviaLinha("11"); delay(GAP_NOTIF_MS); enviaLinha("11"); }

// ---- abrir/fechar: manda o status e gira (tabela de sentido do FI_1_5) ----
// Confirmação IGUAL à de produção: println(status + bateria) -> "1004.09"
// (status 1000/2000 somado à tensão, float com 2 casas).
void acionar(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    float vb = analogRead(PIN_BAT) * (5.0f / 1024.0f);
    char num[16];
    dtostrf((sentidoA ? 1000.0f : 2000.0f) + vb, 0, 2, num);
    enviaLinha(num);
    motorGira(sentidoA);
}

// ---- calibração (espelha FI_1_5, com o timing que o app precisa) ----

// Recebeu o token 190720 (fim do handshake de calibração).
void calibAceitar() {
    beep(60, 2200); beep(60, 2600);
    delay(1150);           // resposta cai DEPOIS do app armar o listener (delay
    envia11Duplo();        // de 1000ms do calibrarpt1 antes de escutar)
}

// Recebeu "CALIBRACAO-FI": gira o sentido A (= rotateMotor01 do FI_1_5) para o
// instalador ver para que lado a porta vai, e confirma.
void calibGirar() {
    beep(60, 2200);
    motorGira(true);       // 1s de giro — o próprio giro consome o delay
    delay(300);            // total ~1,4s após a recepção
    envia11Duplo();
}

// Recebeu "PORTA-ABERTA"(1) / "PORTA-FECHADA"(0): salva o sentido e faz o giro
// de retorno como o FI_1_5 (FECHADA: volta; ABERTA: volta e vai de novo).
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
    beep(80, 2000); beep(80, 2400);   // calibração concluída
}

// ---- testes de bancada (GUI tools/bancada.py) ----
void testeLeds() {
    const CRGB cores[4] = {CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White};
    for (uint8_t c = 0; c < 4; c++) {
        fill_solid(leds, NUM_LEDS, cores[c]);
        FastLED.show();                 // IRQs off durante o show — só na bancada,
        delay(350);                     // com a UART ociosa (GUI espera a resposta)
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
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
    snprintf(buf, sizeof(buf), "WAKE:v%02u", g_moduloVers);   // 03, 04 ou 00 (não leu)
    enviaLinha(buf);
    enviaLinha("VER:" FW_VERSION);
    enviaLinha("FIM-INFO");
}

void testeBancada(const String& t) {
    if (t.startsWith("TST-PING")) { enviaLinha("PONG"); return; }
    if (t.startsWith("TST-BUZ"))  {
        beep(120, 1500); beep(120, 2000); beep(180, 2500);
        enviaLinha("OK-BUZ"); return;
    }
    if (t.startsWith("TST-LED"))  { testeLeds(); enviaLinha("OK-LED"); return; }
    if (t.startsWith("TST-MOT1")) { enviaLinha("OK-MOT1"); delay(20); motorGiraMs(true,  MOTOR_TST_MS); enviaLinha("FIM-MOT1"); return; }
    if (t.startsWith("TST-MOT2")) { enviaLinha("OK-MOT2"); delay(20); motorGiraMs(false, MOTOR_TST_MS); enviaLinha("FIM-MOT2"); return; }
    if (t.startsWith("TST-BAT"))  { enviaBateria(); return; }
    if (t.startsWith("TST-INFO")) { enviaInfo(); return; }
    if (t.startsWith("TST-ROCKY")) { melodiaRocky(); enviaLinha("OK-ROCKY"); return; }
    // Prova do mecanismo de hibernação. LIÇÃO da bancada (12:37): com um
    // cliente CONECTADO o módulo está em modo túnel e NÃO interpreta AT do
    // MCU (o "AT+PIO80" apareceu como texto no cliente). Por isso a ordem de
    // produção é DROP primeiro (derruba a conexão -> módulo volta ao modo
    // comando) e SÓ ENTÃO o corte. Resultado audível para o operador:
    //   silêncio após a queda      = CORTOU (reconecte: bipe de boot + PONG)
    //   3 bipes graves após ~4s    = módulo não obedeceu o PIO80
    //   "HIB-FALHOU-DROP" na tela  = nem o DROP derrubou (segue conectado)
    if (t.startsWith("TST-HIB")) {
        enviaLinha("OK-HIB");
        DBGLN(F("[hib] DROP -> PIO61 -> PIO80 (receita FI_1_0_400)"));
        delay(400);                          // a resposta sai antes do DROP
        at("AT+DROP", 500);
        delay(500);
        if (digitalRead(PIN_WAKE) == HIGH) { // PD3 espelha a conexão (AFTC/BEFC)
            enviaLinha("HIB-FALHOU-DROP");   // ainda conectado -> túnel -> sem corte
            return;
        }
        at("AT+PIO61", 500);
        EEPROM.update(EE_HIB, 1);
        at("AT+PIO80", 60);
        delay(3000);
        EEPROM.update(EE_HIB, 0);
        DBGLN(F("[hib] ainda vivo = modulo nao cortou"));
        beep(160, 400); beep(160, 400); beep(160, 400);   // 3 graves = falhou
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

// ---- atende o app: desafio -> saltos -> tokens (ignorados) -> comando ----
void atenderApp() {
    io = &bluetooth;
    beep(45, 2600);                     // bipe de wake (acordou por BLE)
    DBGLN(F("[app] acordou - ouvindo (20s)"));
    bluetooth.setTimeout(150);
    unsigned long t0 = millis();
    unsigned long tAbs = millis();
    unsigned long janela = JANELA_MS;
    uint8_t step = 0;
    while (millis() - t0 < janela && millis() - tAbs < JANELA_MAX) {
        // Botão físico funciona SEMPRE que o MCU está acordado (com a
        // hibernação, esta janela é o único momento em que ele está ligado).
        if (digitalRead(PIN_BUTTON) == LOW) { atenderBotao(); t0 = millis(); }
        // Cliente CONECTADO (PD3 alto) mantém a janela viva: o INSTALL do app
        // fica conectado por bem mais de 20s (QR codes, telas) e o timeout
        // antigo derrubava a sessão no meio (AT+DROP) -> F05 no calibrar.
        // Desconectou -> zera o handshake (rodada nova do app começa do zero)
        // e aí sim os 20s ociosos contam até dormir/hibernar.
        if (digitalRead(PIN_WAKE) == HIGH) t0 = millis();
        else if (step != 0) step = 0;
        if (bluetooth.available() <= 0) continue;
        int pk = bluetooth.peek();
        if (pk == '\n' || pk == '\r' || pk == ' ' || pk == '\t') { bluetooth.read(); continue; }

        // comando de TEXTO (bancada / calibração)
        if ((pk >= 'A' && pk <= 'Z') || (pk >= 'a' && pk <= 'z')) {
            String txt = bluetooth.readString(); txt.trim(); txt.toUpperCase();
            DBG(F("[app] txt: ")); DBGLN(txt);
            if (txt.startsWith("TST-"))        { t0 = millis(); janela = JANELA_TST; testeBancada(txt); continue; }
            if (txt.indexOf("PORTA-ABERTA")  >= 0) { calibSalvar(1); return; }
            if (txt.indexOf("PORTA-FECHADA") >= 0) { calibSalvar(0); return; }
            if (txt.indexOf("CALIBRACAO-FI") >= 0) { t0 = millis(); calibGirar(); continue; }
            continue;
        }

        // número (handshake)
        unsigned long v = (unsigned long)bluetooth.parseInt();
        if (v == 0) continue;
        DBG(F("[app] num: ")); DBGLN(v);
        // Comandos primeiro (1/2/190720)...
        if (v == CMD_ABRIR || v == CMD_FECHAR) { acionar(v); return; }
        if (v == TOK_CALIB) { t0 = millis(); calibAceitar(); step = 1; continue; }
        // ...e QUALQUER número na faixa de desafio (0..2,1M) vale como NOVO
        // desafio, mesmo com handshake em andamento: o app faz retry/rodadas
        // (reconecta e manda desafio novo) e o firmware antigo ficava PRESO
        // esperando tokens — o desafio da rodada nova era ignorado e o app
        // "pensava" para sempre. Tokens de verdade são 32 bits (quase nunca
        // caem nessa faixa) e, se caírem, saltos extras são inócuos.
        if (v <= 2100000UL) {
            t0 = millis();
            unsigned long rA = random(1, 9999), rB = random(1, 9999);
            // O QUE O APP ESPERA: 2 notificações, cada uma com UM salto.
            // Neste lote de módulo a separação é por TEMPO (GAP_NOTIF_MS) —
            // 20ms colava os dois numa notificação só (F05); par-numa-linha
            // duplicado fazia o app ler respA 2x e congelar no LFSR.
            char buf[16];
            snprintf(buf, sizeof(buf), (g_moduloVers == 3) ? "%lu\n" : "%lu", rA + v + seed01);
            enviaLinha(buf);
            delay(GAP_NOTIF_MS);
            snprintf(buf, sizeof(buf), (g_moduloVers == 3) ? "%lu\n" : "%lu", rB + v + seed02);
            enviaLinha(buf);
            step = 1; continue;
        }
        // fora da faixa = token (bypass: ignorado)
    }
}

// ---- botão físico: toggle curto / reset total em 10s -------------------------
void atenderBotao() {
    delay(30);                                       // debounce
    if (digitalRead(PIN_BUTTON) != LOW) return;      // ruído
    unsigned long t0 = millis();
    unsigned long seg = 0;
    while (digitalRead(PIN_BUTTON) == LOW) {
        unsigned long dur = millis() - t0;
        if (dur >= BTN_RESET_MS) {                   // 10s: reset total
            melodiaReset();
            piscar(CRGB::Red, 3);
            EEPROM.update(EE_MOD_CFG, 0);            // re-provisiona o rádio no boot
            resetMCU();                              // = tirar e recolocar a bateria
        }
        // a partir de 3s: 1 bipe por segundo, subindo (contagem p/ o reset)
        unsigned long s = dur / 1000;
        if (s >= 3 && s != seg) { seg = s; beep(40, (uint16_t)(1200 + s * 150)); }
        delay(10);
    }
    unsigned long dur = millis() - t0;
    if (dur < BTN_CURTO_MS) {                        // toque curto: motor em toggle
        static bool s = false; s = !s;
        beep(50, s ? 2400 : 1800);                   // agudo=vai, grave=volta
        motorGira(s);
    } else {
        beep(120, 500);                              // segurou e soltou: cancelado
    }
}

// dormir = LowPower.powerDown. O MCU dorme e acorda pela interrupção do pino de
// wake (PD3, borda que o módulo gera ao conectar) ou pelo botão (PD2).
// Motor fica OUTPUT LOW (nunca Hi-Z — evita shoot-through na ponte H).
void dormir() {
    motorPara();
#if FEATURE_HIBERNA_MOSFET
    // Hibernação profunda — receita EXATA do goToSleep() do FI_1_0_400 de
    // produção (lá o LowPower.powerDown está COMENTADO: o sono da placa _400
    // É o corte do trilho): AT+DROP -> AT+PIO61 -> AT+PIO80. Morremos no
    // PIO80; o AFTC028 religa o trilho quando um celular conecta e o boot com
    // EE_HIB=1 vai direto atender o app. Se em 3s ainda estivermos vivos
    // (módulo não obedeceu o corte), cai no powerDown normal.
    at("AT+DROP", 500);
    at("AT+PIO61", 500);
    EEPROM.update(EE_HIB, 1);
    at("AT+PIO80", 60);
    delay(3000);
    EEPROM.update(EE_HIB, 0);
    DBGLN(F("[hib] modulo nao cortou - powerDown normal"));
#endif
    at("AT+DROP", 60); at("AT+PIO60", 60);
    acordouBLE = false; acordouBtn = false;
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrBtn, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_WAKE), isrBLE, RISING);
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
    detachInterrupt(digitalPinToInterrupt(PIN_BUTTON));
    detachInterrupt(digitalPinToInterrupt(PIN_WAKE));
}

void setup() {
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_MOTOR_A, OUTPUT);
    pinMode(PIN_MOTOR_B, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_WAKE, INPUT);
    motorPara();

    // ESTOU VIVO — a PRIMEIRA coisa, antes de tudo. Se não tocar = hardware/energia.
    beep(120, 1800);

#if FEATURE_SERIAL_DEBUG
    Serial.begin(DBG_BAUD);
    DBGLN(F("\n[boot] chavi_fi " FW_VERSION));
#endif

    randomSeed(analogRead(A0) ^ micros());

    // LEDs inicializados e APAGADOS. Nada aceso de forma CONTÍNUA no boot: os 3
    // WS2812 puxam corrente e, numa bateria fraca, seguravam o trilho baixo e
    // reiniciavam a fechadura em loop. Feedback visual = só PISCADAS curtas.
    FastLED.addLeds<WS2812B, PIN_LEDS, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(LED_BRIGHT);
    fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();

    // serial (nome BLE) + estado da EEPROM
    for (uint8_t i = 0; i < 11; i++) {
        uint8_t c = EEPROM.read(EE_SERIAL + i);
        if (c == 0 || c == 0xFF) { serialFech[i] = 0; break; }
        serialFech[i] = c; serialFech[i + 1] = 0;
    }
    calibrationOk = EEPROM.read(EE_CALIB);
    if (calibrationOk > 1) calibrationOk = 0;
    EEPROM.get(EE_SEED01, seed01);
    EEPROM.get(EE_SEED02, seed02);
    g_moduloVers = EEPROM.read(EE_VERS_BLE);
    if (g_moduloVers != 3 && g_moduloVers != 4) g_moduloVers = 0;
    g_wakeHib = (EEPROM.read(EE_HIB) == 1);
    if (g_wakeHib) EEPROM.update(EE_HIB, 0);

    DBG(F("[boot] serial=")); DBG(serialFech[0] ? serialFech : "(fabrica)");
    DBG(F(" calib=")); DBG(calibrationOk);
    DBG(F(" seeds=")); DBG((seed01 && seed02) ? F("ok") : F("VAZIAS"));
    DBG(F(" versBLE=")); DBGLN(g_moduloVers);

    // Rádio: 2400 fixo. 1º boot após gravar = provisionamento completo
    // (converge baud + config + nome + reset); boots seguintes = config leve.
    // Boot de WAKE da hibernação = caminho RÁPIDO: o módulo já está configurado
    // e tem um app conectado ESPERANDO — nada de config/melodia, só atender.
    bluetooth.begin(BAUD_MODULO);
    if (g_wakeHib) {
        DBGLN(F("[boot] wake da hibernacao - atendendo direto"));
        return;                              // loop() atende já no 1º giro
    }
    if (EEPROM.read(EE_MOD_CFG) != MOD_CFG_MAGIC) {
        if (digitalRead(PIN_WAKE) == HIGH) {
            // Cliente conectado no 1º boot: provisionar agora vazaria AT pro
            // app (túnel). Fica p/ o próximo boot limpo (flag continua 0).
            DBGLN(F("[boot] conectado - provisionamento adiado"));
        } else {
            bleProvisionar();
        }
    } else {
        DBGLN(F("[boot] modulo ja provisionado - config leve"));
        configModuloLeve();
    }
    moduloOk = bleVivo();
    DBG(F("[boot] moduloOk=")); DBGLN(moduloOk);

    // Feedback final do boot: Rocky = TUDO PRONTO; graves = módulo mudo.
    // (O "mudo" pode ser falso-negativo em clone que não responde AT — a
    // conexão BLE real é a prova final. Mas serve de triagem na bancada.)
    if (moduloOk) sinalPronto();
    else          sinalModuloMudo();
    DBGLN(F("[boot] PRONTA - dormindo"));
}

void loop() {
    // Janela de escuta logo APÓS o boot, sempre: se alguém conectou DURANTE o
    // boot (bancada/app logo depois de gravar), o módulo já está em modo túnel
    // (não interpreta AT) e os writes dele chegaram enquanto o setup rodava —
    // sem esta janela o MCU ia direto dormir e o AT+DROP do dormir() ainda
    // DERRUBAVA o cliente (visto na bancada: conectou no meio do boot, viu os
    // nossos "AT" como dados e caiu sem PONG). Também cobre o wake da
    // hibernação (app conectado esperando).
    static bool primeiraVolta = true;
    if (primeiraVolta || g_wakeHib) {
        primeiraVolta = false;
        g_wakeHib = false;
        atenderApp();
    }
    dormir();                            // powerDown; acorda no connect (PD3), botão
                                         // ou DADOS no RX (PCINT do SoftwareSerial)
    DBG(F("[wake] btn=")); DBG(acordouBtn); DBG(F(" ble=")); DBGLN(acordouBLE);
    if (acordouBtn) atenderBotao();
    // Atende em QUALQUER wake, não só quando o PD3 subiu: módulos clones
    // ("ver.12") não geram a borda de wake ao conectar, mas os DADOS que o app
    // escreve chegam no RX do SoftwareSerial — cujo pin-change interrupt também
    // acorda o MCU do powerDown. O firmware antigo IGNORAVA esse wake (voltava
    // a dormir) e a fechadura ficava "conecta mas não responde" p/ sempre.
    // O 1º write pode se perder (o byte que acordou chega picotado); o app e a
    // bancada retransmitem, e a janela de 20s pega as tentativas seguintes.
    atenderApp();
}
