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
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "LowPower.h"
#include <FastLED.h>

#define FW_VERSION   "2.8.3"

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

// ---- pinos ----
// COMUNS às duas gerações (FI 1.0 e FI 1.5):
#define PIN_BLE_RX   PIN_PD4
#define PIN_BLE_TX   PIN_PD5
#define PIN_BUZZER   PIN_PD6
#define PIN_BUTTON   PIN_PD2
#define PIN_WAKE     PIN_PD3
#define PIN_BAT      A1        // PC1, divisor da bateria
// DIFERENTES por geração (decidido em RUNTIME pelo byte de placa na EEPROM):
//   FI 1.5: motor PB1/PB2 (pinos 9/10), 3× WS2812 no PB3 (pino 11)
//   FI 1.0: motor PB2/PB3 (pinos 10/11), 3 LEDs discretos nos pinos 7/8/9
#define PIN_LEDS     PIN_PB3   // WS2812 (só FI 1.5 — no 1.0 o PB3 é o motor!)
#define PIN_LED10_1  PIN_PD7   // LEDs discretos do FI 1.0
#define PIN_LED10_2  PIN_PB0
#define PIN_LED10_3  PIN_PB1

// BAUD do módulo BLE = 9600, com CRISTAL EXTERNO 16MHz (config da FROTA DE
// PRODUÇÃO, provada em ~1000 fechaduras). Com clock preciso do cristal, 9600
// é confiável no SoftwareSerial (o que falhava era 9600 no RC interno de 8MHz,
// impreciso — por isso o 2400 anterior). AT+BAUD2 = 9600 nos clones "Soft AT
// 5.2" (igual ao baud9600BLE1010 do firmware antigo FI_1_5).
#define BAUD_MODULO  9600
#define AT_BAUD_CMD  "AT+BAUD2"    // -> 9600 nos clones deste lote

// Motor REAL (abrir/fechar/calibração): igual ao FI_1_5 de produção — gira até
// detectar o BATENTE pela corrente (INA219 no I2C 0x45) ou até o teto duro.
// O pulso por tempo (MOTOR_MS) é só o FALLBACK quando o INA219 não responde.
#define INA_ADDR          0x45
#define MOTOR_STALL_MA    300      // corrente de batente (= stallMotor do FI_1_5)
#define MOTOR_TIMEOUT_MS  10000    // teto duro de giro (= timeoutMotor do FI_1_5)
#define MOTOR_ARRANQUE_MS 300      // ignora o pico de arranque (inrush > 300mA)
#define MOTOR_MS     1000      // fallback por tempo (sem INA219)
// RECUO ("line up" do FI_1_5): após bater no fim de curso, gira um pouco no
// sentido CONTRÁRIO para ALIVIAR a pressão do batente (senão o came fica
// forçando o fim de curso e a próxima abertura pode travar). O legado usa
// timeToLineUP=1000ms; aqui é ajustável. 0 = sem recuo.
#define MOTOR_RECUO_MS   900
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
#define EE_BOARD        912    // 1 = placa FI 1.0; qualquer outro valor = FI 1.5
                               // (gravado pelo seed.bin/gerar_seed.py conforme a placa)

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
Adafruit_INA219 ina219(INA_ADDR);
bool inaOk = false;
bool placa10 = false;                       // true = FI 1.0 (EE_BOARD==1)
uint8_t g_pinMotorA = PIN_PB1;              // default FI 1.5
uint8_t g_pinMotorB = PIN_PB2;
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

// LEDs de status. FI 1.5 = WS2812 (FastLED); FI 1.0 = 3 LEDs discretos ligam/
// desligam juntos (sem cor — o firmware antigo também os trata em bloco).
// Só usados em janelas com a UART ociosa (boot e testes) — FastLED.show
// desliga IRQ e corromperia um RX BLE em andamento.
void ledCor(const CRGB& c) {
    if (placa10) {
        bool on = (c.r || c.g || c.b);
        digitalWrite(PIN_LED10_1, on);
        digitalWrite(PIN_LED10_2, on);
        digitalWrite(PIN_LED10_3, on);
        return;
    }
    fill_solid(leds, NUM_LEDS, c);
    FastLED.show();
}
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

// DIAGNÓSTICO POR BIPES (sem cabo, sem BLE): quando o módulo não responde a
// 2400, varre os bauds perguntando (AT/AT+VERS?) e BIPA AGUDO o índice de onde
// ele respondeu, logo após os 4 graves:
//   1 bipe = 2400 | 2 = 9600 | 3 = 19200 | 4 = 38400 | silêncio = mudo em todos
// É a fechadura dizendo de viva voz em que baud a UART do módulo está.
void diagBaudBipes() {
    const long cand[] = {2400, 9600, 19200, 38400};
    for (uint8_t i = 0; i < 4; i++) {
        bluetooth.begin(cand[i]);
        delay(30);
        bool respondeu = bleResponde();
        if (!respondeu) respondeu = (bleLerVersao() != 0);
        if (respondeu) {
            delay(500);
            for (uint8_t k = 0; k <= i; k++) { beep(140, 2600); delay(180); }
            if (i == 0) {
                // Vivo a 2400 AGORA (estava grogue no teste do boot): reaplica
                // a config na hora — inclusive os PIOs do trilho da placa 1.0.
                configModuloLeve();
                delay(200);
                beep(90, 3200);   // bipe extra super-agudo = config reaplicada
            }
            break;
        }
    }
    bluetooth.begin(BAUD_MODULO);
}

// Reset total pelo botão: melodia DESCENDENTE (o contrário da de pronta).
void melodiaReset() {
    static const uint16_t f[] = {784, 659, 523, 392};
    for (uint8_t i = 0; i < 4; i++) beep(120, f[i]);
}

// ---- motor ----
void motorPara() { digitalWrite(g_pinMotorA, LOW); digitalWrite(g_pinMotorB, LOW); }
void motorLiga(bool sentidoA) {
    if (sentidoA) { digitalWrite(g_pinMotorA, HIGH); digitalWrite(g_pinMotorB, LOW); }
    else          { digitalWrite(g_pinMotorA, LOW);  digitalWrite(g_pinMotorB, HIGH); }
}
// Pulso por tempo — testes de bancada e fallback sem INA219.
void motorGiraMs(bool sentidoA, uint16_t ms) {
    motorLiga(sentidoA);
    delay(ms);
    motorPara();
}
// Giro REAL (abrir/fechar/calibração), padrão do FI_1_5 de produção:
//  1. gira no sentido do comando até o BATENTE (corrente média > MOTOR_STALL_MA
//     no INA219) ou o teto de 10s. O pico de arranque é ignorado
//     (MOTOR_ARRANQUE_MS) p/ não virar falso batente. Sem INA219 -> pulso fixo.
//  2. RECUO ("line up"): gira um pouco no sentido CONTRÁRIO (MOTOR_RECUO_MS) p/
//     aliviar a pressão do fim de curso — igual ao FI_1_5. O recuo é curto e
//     NÃO desfaz a abertura (o came já passou o ponto); só solta o batente.
// O motor NUNCA fica ligado: para no batente, no teto ou no fim de cada etapa.
void motorGira(bool sentidoA) {
    if (!inaOk) {
        motorGiraMs(sentidoA, MOTOR_MS);       // 1. giro (fallback por tempo)
    } else {
        ina219.powerSave(false);
        motorLiga(sentidoA);
        unsigned long t0 = millis();
        while (millis() - t0 < MOTOR_TIMEOUT_MS) {   // 1. giro até o batente
            float mA = 0;
            for (uint8_t i = 0; i < 25; i++) mA += ina219.getCurrent_mA();
            mA /= 25.0f;
            if (millis() - t0 > MOTOR_ARRANQUE_MS && fabs(mA) > MOTOR_STALL_MA) break;
        }
        motorPara();
        ina219.powerSave(true);
    }
    // 2. recuo/line-up (alivia o batente). Pausa curta antes p/ o motor parar
    //    de fato (inércia) e não dar shoot-through na inversão de sentido.
    if (MOTOR_RECUO_MS > 0) {
        delay(80);
        motorGiraMs(!sentidoA, MOTOR_RECUO_MS);
    }
}

// ---- módulo BLE (sempre a 2400) ----------------------------------------------

// Manda um comando AT e descarta a resposta (não dependemos dela — os clones
// nem sempre respondem). O delay dá tempo do módulo processar.
// Terminado em '\r' como o FI_1_0/FI_1_0_400 de produção: o lote de módulos
// ANTIGO (ver.03/04 das FI 1.0) exige o CR; o lote novo (ver.05) tolera —
// o FI_1_0_400 sempre mandou com '\r' nos mesmos módulos "Soft AT 5.2".
void at(const char* c, uint16_t w = 150) {
    bluetooth.print(c);
    bluetooth.print('\r');
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
        bluetooth.print("AT\r");
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
    bluetooth.print("AT+VERS?\r");
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
        bluetooth.print("AT+VERS?\r");
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
    // "AT+..." no meio do handshake e extrairia números-lixo.
    // ⚠️ EXCEÇÃO placa 1.0: NÃO pular — nela esta função também SEGURA O TRILHO
    // DE ENERGIA (gate do mosfet num PIO do módulo). Pular com PD3 alto era um
    // dos motivos das 1.0 mortas (relatório FI10_ANALISE §2).
    if (!placa10 && digitalRead(PIN_WAKE) == HIGH) {
        DBGLN(F("[cfg] conectado - pula config")); return;
    }
    at("AT+PWRM1");    // ANTES de tudo: tira o auto-sleep (senão o 1º AT só acorda)
    at("AT+TYPE0");    // sem pareamento
    // NOME reafirmado a CADA boot (como o changeName do FI_1_5) e ANTES do
    // MODE2 — em MODE2 (túnel) alguns clones ignoram o AT+NAME. Sem isto, o
    // nome só era tentado no 1º boot e, se falhasse, ficava o de fábrica.
    if (serialFech[0]) {
        char nm[24];
        snprintf(nm, sizeof(nm), "AT+NAME%s", serialFech);
        at(nm, 200);
    }
    // ⚠️ AT+MODE2 é o ÚLTIMO comando. Em MODE2 (túnel de dados) o módulo PARA de
    // interpretar AT e passa a repassar tudo como dado — então QUALQUER AT depois
    // dele é IGNORADO. Antes o MODE2 vinha cedo e o AT+NOTI1 (crucial) caía no
    // vão: o módulo repassava app->MCU (RX ok, comandos executavam) mas NÃO
    // NOTIFICAVA o MCU->app (TX perdido) -> "buzzer toca / motor gira, mas o app
    // não recebe PONG/OK". A 2400 colava por timing; a 9600 (CH003FI002734)
    // expôs. Ordem correta: config toda ANTES, MODE2 por último.
    at("AT+ROLE0");    // slave
    at("AT+DELI3");    // delimitador '\n' nos 2 sentidos
    at("AT+NOTI1");    // notify ligado -> módulo NOTIFICA o app com o TX do MCU
    if (placa10) {
        // GATE DO MOSFET que alimenta o MCU. O pino NÃO é fixo: a esteira at.js
        // suporta PIO 4/5/6/7 (default 4) e o upload antigo usava 7/8/9.
        // Erguemos TODOS os candidatos — imediato (PIOx1 religa o trilho JÁ) e
        // persistente na NVM (BEFCFF7 = todos os PIOs altos ANTES da conexão,
        // menos o bit3=PIO6 p/ o wake; AFTCFFF = todos altos DEPOIS).
        at("AT+PIO41"); at("AT+PIO51"); at("AT+PIO71");
        at("AT+PIO81"); at("AT+PIO91");
        at("AT+BEFCFF7");
        at("AT+AFTCFFF");
    } else {
        at("AT+BEFC020");  // MOSFET(PIO8)=1, wake(PIO6)=0 antes da conexão
        at("AT+AFTC028");  // MOSFET(PIO8)=1, wake(PIO6)=1 depois -> borda de wake
    }
    at("AT+PIO60");    // repouso arma a próxima borda de wake
    // Módulos ver.03: wake por STATUS — o opcode difere por placa no firmware
    // de produção: FI 1.0/1.5 sem mosfet usam STATUS6; a linha _400 usa STATUS8.
    if (g_moduloVers == 3) at(placa10 ? "AT+STATUS6" : "AT+STATUS8");
    at("AT+MODE2");    // POR ÚLTIMO: túnel de dados (a partir daqui ignora AT)
}

// Provisionamento COMPLETO com DESCONTAMINAÇÃO — só na 1ª vez após gravar
// (flag EE_MOD_CFG; o seed.bin zera o byte, então toda regravação re-provisiona
// do zero). Três passos, todos ÀS CEGAS (clones não respondem "OK" a "AT"
// pelado — detecção por resposta dá falso-positivo/negativo; determinístico é
// mandar em todos os bauds e deixar o baud certo obedecer):
//
//  PASSO 0 — RESET DE FÁBRICA do módulo (AT+RENEW / AT+DEFAULT) em TODOS os
//    bauds. Limpa QUALQUER resíduo de provisionamentos/experimentos antigos
//    que persiste na NVM do módulo e pode até IMPEDIR O ANÚNCIO: ROLE1
//    (módulo virou central), IMME1 (só anuncia após AT+START), ADTY errado
//    (anúncio não-conectável), PWRM, baud e nome tortos. Volta ao default.
//  PASSO 1 — CONVERGE para 2400: AT+BAUD0 + AT+RESET em cada baud candidato.
//  PASSO 2 — CONFIG COMPLETA a 2400: o lote AT da esteira de produção
//    (SHIELD1/BAUD0/PWRM1) + anti-resíduo (ROLE0/IMME0/ADTY0/START) + a config
//    de dados/wake (configModuloLeve) + NAME + RESET final.
//
// Custo: ~20s, uma vez por gravação (na bancada). A melodia de "pronta" só
// toca no fim — o operador já espera por ela.
void bleProvisionar() {
    DBGLN(F("[prov] provisionamento completo do modulo (1o boot)"));
    beep(50, 1200); beep(50, 1200);            // "configurando o rádio, aguarde"

    static const long TODOS[] = {2400, 9600, 38400, 19200, 57600, 4800, 115200, 1200};
    const uint8_t N = sizeof(TODOS) / sizeof(long);

    // PASSO 0 — SEM RENEW. ⚠️ O AT+RENEW/DEFAULT era veneno em DUAS frentes:
    //  (1.0) apagava o AT+BEFC da NVM que segura o gate do MOSFET de energia ->
    //        o módulo cortava a alimentação do MCU (0718/0629 mortas, §1);
    //  (1.5) interferia na tabela de baud do clone -> o módulo NÃO ficava em
    //        9600 -> 4 bipes graves + sem PONG (visto na CH003FI002734).
    // O firmware FI_1_5 de PRODUÇÃO (9600, ~centenas em campo) NUNCA usa RENEW:
    // só reafirma AT+BAUD2 + config. É o que fazemos no PASSO 1/2.
    if (placa10) {
        // 1.0: religa o TRILHO de energia (mosfet) em todos os bauds, primeiro.
        for (uint8_t i = 0; i < N; i++) {
            bluetooth.begin(TODOS[i]);
            delay(30);
            for (uint8_t k = 0; k < 6; k++) at("AT", 40);   // acorda o PWRM
            at("AT+PWRM1", 120);
            at("AT+PIO41", 60); at("AT+PIO51", 60); at("AT+PIO71", 60);
            at("AT+PIO81", 60); at("AT+PIO91", 60);
            at("AT+BEFCFF7", 80); at("AT+AFTCFFF", 80);
        }
    }

    // PASSO 1 — CONVERGE p/ BAUD_MODULO (9600): em cada baud candidato manda
    // AT+BAUD2 (=9600 no clone) + AT+RESET. No baud atual do módulo o comando
    // pega e ele passa a 9600; nos outros é lixo inócuo. Espelha o
    // baud9600BLE1010 do FI_1_5.
    for (uint8_t i = 0; i < N; i++) {
        bluetooth.begin(TODOS[i]);
        delay(30);
        for (uint8_t k = 0; k < 3; k++) at("AT", 40);   // acorda (PWRM)
        at(AT_BAUD_CMD, 250);                  // -> 9600
        at("AT+RESET", 150);
        delay(600);                            // módulo reinicia no baud novo
    }
    bluetooth.begin(BAUD_MODULO);
    delay(1500);                               // módulo termina de reiniciar
    while (bluetooth.available()) bluetooth.read();

    // PASSO 2 — config completa no baud alvo.
    at("AT+SHIELD1");
    at(AT_BAUD_CMD);                           // reafirma (só vale após reset)
    at("AT+PWRM1");                            // sem auto-sleep do módulo
    at("AT+ROLE0");                            // slave (ROLE1 residual = não anuncia)
    at("AT+IMME0");                            // anuncia sozinho ao ligar
    at("AT+ADTY0");                            // anúncio conectável
    g_moduloVers = bleLerVersao();
    DBG(F("[prov] AT+VERS? -> geracao ")); DBGLN(g_moduloVers);
    EEPROM.update(EE_VERS_BLE, g_moduloVers);
    // O AT+NAME é enviado DENTRO do configModuloLeve, ANTES do AT+MODE2 (em
    // MODE2 o clone ignora o NAME) — e é reafirmado em todo boot. Reforço extra
    // aqui em VÁRIOS BAUDS às cegas: se a convergência de baud não fechou, o
    // nome pega no baud certo mesmo assim (nos outros bauds é lixo inócuo).
    if (serialFech[0]) {
        char nm[24];
        snprintf(nm, sizeof(nm), "AT+NAME%s", serialFech);
        const long bd[] = {2400, 9600, 38400, 19200, 57600, 4800};
        for (uint8_t i = 0; i < sizeof(bd) / sizeof(long); i++) {
            bluetooth.begin(bd[i]); delay(30);
            at("AT", 50); at(nm, 180);
        }
        bluetooth.begin(BAUD_MODULO); delay(100);
    }
    configModuloLeve();
    at("AT+START", 150);                       // se IMME1 residual, inicia o anúncio
    // Flag ANTES do reset final: se a bateria afundar durante o reset/espera,
    // o provisionamento não fica re-rodando (pesado) em todo boot.
    EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    at("AT+RESET", 150);
    delay(1500);                               // BAUD/NAME valem após o reset
    while (bluetooth.available()) bluetooth.read();
    // REAPLICA a config pós-reset (sem novo reset): o módulo recém-reiniciado
    // pode ter comido comandos do lote acima — em especial os PIO71/81/91 e
    // BEFC/AFTC que seguram o TRILHO DE ENERGIA da placa 1.0 (_400). Foi
    // exatamente isso que deixou a 0629 morta na bateria: config cedo demais,
    // módulo grogue, PIO do mosfet nunca subiu.
    configModuloLeve();
}

void isrBtn() { acordouBtn = true; }
void isrBLE() { acordouBLE = true; }

// Cada linha vira UMA notificação no app (o módulo DELI3 fatia pelo '\n').
void enviaLinha(const char* s) { io->print(s); io->print('\n'); io->flush(); delay(12); }

// "11" em dobro: o app espera 2 notificações — com 2 linhas o completer fecha
// na hora. (Uma linha só também passa, mas só depois do timeout de 5s.)
void envia11Duplo() { enviaLinha("11"); delay(GAP_NOTIF_MS); enviaLinha("11"); }

// status "1004.09" = 1000/2000 (sentido) + bateria (float 2 casas), igual produção.
void enviaStatus(bool sentidoA) {
    float vb = analogRead(PIN_BAT) * (5.0f / 1024.0f);
    char num[16];
    dtostrf((sentidoA ? 1000.0f : 2000.0f) + vb, 0, 2, num);
    enviaLinha(num);
}

// ANTI-DUPLICATA: se um comando de acionamento chega logo após outro, é uma
// REEXECUÇÃO espúria (o app reenvia quando o AT+DROP do dormir() derruba a
// conexão antes de ele confirmar o status; ou o loop() reentra no atenderApp).
// Dentro da janela, o firmware NÃO gira de novo — só reconfirma o status
// (o app precisa dele p/ parar de tentar). 6s cobre o giro+recuo mais lento;
// o app tem cooldown de 4,5s entre comandos legítimos, então não atrapalha.
#define ANTIDUP_MS 6000UL
unsigned long g_ultimoAcionamentoMs = 0;

bool acionamentoDuplicado() {
    unsigned long agora = millis();
    if (g_ultimoAcionamentoMs && agora - g_ultimoAcionamentoMs < ANTIDUP_MS) return true;
    return false;
}

// HANDSHAKE COM SALTOS (app legado/cascata): manda o status ANTES de girar —
// o app legado usa o status como confirmação rápida (timeout curto) e desconecta;
// ele não espera o fim do giro. Mantido p/ não quebrar a base instalada.
void acionar(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    enviaStatus(sentidoA);
    if (acionamentoDuplicado()) return;   // reenvio: já confirmou, não gira 2x
    g_ultimoAcionamentoMs = millis();
    motorGira(sentidoA);
    g_ultimoAcionamentoMs = millis();     // o giro consumiu tempo; re-marca
}

// PROTOCOLO DIRETO (verbos ABRIR/FECHAR do app novo): gira PRIMEIRO (até o
// batente + recuo) e manda o status SÓ QUANDO O MOTOR PARA. Assim o app
// confirma "concluído" no momento exato em que a fechadura terminou de girar —
// é o "avisar quando parar". O app espera esta notificação com timeout longo.
void acionarVerbo(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    if (acionamentoDuplicado()) {         // reenvio dentro da janela: NÃO gira,
        enviaStatus(sentidoA);            // só reconfirma p/ o app parar de tentar
        return;
    }
    g_ultimoAcionamentoMs = millis();
    motorGira(sentidoA);      // gira até o fim de curso + recuo (motor PARA aqui)
    g_ultimoAcionamentoMs = millis();     // re-marca no FIM (o giro levou tempo)
    delay(120);               // garante o pacote do status transmitido...
    enviaStatus(sentidoA);    // ...antes do AT+DROP do dormir(). "terminei de girar"
}

// ---- calibração (espelha FI_1_5, com o timing que o app precisa) ----

// Recebeu o token 190720 (fim do handshake de calibração).
void calibAceitar() {
    beep(60, 2200); beep(60, 2600);
    delay(1150);           // resposta cai DEPOIS do app armar o listener (delay
    envia11Duplo();        // de 1000ms do calibrarpt1 antes de escutar)
}

// Recebeu "CALIBRACAO-FI": confirma e gira EXATAMENTE como uma abertura real —
// motorGira faz o giro até o fim de curso (corrente) + recuo. O "11" sai ANTES
// do giro (o app já recebe a confirmação e não estoura o timeout enquanto o
// motor gira). Numa fechadura sem carga na bancada o giro vai até o teto de
// tempo; instalada, para no batente — idêntico ao abrir/fechar.
void calibGirar() {
    beep(60, 2200);
    delay(1150);           // o app arma o listener ~1s após escrever
    envia11Duplo();
    motorGira(true);       // giro REAL (batente + recuo), não mais pulso fixo
}

// Recebeu "CALIB-DEMO" (PROTOCOLO DIRETO, app novo): gira REAL e responde "11"
// SÓ QUANDO O MOTOR PARA — assim o app confirma no fim do giro (sincronizado),
// igual ao abrir/fechar. Resolve o "app fica pensando até o timeout": o "11"
// chega no momento exato em que a fechadura terminou. O app espera com timeout
// longo (15s). NÃO usar no handshake legado (que tem timeout curto e exige o
// "11" antes) — por isso é um verbo separado do "CALIBRACAO-FI".
void calibDemoDireto() {
    beep(60, 2200);
    motorGira(true);       // giro real (batente + recuo) — motor PARA aqui
    delay(120);            // garante o pacote antes de qualquer AT+DROP
    envia11Duplo();        // "11" = terminei de girar
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
    if (placa10) {                      // FI 1.0: acende cada LED discreto e os 3
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
        FastLED.show();                 // IRQs off durante o show — só na bancada,
        delay(350);                     // com a UART ociosa (GUI espera a resposta)
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

// Teste de motor com LAUDO ELÉTRICO: gira o pulso de bancada medindo a
// corrente no INA219 e reporta a média. Interpretação (p/ o laudo do
// fornecedor): ~0-15mA = circuito ABERTO (fio/motor desconectado — a ponte
// acionou e nada fluiu); ~30-300mA = motor girando; >300mA = travado.
void motorTesteCorrente(bool sentidoA, const char* fim) {
    char buf[20];
    if (inaOk) {
        ina219.powerSave(false);
        motorLiga(sentidoA);
        delay(120);                          // ignora o pico de arranque
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
    snprintf(buf, sizeof(buf), "INA:%s", inaOk ? "OK" : "SEM");  // batente por corrente
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "PLACA:%s", placa10 ? "1.0" : "1.5");
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
    if (t.startsWith("TST-MOT1")) { enviaLinha("OK-MOT1"); delay(20); motorTesteCorrente(true,  "FIM-MOT1"); return; }
    if (t.startsWith("TST-MOT2")) { enviaLinha("OK-MOT2"); delay(20); motorTesteCorrente(false, "FIM-MOT2"); return; }
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
            // PROTOCOLO DIRETO (app novo, sem handshake): o app sonda com
            // TST-PING ao conectar (o firmware LEGADO ignora texto — parseInt
            // dá 0); se veio PONG, ele fala estes verbos. Confirmação idêntica
            // à do handshake ("1004.09" = status+bateria).
            if (txt.startsWith("ABRIR"))       { acionarVerbo(CMD_ABRIR);  return; }
            if (txt.startsWith("FECHAR"))      { acionarVerbo(CMD_FECHAR); return; }
            if (txt.indexOf("PORTA-ABERTA")  >= 0) { calibSalvar(1); return; }
            if (txt.indexOf("PORTA-FECHADA") >= 0) { calibSalvar(0); return; }
            if (txt.indexOf("CALIB-DEMO")    >= 0) { t0 = millis(); calibDemoDireto(); continue; }
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
    // Hibernação profunda por corte do trilho — receita do goToSleep() do
    // FI_1_0_400. ⚠️ DESLIGADA na placa 1.0: o pino do gate ainda não está
    // confirmado (4/5/7/8/9) e um corte que não religue deixa a fechadura
    // MORTA (é o que estamos combatendo). Na 1.0, sono = powerDown normal, que
    // NÃO corta a energia (relatório FI10_ANALISE §3 recomenda até validar).
    if (!placa10) {
        at("AT+DROP", 500);
        at("AT+PIO61", 500);
        EEPROM.update(EE_HIB, 1);
        at("AT+PIO80", 60);
        delay(3000);
        EEPROM.update(EE_HIB, 0);
        DBGLN(F("[hib] modulo nao cortou - powerDown normal"));
    }
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
    // PLACA primeiro de tudo: os pinos do motor dependem dela (no FI 1.0 o
    // PB3 é motor, não LED — configurar errado chacoalharia o motor).
    placa10 = (EEPROM.read(EE_BOARD) == 1);
    if (placa10) { g_pinMotorA = PIN_PB2; g_pinMotorB = PIN_PB3; }

    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(g_pinMotorA, OUTPUT);
    pinMode(g_pinMotorB, OUTPUT);
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
    // FI 1.0: LEDs discretos (7/8/9); o FastLED NUNCA é inicializado (o PB3 é
    // o motor B nessa placa — bit-bang de WS2812 ali chacoalharia o motor).
    if (placa10) {
        pinMode(PIN_LED10_1, OUTPUT); digitalWrite(PIN_LED10_1, LOW);
        pinMode(PIN_LED10_2, OUTPUT); digitalWrite(PIN_LED10_2, LOW);
        pinMode(PIN_LED10_3, OUTPUT); digitalWrite(PIN_LED10_3, LOW);
    } else {
        FastLED.addLeds<WS2812B, PIN_LEDS, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(LED_BRIGHT);
        fill_solid(leds, NUM_LEDS, CRGB::Black); FastLED.show();
    }

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

    // INA219 (detecção de batente do motor). Se não responder no I2C, o giro
    // cai no fallback por tempo — nunca trava o boot.
    inaOk = ina219.begin();
    if (inaOk) ina219.powerSave(true);
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
    if (moduloOk) {
        sinalPronto();
    } else {
        sinalModuloMudo();
        diagBaudBipes();     // bipa o baud onde o módulo respondeu (ver acima)
    }
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
