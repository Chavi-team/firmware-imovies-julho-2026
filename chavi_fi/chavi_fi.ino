/*
 * chavi_fi.ino — Firmware BYPASS das fechaduras Chavi FI (setor imobiliário).
 *
 * Filosofia: confiabilidade > segurança (decisão do cliente). Não valida seed,
 * não valida token, aceita tudo — mas fala o MESMO protocolo dos ~1000 apps em
 * campo (desafio -> 2 saltos -> 3 writes -> comando), então nada muda no app.
 *
 * BAUD DO MÓDULO = 2400-slow (AT+BAUD0) — ver `BAUD_MODULO` / `AT_BAUD_CMD`,
 * que são a fonte da verdade. ⚠️ v2.23: este cabeçalho dizia "9600" enquanto o
 * código define 2400 desde a v2.16 — num arquivo cuja história é uma sucessão
 * de flip-flops 2400↔9600, comentário mentiroso é risco operacional, não
 * cosmética. A escolha do 2400-slow NÃO é por consumo (o PWRM1 dá os mesmos
 * ~0,65 mA em qualquer baud, manual 1010 p.40): é porque o wake do módulo POR
 * DADO NA UART só existe em AT+BAUD0 + AT+UART1. Em 9600 o módulo em PWRM1 só
 * acorda com GND no pino 24 (WAKE), que nesta placa não é acionado por nada.
 * O provisionamento do 1º boot converge módulos em outros bauds via sweep às
 * cegas. Boots seguintes = só config leve (sem reset).
 *
 * ⚠️⚠️ v2.23 — EM PLACA COM BEFC000/AFTC028 O MCU NUNCA CONFIGURA O MÓDULO.
 * Medido pelo ar na 003FI002910 (09/08/2026): BEFC=000 (tudo BAIXO desconectado
 * = trilho cortado) e AFTC=028 (PIO8 gate + PIO6 wake ALTOS ao conectar). Logo
 * a placa SÓ tem energia enquanto há cliente conectado — e conectado o PD3 está
 * ALTO, condição em que o at() se recusa a enviar (corretamente, senão o AT
 * vaza no túnel MODE2 e vai parar no celular). Resultado: nenhum AT é enviado
 * JAMAIS, e daí vêm MOD:SEM-AT, MODF:? e WAKE:v00 — que NÃO são defeito do
 * módulo. A configuração dessas placas tem de ser feita PELO AR: em AT+MODE2
 * (padrão de fábrica) o manual garante que o módulo "permite comandos AT vindos
 * do dispositivo remoto" e deixa o remoto controlar PIO5..PIO11 — o gate (PIO8)
 * está nessa faixa. É assim que se atende os 2.000+ FI em campo sem abrir nenhum.
 *
 * ⭐ IDENTIFICAÇÃO DO MÓDULO (v2.10, manuais oficiais Soft 1010 REV11 + 5.2 R05):
 * a frota mistura DOIS chips que falam a mesma AT — o AT+VERS? diz qual é:
 *   "Soft AT 5.2 ver.XX"           -> BLE 5.2 (EFR32BG22)
 *   "Soft AT ver.XX"/"Soft ATm..." -> BLE-1010 (CSR-1010, BT 4.1)
 * A FAMÍLIA+REV dirigem a config: AT+STATUS só existe no 5.2; no 5.2 rev<04 o
 * BEFC/AFTC/PIO/COL são QUEBRADOS (respondem só 0x000 — corrigido na REV04,
 * histórico do manual R05) e o STATUS é o único wake que funciona; no 1010 o
 * wake é SEMPRE o AFTC. Família (EEPROM 915) e rev (768) ficam persistidas.
 *
 * PINO DO MOSFET parametrizado (EEPROM 914, default 8): a esteira legada usou
 * PIO 4..9 conforme a placa — as máscaras BEFC/AFTC e o corte da hibernação
 * (AT+PIOx0) são calculadas em runtime do byte gravado pelo gravar.sh/bancada.
 *
 * AUTO-CURA DO NOME (v2.10): após o AT+NAME o firmware LÊ DE VOLTA (AT+NAME?)
 * e compara com o serial — UART marginal garble o último byte (caso real:
 * "002FI00187<") e antes só a bancada consertava; agora todo boot conserta.
 *
 * FEEDBACK SONORO/VISUAL (o instalador entende o que está acontecendo):
 *   ligar bateria: 1 bipe curto ......... "estou vivo" (se não tocar = energia)
 *   fim do boot OK: MELODIA (fanfarra do Rocky) + 3 piscadas VERDES = PRONTA
 *   fim do boot com módulo mudo: 4 bipes GRAVES + 2 piscadas VERMELHAS
 *   conectou o celular: 1 bipe curto agudo (acordou por BLE; se conectar e NÃO
 *     bipar, o pino de wake não subiu — módulo/solda)
 *   comando executado (abrir/fechar): 3 piscadas VERDES e DEPOIS a fanfarra do
 *     Rocky (⚠️ v2.22 — nesta ordem de propósito: a luz sai na janela de energia
 *     garantida e só o som fica exposto ao corte; ver fbComandoOk)
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
 *   TST-INFO  -> SER/CAL/SEEDS/MOD/MODF/INA/PLACA/MOSFET/WAKE/VER, "FIM-INFO"
 *   TST-UART  -> rajada-padrão "UARTn:0123456789ABCDEF" ×5 (a bancada confere a
 *                integridade — diagnóstico de UART marginal SEM multímetro)
 *   TST-ECO<x>-> "ECO:<x>" (eco do payload — testa RX+TX do MCU pelo túnel)
 *   TST-ALL   -> roda tudo em sequência, "FIM-TST"
 *
 * TIMING DA CALIBRAÇÃO (pegadinha do app): depois de escrever (tokens+190720,
 * ou CALIBRACAO-FI) o app espera 1000ms ANTES de armar o listener de
 * notificação — e o stream não tem histórico. Resposta enviada cedo demais se
 * PERDE e vira CALIBRACAOERROR. Por isso as respostas "11" da calibração são
 * seguradas ~1,2-1,5s e enviadas EM DOBRO (o app espera 2 notificações).
 *
 * ATmega328/328PB @ 16MHz CRISTAL (MiniCore; a placa FI 1.5 tem StepUp 5V
 * MT3608 -> MCU a 5V, dentro do SOA de 16MHz que exige VCC>=3,78V). Módulo BLE
 * em SoftwareSerial (PD4/PD5 — nesses pinos NÃO existe UART de hardware em
 * nenhum 328; o USART1 do 328PB fica em PB3/PB4 = candidato p/ revisão de
 * placa). Com AT+DELI3: cada write BLE chega na UART terminado em '\n', e cada
 * linha que enviamos vira UMA notificação (o '\n' é consumido).
 */
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include <avr/sleep.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "LowPower.h"
#include <FastLED.h>

#define FW_VERSION   "2.23.1"

// ---- HIBERNAÇÃO PROFUNDA via MOSFET — DUAS GERAÇÕES de hardware --------------
// GERAÇÃO 1 — gate em PIO ENDEREÇÁVEL (retrofit _400 da era FI 1.0, at.js;
// EEPROM 914 = 4..9, 90% = PIO8):
//   AT+PIO80  -> corta o trilho NA HORA (o MCU DESLIGA; consumo ~zero)
//   AT+AFTC028 -> ao CONECTAR o módulo religa o PIO8 -> o MCU dá boot e atende
//   AT+BEFC020 -> ao DESCONECTAR religa também (o MCU boota, faz manutenção e
//                 corta de novo) — é o ciclo do FI_1_0_400 de produção.
// GERAÇÃO 2 — MOSFET "AUTOMÁTICO" (⭐ v2.13; placa v2.7 integrada + retrofit
// padrão 2024; EEPROM 914 = 12): o gate liga no PINO FÍSICO 12 do módulo =
// PIO2 = VCC da EEPROM DO PRÓPRIO MÓDULO (manual 1010, tabela de pinos) — NÃO
// endereçável por AT (AT+PIO cobre só PIO3..11; máscaras BEFC/AFTC = 9 bits).
// O pino SEGUE O ESTADO DO MÓDULO: acordado = alto = placa LIGADA; auto-sleep
// (AT+PWRM1) = baixo = placa CORTADA. A conexão BLE acorda o módulo -> PIO2
// sobe -> a placa religa e o MCU dá boot frio. Corte e religa são 100%
// automáticos: nenhum comando existe nem é necessário — a ÚNICA alavanca é
// PWRM1 (em vez do PWRM0 padrão) no provisionamento. Módulo alimentado direto
// da bateria (fora da chave); todo o resto atrás dela (~0,65mA total ocioso).
// Custo (as duas gerações): trilho cortado = BOTÃO FÍSICO morto (MCU desligado).
// ✅ G1 PROVADA em bancada (05/07 12:42, CH003FI003066 v2.2.2): TST-HIB cortou
// (silêncio) e a reconexão religou o MCU com PONG imediato. LIGADO.
// 🟡 G2: validar em bancada (teste por UPTIME — ver TST-INFO/bancada v2.13).
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

// ⭐⭐ BAUD do módulo BLE = 2400-SLOW (AT+BAUD0) — DECISÃO 02/08/2026, volta à
// config da PRODUÇÃO LEGADA (esteira at.js: BAUD0 + PWRM1 em toda a frota).
// PORQUÊ (tabela do AT+PWRM, manual 1010 §54): o auto-sleep do módulo (PWRM1)
// só permite ACORDAR POR DADO na UART quando o baud é BAUD0/2400-slow; nos
// demais bauds o módulo dormindo só acorda por pulso GND no pino 24 (WAKE) —
// que na placa tem apenas pull-up (R22), ninguém pulsa. Provado na 2910 em
// 02/08: 9600 + PWRM1 = MCU nunca recebe nada (zero PONG, F07 no app).
// E PWRM1 é OBRIGATÓRIO p/ a hibernação: com PWRM0 o módulo fica acordado e o
// TX da UART dele (3,3V constante) ALIMENTA DE FORMA PARASITA o MCU da placa
// CORTADA -> boots fracos em loop (bipe a cada ~1s). Dormindo, ele solta o TX.
// Bônus: rádio 1,5mA -> 0,65mA (metade da conta de bateria).
// CLOCK: cristal externo 16MHz. A 2400 o SoftwareSerial tem ~6.700 ciclos por
// bit (contra ~1.700 a 9600) — é a config MAIS folgada, não menos. O trauma
// histórico do 9600 vinha do RC interno de 8MHz (±10%), que não existe mais.
// Módulo VIRGEM sai de fábrica em 9600 -> o provisionamento converte (o sweep
// manda AT+BAUD0 em cada baud candidato; a bancada faz o mesmo pelo ar).
#define BAUD_MODULO  2400
#define AT_BAUD_CMD  "AT+BAUD0"    // -> 2400-slow (wake por dado + PWRM1)

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
#define MOTOR_RECUO_ABORT_MS 250   // recuo curto quando o giro abortou por queda
                                   // de trilho (alivia o came sem afundar de novo)
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
#define EE_VERS_BLE     768    // REV do firmware do módulo (3, 5, 12... 0=não leu)
#define EE_SERIAL       769    // 11 chars sem "CH"
#define EE_MOD_CFG      910    // 0xC9 = módulo já provisionado (baud+config+nome)
#define MOD_CFG_MAGIC   0xC9
#define EE_HIB          911    // 1 = desligou hibernando (boot seguinte = wake)
#define EE_BOARD        912    // 1 = placa FI 1.0; qualquer outro valor = FI 1.5
#define EE_HIBERNA      913    // 1 = HIBERNAÇÃO por corte de MOSFET ligada (default
                               // 0 = sono leve IDLE, seguro). Ativa via HIB-ON após
                               // validar o ciclo corta->religa na bancada (TST-HIB).
                               // (gravado pelo seed.bin/gerar_seed.py conforme a placa)
#define EE_MOSFET       914    // Gate do MOSFET (gravado pelo gravar.sh/bancada):
                               //   4..9 = PIO endereçável (geração 1; default 8)
                               //   12   = MOSFET AUTOMÁTICO no pino físico 12 =
                               //          PIO2/VCC-EEPROM do módulo (geração 2,
                               //          placa v2.7 — corte via AT+PWRM1)
                               // fora da faixa/0xFF = default 8.
#define EE_MOD_FAM      915    // família do módulo (FAM_*), persistida na identificação
// 916: QUEIMADO — foi a "variante sem MOSFET" (v2.11.0, removida na v2.11.1:
// provado em bancada+app que a placa sem MOSFET funciona com a config NORMAL,
// pois o MCU dela é sempre alimentado). Não reusar o byte sem apagar a frota.
// ⭐⭐ v2.18 — TELEMETRIA DE SOAK (medir em vez de interpretar). Zerada por
// TST-ZERA no início de cada bateria de testes.
#define EE_BOOTS        918    // u16: quantas vezes o MCU bootou
#define EE_BODS         920    // u16: bootou por BROWN-OUT (queda de tensão!)
#define EE_CUTS         922    // u16: vezes que o firmware EXECUTOU o corte
                               // (CUTS ~ BOOTS = o corte está funcionando;
                               //  CUTS subindo e BOOTS parado = o módulo
                               //  ignorou o comando de corte)
#define EE_SOAK         924    // ⭐ v2.23: 1 = MODO SOAK (bancada) — só então a
                               // telemetria BOOTS/BODS/CUTS grava EEPROM. Em
                               // campo fica 0 e NENHUMA escrita acontece, senão
                               // são ~480 escritas/dia (100k = fim de vida da
                               // célula em ~7 meses). Liga/desliga por TST-SOAK.
                               // (925 fica livre: EE_SOAK é 1 byte)
#define EE_PROV_TENT    917    // ⭐ v2.13.3: tentativas de provisionamento pesado
                               // (anti-loop-de-suicídio: nas placas com mosfet, o
                               // AT+RESET do provisionamento reinicia o módulo,
                               // os PIOs caem no boot dele e o gate CORTA o
                               // próprio MCU -> boot -> provisiona -> corta...
                               // bipe agudo a cada ~2s. Teto de 3 tentativas.)

// ---- família do módulo BLE (identificada pelo AT+VERS? — manuais Soft) ----
#define FAM_DESCONHECIDA 0
#define FAM_1010         1     // "Soft AT ver.XX" / "Soft ATm ver.XX" (CSR-1010)
#define FAM_52           2     // "Soft AT 5.2 ver.XX" (EFR32BG22)

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
uint8_t g_moduloVers = 0;      // REV do módulo (3, 5, 12...; 0 = não leu)
uint8_t g_moduloFam = FAM_DESCONHECIDA;   // família (AT+VERS? — 1010 × 5.2)
uint8_t g_pinMosfet = 8;       // gate do MOSFET (EEPROM 914; default 8 = frota;
                               // 12 = automático/pino12 — ver cabeçalho)
// MOSFET-AUTO (geração 2): gate no pino físico 12 = PIO2 = VCC da EEPROM do
// módulo. Inendereçável por AT — o corte é o auto-sleep do módulo (PWRM1).
bool mosfetAuto() { return g_pinMosfet == 12; }
char serialFech[12] = {0};
volatile bool acordouBLE = false, acordouBtn = false;
bool moduloOk = false;
bool g_wakeHib = false;        // este boot foi um "acordar da hibernação"
bool g_hiberna = false;        // HIBERNAÇÃO por corte de MOSFET ligada (EE_HIBERNA)
uint8_t g_atOk = 2;             // 2=não testado · 1=módulo respondeu ao MCU · 0=mudo
uint16_t g_vccMinGiro = 0;       // menor VCC visto no último giro (mV)
bool g_motorAbortouVcc = false;  // giro parou por queda de trilho (v2.17)
uint16_t lerVccMv();             // (definida antes do setup)
bool g_sessaoConectada = false; // já tocou a melodia de "conectou" nesta sessão BLE
void atenderBotao();           // usada pelo atenderApp (definida mais abaixo)

// Canal de RESPOSTA (sempre BLE neste build; Stream* mantido p/ um futuro
// modo cabo em placa com pads acessíveis).
Stream* io = &bluetooth;

// ⭐⭐ v2.23 — UMA ÚNICA função .init3 (era duas, e AMBAS faziam `MCUSR = 0`).
// A ordem entre funções da mesma seção não é garantida pela linguagem: hoje o
// GCC emite capturaMcusr antes (conferido no .lst: 0x234 vs 0x23c), mas isso é
// o INVERSO da ordem do fonte. Uma troca de toolchain ou LTO inverteria e o
// wdt_init zeraria o MCUSR antes da captura -> TODA a telemetria de reset iria
// a zero em silêncio. Fundir é a receita canônica.
// Também é obrigatório desarmar o WDT aqui: após um reset por watchdog ele
// continua ARMADO em 15ms e o MCU entraria em loop infinito de reset.
uint8_t g_mcusr __attribute__((section(".noinit")));
void initReset(void) __attribute__((naked, used, section(".init3")));
void initReset(void) { g_mcusr = MCUSR; MCUSR = 0; wdt_disable(); }

// Reset REAL do MCU (periféricos e registradores voltam ao estado de power-on,
// como tirar a bateria) — nada de salto-para-zero do firmware antigo.
void resetMCU() { wdt_enable(WDTO_15MS); while (1) {} }

// ---- feedback sonoro/visual --------------------------------------------------
void beep(uint16_t ms, uint16_t freq) { tone(PIN_BUZZER, freq, ms); delay(ms); noTone(PIN_BUZZER); }

// ⭐ v2.22 — INICIALIZAÇÃO DOS LEDs IDEMPOTENTE. Extraída do setup porque o
// esperaEnergiaReal() (que roda ANTES) também precisa apagar os WS2812 de
// verdade, e não só baixar o pino de dados. Chamar FastLED.addLeds duas vezes
// empilharia dois controllers no mesmo pino — daí a flag.
// ⚠️ No FI 1.0 o FastLED NUNCA é inicializado: PIN_LEDS (PB3) é o MOTOR B ali,
// e um bit-bang de WS2812 nesse pino chacoalharia o motor.
bool g_ledsInit = false;
void ledsInit() {
    if (g_ledsInit) return;
    g_ledsInit = true;
    if (placa10) {
        pinMode(PIN_LED10_1, OUTPUT); digitalWrite(PIN_LED10_1, LOW);
        pinMode(PIN_LED10_2, OUTPUT); digitalWrite(PIN_LED10_2, LOW);
        pinMode(PIN_LED10_3, OUTPUT); digitalWrite(PIN_LED10_3, LOW);
    } else {
        FastLED.addLeds<WS2812B, PIN_LEDS, GRB>(leds, NUM_LEDS);
        FastLED.setBrightness(LED_BRIGHT);
    }
}

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
// ⭐ v2.22 — ENCURTADA de 2340ms para 1590ms: cortada a 1ª das duas frases
// quase idênticas (392/523/659), mantendo a 2ª + o final (659/698/784), que é
// o que dá o caráter da fanfarra. PORQUÊ: o feedback INTEIRO (piscadas +
// melodia) tem de caber na janela em que a placa ainda tem energia garantida —
// o app segura a conexão 2500ms após a 2ª confirmação (funcao_blue_tooth.dart,
// "DEIXA A FANFARRA TOCAR") e a versão longa estourava esse teto em 440ms,
// truncando justamente a nota final. Agora: 600 (piscadas) + 1590 = 2190ms,
// com 310ms de folga. Bônus: 750ms a menos de MCU acordado por acionamento.
void melodiaRocky() {
    static const uint16_t f[]  = {392, 523, 698, 0, 659, 698, 784};
    static const uint16_t ms[] = {140, 140, 320, 70, 130, 130, 520};
    for (uint8_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (f[i]) tone(PIN_BUZZER, f[i], ms[i]);
        delay(ms[i] + 20);
    }
    noTone(PIN_BUZZER);
}

// ERRO de BLE / módulo mudo: 4 bipes GRAVES + 2 piscadas VERMELHAS.
void sinalModuloMudo() {
    for (uint8_t i = 0; i < 4; i++) beep(160, 400);
    piscar(CRGB::Red, 2, 200);
}

// CONECTOU por BLE: aviso CURTO de sucesso (2 notas ascendentes "ta-dá") + verde.
// ⚠️ NÃO usa a melodia Rocky: o app RECONECTA a cada abrir/fechar, então o
// OK+CONN dispara a cada comando — a fanfarra tocaria toda vez (pesado). Duas
// notas subindo (sol5→dó6) remetem a "conectou!" em ~200ms. A Rocky completa
// fica só sob demanda (TST-ROCKY).
void sinalConectado() { beep(70, 784); beep(130, 1047); piscar(CRGB::Green, 2, 90); }

// ABRIR/FECHAR com SUCESSO: 3 piscadas VERDES + fanfarra do Rocky = "conseguiu!".
// A melodia toca 1× por acionamento concluído (não pesa como pesaria na conexão,
// que repete a cada reconexão do app — por isso a CONEXÃO fica com o aviso curto
// de 2 notas e o ACIONAMENTO ganha a fanfarra completa).
//
// ⭐⭐ v2.22 — A ORDEM É O FIX (LED ANTES, SOM DEPOIS). Causa-raiz do caso real
// 05/08 (2910): a fechadura passou a NOITE com os 3 LEDs verdes acesos, botão
// morto, ~14mA drenando à toa. Os WS2812 GUARDAM o último valor recebido; se a
// placa perde energia no meio de uma piscada, o "apagar" seguinte nunca roda e
// o verde fica LATCHADO — e como o trilho cortado não vai a 0V (o módulo BLE
// fica fora do MOSFET e injeta corrente pelos diodos de clamp do MCU), sobra
// tensão de sobra para o LED brilhar mesmo com o MCU morto pelo brown-out.
// Com a melodia PRIMEIRO, as piscadas caíam em 2340..2940ms — exatamente onde
// o app solta a conexão (2500ms). Invertendo, todo o risco visual se concentra
// nos primeiros 600ms (energia garantida) e o que sobra na zona de risco é só
// buzzer: perder um pedaço de som é cosmético, perder o LED custa a bateria.
// ⚠️ NÃO reordenar de volta sem reler isto. E o feedback só entra DEPOIS da 2ª
// confirmação (acionarVerbo) — o app não pode esperar por som/luz.
void fbComandoOk() {
    piscar(CRGB::Green, 3);   // 600ms — termina APAGADO, bem longe do corte
    melodiaRocky();           // 1590ms — se truncar aqui, o dano é só sonoro
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
        if (!respondeu) respondeu = (bleIdentificar() != 0);
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
// ⭐⭐ v2.17 — PROTEÇÃO DE TRILHO (fim do reset no meio do giro). MEDIDO com
// tools/simula_app.py na 2910: a 2ª confirmação (fim do giro) NUNCA chegava e a
// fechadura dava o bipe de BOOT logo após o motor = o MCU estava MORRENDO no
// giro. Motivo: o motor puxa do trilho de 12V; com a bateria meia-boca e a
// resistência em série do retrofit (Rds-on do mosfet + fios finos + soldas), o
// VCC cai até o brown-out (2,7V) e o MCU reinicia — o app fica sem a 2ª
// confirmação, sem melodia, e o comando seguinte pega a placa em estado ruim
// (F07). Pior no caso SEM CARGA MECÂNICA (bancada), onde o batente nunca é
// atingido e o motor roda os 10s inteiros do teto.
// Agora o giro VIGIA o próprio VCC (bandgap, ~1ms) e PARA por conta própria se
// o trilho afundar — a fechadura conclui o comando, responde e toca a melodia
// em vez de morrer. Numa porta real o batente chega antes e isto nunca dispara.
#define VCC_MIN_BOOT_MV  4200      // abaixo disto o boot é PARASITA (real = ~5V)
#define VCC_MOTOR_MIN_MV 4300      // trilho saudável = 5V (StepUp); abaixo disto
                                   // o próximo passo é o brown-out
void motorGira(bool sentidoA) {
    g_motorAbortouVcc = false;
    g_vccMinGiro = 0;
    if (!inaOk) {
        motorGiraMs(sentidoA, MOTOR_MS);       // 1. giro (fallback por tempo)
    } else {
        // ⭐⭐ v2.23 — WATCHDOG ARMADO DURANTE O GIRO. É a única rede de
        // segurança contra "motor ligado para sempre": mesmo com o timeout da
        // Wire, qualquer travamento neste laço (I2C, EMI, bug) agora resulta em
        // reset em 8s — e o reset desliga a ponte H, porque os pinos do motor
        // voltam a INPUT no power-on. Sem isto, travar aqui = motor girando até
        // a bateria acabar. wdt_reset() a cada volta; wdt_disable() ao sair.
        wdt_enable(WDTO_8S);
        ina219.powerSave(false);
        motorLiga(sentidoA);
        unsigned long t0 = millis();
        while (millis() - t0 < MOTOR_TIMEOUT_MS) {   // 1. giro até o batente
            wdt_reset();
            float mA = 0;
            for (uint8_t i = 0; i < 25; i++) mA += ina219.getCurrent_mA();
            mA /= 25.0f;
            if (millis() - t0 > MOTOR_ARRANQUE_MS && fabs(mA) > MOTOR_STALL_MA) break;
            // trilho afundando? para AGORA (antes do brown-out levar o MCU)
            if (millis() - t0 > MOTOR_ARRANQUE_MS) {
                uint16_t v = lerVccMv();
                if (v && (g_vccMinGiro == 0 || v < g_vccMinGiro)) g_vccMinGiro = v;
                if (v && v < VCC_MOTOR_MIN_MV) { g_motorAbortouVcc = true; break; }
            }
        }
        motorPara();
        ina219.powerSave(true);
        wdt_disable();
    }
    // 2. recuo/line-up (alivia o batente). Pausa curta antes p/ o motor parar
    //    de fato (inércia) e não dar shoot-through na inversão de sentido.
    // ⭐ v2.17: se o giro foi ABORTADO por queda de trilho, PULA o recuo (outro
    //    giro afundaria de novo) e dá um tempo p/ a bateria se recuperar — o
    //    que importa agora é concluir o comando (status + melodia) sem morrer.
    if (g_motorAbortouVcc) {
        // Trilho afundou: espera a bateria se recuperar e, SE ela voltar a um
        // nível saudável, faz um recuo CURTO. Pular o recuo por completo era
        // elétricamente seguro mas deixava o came pressionando o fim de curso
        // (próxima abertura dura); um pulso curto alivia sem afundar de novo.
        delay(600);
        uint16_t v = lerVccMv();
        if (v == 0 || v >= VCC_MOTOR_MIN_MV) {
            motorGiraMs(!sentidoA, MOTOR_RECUO_ABORT_MS);
        }
    } else if (MOTOR_RECUO_MS > 0 && inaOk) {
        // ⭐⭐ v2.17.2 — COAST antes de inverter (causa-raiz do reset no fim do
        // giro). 80ms NÃO param um motor com inércia: inverter o sentido com o
        // eixo ainda girando é FRENAGEM POR INVERSÃO — a tensão gerada pelo
        // motor se soma à aplicada e o pico de corrente chega ao DOBRO do
        // stall, derrubando o trilho por alguns ms -> brown-out -> reset. Era
        // o que comia a 2ª confirmação, a melodia e o próprio recuo (medido:
        // 3/3 ciclos sem a 2ª confirmação, com bateria a 60%/3,82V — carga em
        // que um giro normal NÃO derruba nada).
        // 350ms de roda-livre (motor em LOW/LOW = freio suave da ponte H) +
        // conferência do trilho antes de aplicar o sentido oposto.
        delay(350);
        uint16_t v = lerVccMv();
        if (v == 0 || v >= VCC_MOTOR_MIN_MV) {
            motorGiraMs(!sentidoA, MOTOR_RECUO_MS);
        }
    } else if (MOTOR_RECUO_MS > 0) {
        // ⭐⭐ v2.23 — RECUO PROPORCIONAL NO FALLBACK SEM INA219. Antes este
        // caminho caía no recuo de 900ms depois de um giro de apenas 1000ms
        // (MOTOR_MS): a fechadura DESFAZIA 90% da abertura, respondia 1004.xx e
        // o app mostrava sucesso com a porta fechada. Com INA219 o giro vai até
        // o batente (segundos) e 900ms é uma fração pequena — a constante foi
        // calibrada só para esse caso. Aqui o recuo é 1/5 do giro efetivo.
        delay(350);
        uint16_t v = lerVccMv();
        if (v == 0 || v >= VCC_MOTOR_MIN_MV) {
            motorGiraMs(!sentidoA, MOTOR_MS / 5);
        }
    }
}

// ---- módulo BLE (sempre a 2400) ----------------------------------------------

// Manda um comando AT e descarta a resposta (não dependemos dela — os clones
// nem sempre respondem). O delay dá tempo do módulo processar.
// Terminado em '\r' como o FI_1_0/FI_1_0_400 de produção: o lote de módulos
// ANTIGO (ver.03/04 das FI 1.0) exige o CR; o lote novo (ver.05) tolera —
// o FI_1_0_400 sempre mandou com '\r' nos mesmos módulos "Soft AT 5.2".
// ⭐ v2.23 — `forcar` permite enviar mesmo com PD3 alto. Necessário para o
// AT+DROP: derrubar a conexão é justamente a operação que só faz sentido
// CONECTADO, e a trava abaixo a tornava impossível (ver TST-HIB).
void at(const char* c, uint16_t w = 150, bool forcar = false) {
    // ⛔ TRAVA CRÍTICA: se o app está CONECTADO (PD3 alto), o módulo está em
    // MODE2 túnel e NÃO interpreta AT — ele REPASSA o "AT+..." como DADO pro app
    // (visto na bancada: "⟵ AT", "⟵ AT+NAME003FI002734" + lixo). AT é só p/
    // config, que só roda DESCONECTADO. Conectado, não manda nada.
    if (!forcar && digitalRead(PIN_WAKE) == HIGH) return;
    // ⭐⭐ v2.21 — RESILIENTE AO DESPERTAR (a razão de o corte pelo firmware
    // nunca pegar). Com AT+PWRM1 o módulo DORME quando ocioso e é acordado pelo
    // primeiro dado da UART (só funciona em AT+BAUD0/2400-slow, manual §22) —
    // mas ESSE primeiro byte se PERDE no despertar. O código já sabia disso em
    // outro ponto (bleVivo manda "AT" 4× "porque o 1º só acorda"), mas aqui
    // cada comando ia UMA vez: o AT+DROP era engolido e a sequência do corte
    // saía torta. Agora: um '\r' de sacrifício acorda, pausa curta, e o comando
    // vai DUAS vezes (repetir um AT de config/PIO é inócuo — mesmo resultado).
    bluetooth.print('\r');
    delay(12);                       // tempo do módulo despertar
    for (uint8_t t = 0; t < 2; t++) {
        bluetooth.print(c);
        bluetooth.print('\r');
        delay(t == 0 ? 25 : w);      // 1ª rápida, 2ª com a espera pedida
    }
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

// ⭐ IDENTIFICA o módulo pelo AT+VERS? — a string inteira diz a FAMÍLIA
// (manuais oficiais: 1010 responde "Soft AT ver.XX"/"Soft ATm ver.XX"; o 5.2
// responde "Soft AT 5.2 ver.XX") e o número diz a REV do firmware dele.
// Preenche g_moduloFam + g_moduloVers e PERSISTE na EEPROM (telemetria/boots
// futuros). Devolve a rev (0 = não respondeu = módulo mudo p/ consulta).
// Regras derivadas (ver cabeçalho): 5.2 rev<04 = BEFC/AFTC quebrados -> wake
// por STATUS; 1010 = sem STATUS, wake por AFTC. "ver.00..02" viram rev 1..2
// via atoi; resposta com "ver." mas número ilegível vira rev=1 (conta como
// vivo e cai no caminho conservador rev<4).
uint8_t bleIdentificar() {
    for (uint8_t t = 0; t < 3; t++) {
        while (bluetooth.available()) bluetooth.read();
        bluetooth.print("AT+VERS?\r");
        char resp[48] = {0};
        uint8_t n = 0;
        unsigned long tt = millis();
        while (millis() - tt < 450) {
            if (bluetooth.available() && n < sizeof(resp) - 1) resp[n++] = bluetooth.read();
        }
        char* v = strstr(resp, "ver.");
        if (!v) continue;
        g_moduloFam = strstr(resp, "5.2") ? FAM_52 : FAM_1010;
        uint8_t rev = (uint8_t)atoi(v + 4);
        if (rev == 0) rev = 1;                 // respondeu mas rev ilegível/00
        g_moduloVers = rev;
        EEPROM.update(EE_VERS_BLE, g_moduloVers);
        EEPROM.update(EE_MOD_FAM, g_moduloFam);
        return rev;
    }
    return 0;                                  // não leu (NÃO zera o que já sabíamos)
}

// Máscara dos comandos BEFC/AFTC (3 dígitos hex; bit = PIO−3, manual dos dois
// módulos). Calculada em runtime do pino do MOSFET (EEPROM 914) — a esteira
// legada usou PIO 4..9 conforme a placa; 90% da frota = 8 (BEFC020/AFTC028).
uint16_t mascaraPio(uint8_t pio) { return (pio >= 3 && pio <= 12) ? (uint16_t)(1u << (pio - 3)) : 0; }
void atMascara(const char* cmd, uint16_t mask) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%s%03X", cmd, mask);
    at(buf);
}

// AUTO-CURA DO NOME: lê AT+NAME? e confere se o anúncio é EXATAMENTE o serial.
// true = confere OU o módulo não respondeu à consulta (inconclusivo — clones
// mudos p/ consulta seguem o fluxo às cegas de sempre, sem reprovar).
// false = respondeu um nome DIFERENTE (ex.: garble "002FI00187<") -> reescrever.
bool bleNomeConfere() {
    if (digitalRead(PIN_WAKE) == HIGH) return true;   // conectado: AT tunelaria
    while (bluetooth.available()) bluetooth.read();
    bluetooth.print("AT+NAME?\r");
    char resp[40] = {0};
    uint8_t n = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 400) {
        if (bluetooth.available() && n < sizeof(resp) - 1) resp[n++] = bluetooth.read();
    }
    if (n == 0) return true;                          // mudo p/ consulta: inconclusivo
    char* g = strstr(resp, "Get:");
    if (!g) return strstr(resp, serialFech) != NULL;  // resposta fora do padrão
    g += 4;
    uint8_t i = 0;
    while (serialFech[i] && g[i] == serialFech[i]) i++;
    return serialFech[i] == 0 &&
           (g[i] == 0 || g[i] == '\r' || g[i] == '\n');  // match EXATO até o fim
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
    // ⭐⭐ v2.15.1: AT+PWRM1 SEMPRE (auto-sleep do módulo LIGADO) — reversão
    // consciente do PWRM0 da v2.9.12, por três motivos (02/08):
    // (1) BACKFEED: com o módulo sempre acordado (PWRM0), o TX da UART dele
    //     fica em 3,3V e ALIMENTA DE FORMA PARASITA o MCU de uma placa com
    //     mosfet CORTADA -> tentativas de boot fracas em loop (bipe a cada
    //     ~1s, caso real 2910). Com PWRM1 o módulo ocioso dorme, solta o TX
    //     -> corte limpo e silencioso.
    // (2) É a config da ESTEIRA DE PRODUÇÃO legada (at.js mandava PWRM1 p/
    //     toda a frota — anos de campo).
    // (3) Módulo dormindo = 0,65mA vs 1,5mA — metade do consumo de rádio.
    // O custo histórico do PWRM1 (1º byte perdido no diálogo MCU->módulo a
    // 9600, "ONG" sem o P etc.) perdeu relevância: a BANCADA é a dona da
    // config pelo ar (v2.14) e a conexão BLE acorda o módulo antes de
    // qualquer dado do app. No pino-12/auto o PWRM1 já era o próprio corte.
    at("AT+PWRM1");   // ⭐ v2.16: auto-sleep LIGADO (config da esteira legada).
                      // Só funciona junto do BAUD0/2400-slow (wake por dado) —
                      // ver o bloco do BAUD_MODULO. Mata o backfeed do corte e
                      // derruba o rádio de 1,5mA p/ 0,65mA.
    at("AT+TYPE0");    // sem pareamento (TYPE1 residual = pede PIN em toda conexão)
    // NOME reafirmado a CADA boot (como o changeName do FI_1_5), ANTES do MODE2.
    // ⭐ v2.10 AUTO-CURA: escreve, LÊ DE VOLTA (AT+NAME?) e compara — se a UART
    // marginal garblou o fim (caso real "002FI00187<"), REESCREVE (até 3×).
    // Módulo mudo p/ consulta (clone): bleNomeConfere devolve true na 1ª e o
    // fluxo fica idêntico ao antigo (1 write às cegas). Serial tem 11 chars —
    // cabe no limite dos DOIS módulos (1010 = 12; 5.2 rev05+ = 18).
    if (serialFech[0]) {
        char nm[24];
        snprintf(nm, sizeof(nm), "AT+NAME%s", serialFech);
        for (uint8_t t = 0; t < 3; t++) {
            at(nm, 200);
            if (bleNomeConfere()) break;
            DBGLN(F("[cfg] nome nao confere - reescrevendo"));
        }
    }
    // AT+MODE2 = modo controle remoto / túnel de dados (é o PADRÃO DE FÁBRICA,
    // manual pág.8). ORDEM DA v2.7.6 (provada em bancada com PONG): MODE2 logo
    // após o NAME. Aqui o módulo está DESCONECTADO (a função dá early-return se
    // conectado), então ele interpreta AT em qualquer posição — a ordem relativa
    // ao MODE2 não muda nada. Reafirmá-lo é redundante (já é fábrica) mas inócuo.
    at("AT+MODE2");
    at("AT+ROLE0");    // slave (executa advertising)
    at("AT+DELI3");    // delimitador da resposta AT = 0x0A 0x0D (manual pág.23)
    at("AT+NOTI1");    // módulo emite OK+CONN/OK+LOST na UART no connect/disconnect
                       // (manual pág.38). NÃO afeta o notify de dados do app (esse
                       // é o CCCD 0x2902 que o próprio app habilita no FFE1).
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
        // MODO NORMAL *E* HIBERNAÇÃO G1 — MESMAS máscaras (⭐ v2.13.3):
        //   BEFC mask(gate) = gate ALTO no power-on/pré-conexão -> ciclo de
        //                     bateria e desconexão RELIGAM a placa;
        //   AFTC mask(gate)|mask(6) = conexão religa o trilho E acorda o MCU.
        // É o ciclo comprovado da frota _400 de produção: a hibernação G1
        // difere só no dormir() — corte explícito AT+PIOx0, que persiste até o
        // próximo evento (conexão/power-on religa e o MCU corta de novo após a
        // janela ociosa).
        // ⚠️ A config antiga da hibernação (BEFC000/AFTC008, até v2.13.2) era um
        // CAMINHO DE TIJOLO: sem o bit do gate no AFTC, após o corte nem a
        // CONEXÃO nem o ciclo de bateria religavam — só o cabo USBasp.
        // ⭐ v2.13 MOSFET-AUTO (pino12/PIO2): o gate NÃO cabe nas máscaras (PIO2
        // é inendereçável) — quem corta/religa é o auto-sleep (PWRM1 acima).
        // ⭐⭐ v2.13.1 SEGURANÇA: mesmo no auto, as máscaras seguram o PIO8 ALTO
        // (BEFC020/AFTC028). Numa placa pino-12 de verdade isso é inócuo (o
        // pino 13 não liga em nada); numa GERAÇÃO 1 (gate em PIO real) rotulada
        // errada como 12, é o que impede o BEFC000 de CORTAR o trilho p/ sempre
        // (caso real: CH003FI002910, R0, morta pelo ar em 02/08 — MCU sem
        // energia, botão morto, estalos no boot; resgate = regravar com 8).
        uint16_t mMos = mosfetAuto() ? mascaraPio(8) : mascaraPio(g_pinMosfet);
        atMascara("AT+BEFC", mMos);                   // MOSFET=1 antes (se endereçável)
        atMascara("AT+AFTC", mMos | mascaraPio(6));   // +wake depois
    }
    at("AT+PIO60");    // repouso arma a próxima borda de wake
    // Wake por FAMÍLIA/REV (manuais oficiais): AT+STATUS só existe no 5.2 — no
    // 1010 é no-op e o wake é o AFTC (que existe e funciona nos dois). No 5.2
    // rev<04 o BEFC/AFTC é QUEBRADO (respondia só 0x000; corrigido na REV04) ->
    // o STATUS é o ÚNICO wake que funciona nesses módulos ("ver.03" da frota).
    // Família desconhecida + rev<4 = manda também (conservador, era a regra antiga).
    if (g_moduloFam != FAM_1010 && g_moduloVers != 0 && g_moduloVers < 4) {
        char st[14];
        // MOSFET-AUTO: STATUS no PIO6 (o wake do PD3) — não existe "STATUSC".
        snprintf(st, sizeof(st), "AT+STATUS%X",
                 (placa10 || mosfetAuto()) ? 6 : g_pinMosfet);
        at(st);
    }
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
//  PASSO 1 — ACORDA (PWRM0) e CONVERGE para 9600: em cada baud candidato,
//    AT+PWRM0 (v2.10.1: auto-sleep OFF primeiro — pega a janela acordada de
//    módulos com herança legada PWRM1) + AT+BAUD2 + AT+RESET.
//  PASSO 2 — CONFIG COMPLETA a 9600: SHIELD1/BAUD2/PWRM0 + anti-resíduo
//    (ROLE0/IMME0/ADTY0/START) + a config de dados/wake (configModuloLeve)
//    + NAME + RESET final. Flag 0xC9 SÓ se o módulo deu sinal de vida.
//
// Custo: ~20s, uma vez por gravação (na bancada). A melodia de "pronta" só
// toca no fim — o operador já espera por ela.
void bleProvisionar() {
    DBGLN(F("[prov] provisionamento completo do modulo (1o boot)"));
    beep(50, 1200); beep(50, 1200);            // "configurando o rádio, aguarde"

    // Só os bauds ALCANÇÁVEIS pelo SoftwareSerial a 16MHz (9600 primeiro = o mais
    // comum). Tirei 57600/115200/1200: SoftwareSerial não os faz confiável, e
    // varrê-los só alongava o boot (colidia com a conexão da bancada).
    static const long TODOS[] = {2400, 9600, 4800, 19200, 38400};
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
            for (uint8_t k = 0; k < 6; k++) at("AT", 40);   // acorda o módulo
            at("AT+PWRM0", 120);   // auto-sleep OFF (sempre acordado)
            at("AT+PIO41", 60); at("AT+PIO51", 60); at("AT+PIO71", 60);
            at("AT+PIO81", 60); at("AT+PIO91", 60);
            at("AT+BEFCFF7", 80); at("AT+AFTCFFF", 80);
        }
    }

    // PASSO 1 — CONVERGE p/ BAUD_MODULO (9600): em cada baud candidato manda
    // AT+BAUD2 (=9600) + AT+RESET. Como o módulo já vem de fábrica em 9600, na
    // prática isto REAFIRMA 9600; se algum módulo veio de outro baud, o comando
    // pega no baud atual dele e ele passa a 9600 (nos demais é lixo inócuo).
    for (uint8_t i = 0; i < N; i++) {
        bluetooth.begin(TODOS[i]);
        delay(30);
        for (uint8_t k = 0; k < 3; k++) at("AT", 40);   // acorda o módulo
        // ⭐ v2.10.1: PWRM0 (auto-sleep OFF) PRIMEIRO, no baud REAL do módulo.
        // Módulo com herança legada (esteira mandava PWRM1 = auto-sleep LIGADO)
        // fica com a UART DORMINDO — em baud != 2400-slow, dado serial NÃO o
        // acorda (manuais 5.2 §57 / 1010 §54: só GND no pino 24 ou conexão BLE).
        // A única brecha por UART é a JANELA ACORDADA logo após o power-on do
        // módulo (bateria recém-ligada) — e ela fechava antes do PWRM0 antigo,
        // que só vinha no PASSO 2 (~10s depois). Caso real: CH003FI002584,
        // surda p/ SEMPRE até o PWRM0 entrar pelo ar (bancada). Aqui é a
        // primeira coisa dita em CADA baud: se a janela existir, destrava já.
        at("AT+PWRM0", 120);
        at(AT_BAUD_CMD, 250);                  // -> 2400-slow
        at("AT+RESET", 150);
        delay(600);                            // módulo reinicia no baud novo
    }
    bluetooth.begin(BAUD_MODULO);
    delay(1500);                               // módulo termina de reiniciar
    while (bluetooth.available()) bluetooth.read();

    // PASSO 2 — config completa no baud alvo.
    at("AT+SHIELD1");
    at(AT_BAUD_CMD);                           // reafirma (só vale após reset)
    at("AT+PWRM1");                            // auto-sleep ON (2400-slow: wake por dado)
    at("AT+ROLE0");                            // slave (ROLE1 residual = não anuncia)
    at("AT+IMME0");                            // anuncia sozinho ao ligar
    at("AT+ADTY0");                            // anúncio conectável
    // ⭐ ECONOMIA DE RÁDIO: advertising de 100ms (fábrica) -> 211,25ms (ADVI2).
    // Corta o duty do rádio quase pela METADE com custo médio de ~+55ms na
    // descoberta (bem dentro dos intervalos recomendados p/ iOS, teto 1285ms).
    // No 1010 o ADVI exige RESET p/ valer — o RESET final deste provisionamento
    // cobre. Reverter: AT+ADVI0 pela bancada.
    at("AT+ADVI2");
    bleIdentificar();                          // família+rev (persiste na EEPROM)
    DBG(F("[prov] AT+VERS? -> fam ")); DBG(g_moduloFam);
    DBG(F(" rev ")); DBGLN(g_moduloVers);
    // O AT+NAME é enviado DENTRO do configModuloLeve, ANTES do AT+MODE2 (em
    // MODE2 o clone ignora o NAME) — e é reafirmado em todo boot. Reforço extra
    // aqui em VÁRIOS BAUDS às cegas: se a convergência de baud não fechou, o
    // nome pega no baud certo mesmo assim (nos outros bauds é lixo inócuo).
    if (serialFech[0]) {
        char nm[24];
        snprintf(nm, sizeof(nm), "AT+NAME%s", serialFech);
        const long bd[] = {2400, 9600, 38400, 19200, 4800};
        for (uint8_t i = 0; i < sizeof(bd) / sizeof(long); i++) {
            bluetooth.begin(bd[i]); delay(30);
            at("AT", 50); at("AT+PWRM0", 100); at(nm, 180);   // v2.10.1: acorda antes do nome
        }
        bluetooth.begin(BAUD_MODULO); delay(100);
    }
    configModuloLeve();
    at("AT+START", 150);                       // se IMME1 residual, inicia o anúncio
    // ⭐ v2.10.1: a flag SÓ é gravada se o módulo deu SINAL DE VIDA. Módulo mudo
    // (herança PWRM1 dormindo, UART com problema) fica SEM a flag -> todo boot
    // re-tenta o provisionamento completo (cada ciclo de bateria = nova janela
    // acordada do módulo). Se vivo, marca ANTES do reset (bateria afundando no
    // reset não re-roda o pesado à toa — motivo original da flag).
    if (bleVivo()) EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    else DBGLN(F("[prov] modulo MUDO - flag nao marcada (re-tenta no proximo boot)"));
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

// (A captura do MCUSR foi fundida na única função .init3 lá em cima — ver
// `initReset()`. MCUSR guarda o MOTIVO do último reset e precisa ser lido ANTES
// do init do core, porque o core o zera.)

// ⭐⭐ v2.19 — ESPERA PELA ENERGIA REAL.
// ⚠️ v2.23.1: a v2.23.0 anotou aqui que o "203 de 204 boots por BROWN-OUT"
// seria artefato do classificador (PORF não testado). MEDIDO DEPOIS, com o
// campo MCUSR cru: a 2910 devolve MCUSR:04 = BORF puro, SEM PORF — são
// brown-outs de VERDADE. Motivo: o gate corta o NEGATIVO, mas os ~17 capacitores
// de desacoplamento seguram o trilho, o VCC nunca chega a 0 e na volta só o BOD
// acusa. A medição original estava certa e esta função é necessária.
// Com a placa cortada, o TX do
// módulo vaza pelo diodo do pino RX e alimenta o MCU o suficiente para ele
// COMEÇAR a bootar; o consumo do boot derruba essa fonte fraquíssima, o BOD
// dispara e tudo recomeça ~3x por segundo — bipes, bateria drenando à toa e o
// comando seguinte pegando a placa num estado ruim (F07).
// Aqui, ANTES de qualquer coisa que gaste energia (beep, LED, rádio), o MCU
// confere o próprio VCC: alimentação real = ~5V (StepUp); parasita ~3V. Se
// estiver parasita, ele DORME em ciclos de 1s (watchdog em modo interrupção,
// consumo desprezível) e só continua o boot quando a energia de verdade
// chegar — o que acontece quando o app conecta e o módulo religa o gate.
// ⚠️ Diferente da v2.16.2 (removida): lá o MCU dormia PARA SEMPRE e uma
// leitura ruim do ADC deixava a fechadura muda até regravar. Aqui o sono é
// por tempo, sempre reversível, e a leitura passa por mediana (v2.18.1).
void esperaEnergiaReal() {
    // ⭐⭐ v2.22 — APAGA OS WS2812 DE VERDADE (rede de segurança do LED preso).
    // Até aqui esta função só baixava o PINO DE DADOS, o que impede o LED de
    // acender lixo novo mas NÃO limpa o que já está latchado: um verde travado
    // por perda de energia no meio de uma piscada SOBREVIVIA ao reset e ficava
    // aceso pelas horas seguintes (~14mA), porque o MCU dorme aqui e só
    // inicializava o FastLED lá embaixo no setup, que nunca era alcançado.
    // Agora todo boot — inclusive o parasita — começa mandando PRETO.
    // Esta é a segunda linha de defesa: a primeira é a ordem do fbComandoOk().
    ledsInit();
    ledCor(CRGB::Black);
    // pino de dados dos LEDs em nível BAIXO: sem dado válido os WS2812 mantêm
    // o último valor (agora garantidamente preto) em vez de acender lixo.
    // ⚠️ Só no FI 1.5: no FI 1.0 o PIN_LEDS (PB3) é o MOTOR B.
    if (!placa10) {
        pinMode(PIN_LEDS, OUTPUT);
        digitalWrite(PIN_LEDS, LOW);
    }
    // ⛔⛔ v2.23.1 — NÃO ARMAR O BOTÃO AQUI. NÃO REDUZIR O TETO. (revertido)
    //
    // A v2.23.0 armou o INT0 e saía do laço quando o botão fosse apertado,
    // "para o botão deixar de ficar surdo". Era uma leitura ERRADA do código, e
    // causou um sintoma real em campo (2910, 10/08): com a placa cortada e o
    // botão MANTIDO PRESSIONADO, ouvia-se um "tec tec tec" rápido e baixo.
    //
    // O raciocínio correto: este laço SÓ executa quando o VCC está ABAIXO de
    // VCC_MIN_BOOT_MV — se a energia fosse real, a 1ª leitura já teria saído da
    // função. Ou seja, aqui dentro a alimentação é sempre PARASITA (o TX do
    // módulo vazando pelo diodo do pino RX). Sair do laço nessa condição é
    // exatamente o que a v2.19 existe para IMPEDIR: o boot segue, chega no
    // beep(70,2600) do "ESTOU VIVO", o consumo derruba a fonte fraquíssima, o
    // BOD dispara e tudo recomeça — o bipe sai truncado ("tec") e o ciclo se
    // repete várias vezes por segundo. Com o botão preso, isso vira um loop.
    //
    // E o botão NÃO tem o que fazer aqui de qualquer forma: sem energia real
    // não há como girar o motor. O botão estar "surdo" durante a espera é o
    // comportamento CORRETO — o que ele não pode é ficar surdo com a placa
    // ALIMENTADA, e isso já é tratado no polling do atenderApp().
    //
    // O teto também volta a 300s: encurtá-lo para 30s só faz o ciclo parasita
    // recomeçar 10x mais vezes, gastando bateria à toa.
    for (uint16_t i = 0; i < 300; i++) {          // teto ~5 min, nunca infinito
        uint16_t v = lerVccMv();
        if (!v || v >= VCC_MIN_BOOT_MV) return;   // energia real (ou inconclusivo)
        // sono de 1s com ADC e BOD desligados (a lib cuida do watchdog; definir
        // um ISR(WDT_vect) próprio colide com o vetor dela)
        LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_OFF);
    }
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

// ANTI-DUPLICATA (⭐ v2.12.1: POR COMANDO): reenvio do MESMO comando logo após
// outro é REEXECUÇÃO espúria (o app reenvia quando a confirmação se perde; ou
// o loop() reentra no atenderApp) — dentro da janela o firmware NÃO gira de
// novo, só reconfirma o status (o app precisa dele p/ parar de tentar).
// Comando DIFERENTE (FECHAR logo após ABRIR) SEMPRE executa: a versão antiga
// não distinguia e ENGOLIA o fechar-após-abrir — provado em bancada 09/07
// (FECHAR na janela = reconfirma sem girar; o app mostrava "porta aberta" com
// o motor parado). Janela 6s→4s: a tempestade de retry automático chega em
// 1-3s (mesma sessão); o retry HUMANO (reação + cooldown 4,5s do app +
// reconexão) chega em ~6,5s — 4s separa os dois com folga pros dois lados
// (caso real: fechadura emperrou -> fechar de novo tem que girar).
// A janela conta do FIM do giro e um comando engolido NÃO a renova.
#define ANTIDUP_MS 4000UL
unsigned long g_ultimoAcionamentoMs = 0;
unsigned long g_ultimoCmd = 0;          // CMD_ABRIR/CMD_FECHAR do último giro

bool acionamentoDuplicado(unsigned long cmd) {
    unsigned long agora = millis();
    if (g_ultimoAcionamentoMs && cmd == g_ultimoCmd &&
        agora - g_ultimoAcionamentoMs < ANTIDUP_MS) return true;
    return false;
}

// HANDSHAKE COM SALTOS (app legado/cascata): manda o status ANTES de girar —
// o app legado usa o status como confirmação rápida (timeout curto) e desconecta;
// ele não espera o fim do giro. Mantido p/ não quebrar a base instalada.
void acionar(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    enviaStatus(sentidoA);
    if (acionamentoDuplicado(cmd)) return;   // MESMO cmd: já confirmou, não gira 2x
    g_ultimoAcionamentoMs = millis();
    g_ultimoCmd = cmd;
    motorGira(sentidoA);
    g_ultimoAcionamentoMs = millis();     // o giro consumiu tempo; re-marca
}

// PROTOCOLO DIRETO (verbos ABRIR/FECHAR do app novo): gira PRIMEIRO (até o
// batente + recuo) e manda o status SÓ QUANDO O MOTOR PARA. Assim o app
// confirma "concluído" no momento exato em que a fechadura terminou de girar —
// é o "avisar quando parar". O app espera esta notificação com timeout longo.
void acionarVerbo(unsigned long cmd) {
    bool sentidoA = (cmd == CMD_ABRIR) ? (calibrationOk == 1) : (calibrationOk == 0);
    if (acionamentoDuplicado(cmd)) {      // reenvio do MESMO cmd: NÃO gira,
        enviaStatus(sentidoA);            // só reconfirma p/ o app parar de tentar
        return;
    }
    g_ultimoAcionamentoMs = millis();
    g_ultimoCmd = cmd;
    // ⭐ CONFIRMA ANTES DO MOTOR (fix do "abriu 3x + loading infinito"): o motor
    // puxa corrente e pode DERRUBAR o BLE; se a confirmação saísse só DEPOIS do
    // giro, ela se perdia -> o app dava timeout e reenviava (abrindo de novo a
    // cada rodada). Mandando o status JÁ (conexão ainda saudável), o app recebe
    // na hora e para de tentar. Mesmo padrão do "11" da calibração.
    enviaStatus(sentidoA);
    delay(60);
    motorGira(sentidoA);      // gira até o fim de curso + recuo (motor PARA aqui)
    g_ultimoAcionamentoMs = millis();     // re-marca no FIM (o giro levou tempo)
    delay(120);               // garante o pacote do status transmitido...
    enviaStatus(sentidoA);    // reconfirma no FIM se a conexão sobreviveu ("parou")
    fbComandoOk();            // fanfarra do Rocky + verde = "abriu/fechou!"
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
    // Família do módulo identificada pelo AT+VERS? (5.2 = EFR32BG22; 1010 =
    // CSR-1010; ? = nunca respondeu à consulta — clone ou UART marginal).
    snprintf(buf, sizeof(buf), "MODF:%s",
             g_moduloFam == FAM_52 ? "5.2" : g_moduloFam == FAM_1010 ? "1010" : "?");
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "INA:%s", inaOk ? "OK" : "SEM");  // batente por corrente
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "PLACA:%s", placa10 ? "1.0" : "1.5");
    enviaLinha(buf);
    // 4..9 = gate em PIO endereçável; 12-AUTO = pino físico 12/PIO2 (PWRM1)
    if (mosfetAuto()) snprintf_P(buf, sizeof(buf), PSTR("MOSFET:12-AUTO"));
    else              snprintf_P(buf, sizeof(buf), PSTR("MOSFET:%u"), g_pinMosfet);
    enviaLinha(buf);
    snprintf(buf, sizeof(buf), "WAKE:v%02u", g_moduloVers);   // rev do módulo (00 = não leu)
    enviaLinha(buf);
    // Segundos desde o boot — a PROVA DE CORTE do teste de hibernação da
    // bancada v2.13 (mosfet-auto): uptime pequeno após reconectar = o MCU
    // REBOOTOU = a placa foi cortada e religada; uptime grande = nunca cortou.
    snprintf_P(buf, sizeof(buf), PSTR("UPTIME:%lu"), millis() / 1000UL);
    enviaLinha(buf);
    // ⭐ v2.18 — telemetria de soak (tudo que o teste automatizado precisa):
    //  RST    motivo do último reset: P=power-on(religou pelo mosfet)
    //         B=BROWN-OUT(caiu a tensão!) E=externo W=watchdog
    //  BOOTS  quantos boots desde o TST-ZERA · BODS quantos foram brown-out
    //  CUTS   quantas vezes o firmware EXECUTOU o corte de energia
    //         ⚠️ Em placa mosfetAuto() (EEPROM 914=12) o corte é feito pelo
    //         PRÓPRIO MÓDULO ao dormir e o firmware NUNCA o executa: ali
    //         CUTS:AUTO, e CUTS:0 não significaria falha.
    //  MCUSR  o registrador CRU, em hex — a fonte da verdade sobre o reset
    //  VCC    tensão do trilho agora · VCCMIN a MENOR vista no último giro
    { uint16_t bo, bd, ct;
      EEPROM.get(EE_BOOTS, bo); EEPROM.get(EE_BODS, bd); EEPROM.get(EE_CUTS, ct);
      if (bo == 0xFFFF) bo = 0; if (bd == 0xFFFF) bd = 0; if (ct == 0xFFFF) ct = 0;
      // ⭐⭐ v2.23 — PORF PRIMEIRO. Os flags do MCUSR são CUMULATIVOS e um
      // power-on legítimo tipicamente seta PORF **e** BORF (o detector de BOD
      // dispara na rampa de subida do trilho, com efuse 0xFD = BOD 2,7V).
      // Testando BORF primeiro, TODO religamento pelo mosfet — que é um
      // power-on de verdade — era rotulado 'B'. Assinatura do bug nos soaks da
      // 2910: rst='B' em 100% das linhas e bods == boots numa razão 1:1
      // perfeita. Um sistema com brown-outs reais mostraria mistura.
      snprintf_P(buf, sizeof(buf), PSTR("RST:%c"),
                 (g_mcusr & _BV(PORF)) ? 'P' : (g_mcusr & _BV(BORF)) ? 'B'
                 : (g_mcusr & _BV(WDRF)) ? 'W' : (g_mcusr & _BV(EXTRF)) ? 'E' : '?');
      enviaLinha(buf);
      // MCUSR cru: nenhuma interpretação, para o diagnóstico nunca mais
      // depender de a classificação estar certa.
      snprintf_P(buf, sizeof(buf), PSTR("MCUSR:%02X"), g_mcusr); enviaLinha(buf);
      snprintf_P(buf, sizeof(buf), PSTR("BOOTS:%u"), bo);   enviaLinha(buf);
      snprintf_P(buf, sizeof(buf), PSTR("BODS:%u"), bd);    enviaLinha(buf);
      // Em placa mosfetAuto() o firmware nunca executa corte — reportar 0 ali
      // induzia o operador a diagnosticar "o corte falhou" quando na verdade
      // quem corta é o módulo ao dormir.
      // literais via PSTR: string em RAM é escassa aqui (~470 B livres)
      if (mosfetAuto()) { snprintf_P(buf, sizeof(buf), PSTR("CUTS:AUTO")); }
      else              { snprintf_P(buf, sizeof(buf), PSTR("CUTS:%u"), ct); }
      enviaLinha(buf); }
    snprintf_P(buf, sizeof(buf), PSTR("VCC:%u"), lerVccMv());       enviaLinha(buf);
    snprintf_P(buf, sizeof(buf), PSTR("VCCMIN:%u"), g_vccMinGiro);  enviaLinha(buf);
    // ATOK: o módulo respondeu ao MCU na última tentativa de corte (medido
    // DESCONECTADO, que é a condição real). 2 = ainda não houve tentativa.
    snprintf_P(buf, sizeof(buf), PSTR("ATOK:%u"), g_atOk);          enviaLinha(buf);
    enviaLinha("VER:" FW_VERSION);
    enviaLinha("FIM-INFO");
}

void testeBancada(const String& t) {
    if (t.startsWith("TST-PING")) {
        // PONG em DOBRO com gap: este lote de módulo às vezes engole o 1º byte
        // (mesma razão do "11 duplo" da calibragem). Se o 1º PONG for comido, o
        // 2º chega limpo. O app casa no primeiro que vier íntegro.
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
    // DIAGNÓSTICO DE UART SEM MULTÍMETRO: conectado, cada byte MCU<->app passa
    // pela MESMA UART MCU<->módulo — então dá pra medir a saúde dela por
    // software. TST-UART manda 5 linhas-padrão conhecidas; a bancada confere a
    // integridade (garble = nível/solda marginal — a causa física do nome
    // corrompido). TST-ECO<x> devolve "ECO:<x>" = prova o caminho RX+TX.
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
    // Prova do mecanismo de hibernação. LIÇÃO da bancada (12:37): com um
    // cliente CONECTADO o módulo está em modo túnel e NÃO interpreta AT do
    // MCU (o "AT+PIO80" apareceu como texto no cliente). Por isso a ordem de
    // produção é DROP primeiro (derruba a conexão -> módulo volta ao modo
    // comando) e SÓ ENTÃO o corte. Resultado audível para o operador:
    //   silêncio após a queda      = CORTOU (reconecte: bipe de boot + PONG)
    //   3 bipes graves após ~4s    = módulo não obedeceu o PIO80
    //   "HIB-FALHOU-DROP" na tela  = nem o DROP derrubou (segue conectado)
    // Liga/desliga a HIBERNAÇÃO permanente (toggle EE_HIBERNA). Só depois de
    // validar o ciclo com TST-HIB! (checados ANTES de "TST-HIB" — prefixo maior)
    if (t.startsWith("TST-HIB-ON")) {
        EEPROM.update(EE_HIBERNA, 1); g_hiberna = true;
        enviaLinha("OK-HIB-ON");             // vale no próximo boot (config BEFC000)
        return;
    }
    if (t.startsWith("TST-HIB-OFF")) {
        EEPROM.update(EE_HIBERNA, 0); g_hiberna = false;
        enviaLinha("OK-HIB-OFF");
        return;
    }
    // VALIDAÇÃO do ciclo corta->religa (não muda o toggle; testa o hardware).
    //   silêncio + boot(Rocky) na reconexão = CORTOU e RELIGOU -> hibernação OK
    //   3 bipes graves após ~3s              = MCU vivo = módulo NÃO cortou
    //   "HIB-FALHOU-DROP" na tela            = nem o DROP derrubou (segue conectado)
    if (t.startsWith("TST-HIB")) {
        // ⭐ v2.13 MOSFET-AUTO (pino12/PIO2): não existe comando de corte — o
        // módulo corta sozinho ao dormir (PWRM1). A bancada valida por UPTIME
        // (TST-INFO): derruba, espera o auto-sleep e confere se o MCU REBOOTOU.
        if (mosfetAuto()) { enviaLinha("HIB-AUTO"); return; }
        enviaLinha("OK-HIB");
        delay(400);                          // a resposta sai antes do DROP
        // ⭐⭐ v2.23 — `forcar=true`. Este teste chega POR BLE, logo PD3 está
        // ALTO por definição — e a trava do at() recusava enviar com PD3 alto.
        // O AT+DROP nunca saía, a conexão nunca caía e a resposta era SEMPRE
        // "HIB-FALHOU-DROP". O teste era matematicamente impossível de passar,
        // e foi ele que sustentou a tese de que o hardware não obedecia ao
        // corte. O AT vazar como texto para o cliente aqui é aceitável: a
        // intenção é justamente derrubá-lo.
        at("AT+DROP", 500, /*forcar=*/true);  // derruba a conexão -> sai do túnel
        delay(500);
        if (digitalRead(PIN_WAKE) == HIGH) { // ainda conectado -> não dá p/ cortar
            enviaLinha("HIB-FALHOU-DROP");
            return;
        }
        atMascara("AT+BEFC", 0);             // ⭐ libera o gate (senão o BEFC re-liga)
        at("AT+PIO60", 200);                 // arma a borda de wake (PIO6 baixo)
        EEPROM.update(EE_HIB, 1);            // marca "desligou hibernando" p/ o wake
        char pio[12];                        // corta pelo PIO do MOSFET (EEPROM 914)
        snprintf(pio, sizeof(pio), "AT+PIO%X0", g_pinMosfet);
        at(pio, 60);                         // CORTA o MOSFET -> MCU morre se cortou
        delay(3000);                         // se cortou, nunca passa daqui
        EEPROM.update(EE_HIB, 0);
        beep(160, 400); beep(160, 400); beep(160, 400);   // 3 graves = NÃO cortou
        return;
    }
    // ⭐ v2.18: zera a telemetria (início de uma bateria de testes)
    if (t.startsWith("TST-ZERA")) {
        uint16_t z = 0;
        EEPROM.put(EE_BOOTS, z); EEPROM.put(EE_BODS, z); EEPROM.put(EE_CUTS, z);
        enviaLinha("OK-ZERA");
        return;
    }
    // ⭐ v2.23 — MODO SOAK: liga/desliga a persistência da telemetria em EEPROM.
    // Em campo fica DESLIGADO (nenhuma escrita por ciclo de sono ou boot); a
    // bancada liga antes de uma bateria de testes e desliga ao final.
    // `TST-SOAK` alterna, `TST-SOAK1`/`TST-SOAK0` forçam o estado.
    if (t.startsWith("TST-SOAK")) {
        uint8_t atual = (EEPROM.read(EE_SOAK) == 1) ? 1 : 0;
        uint8_t novo = t.endsWith("1") ? 1 : (t.endsWith("0") ? 0 : (atual ? 0 : 1));
        EEPROM.update(EE_SOAK, novo);
        char b[14];
        snprintf_P(b, sizeof(b), novo ? PSTR("OK-SOAK-ON") : PSTR("OK-SOAK-OFF"));
        enviaLinha(b);
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
    // ⚠️ SEM bipe de wake aqui. Com o sono leve (IDLE) o MCU acorda a CADA byte —
    // bipar na acordada dava o "beep agudo de tempos em tempos" (mesmo com o app
    // fechado, o módulo acorda o MCU com OK+LOST/ruído). O feedback sonoro agora é
    // por EVENTO: a MELODIA toca quando chega "OK+CONN" (conectou de verdade); nada
    // em wakes espúrios.
    DBGLN(F("[app] acordou - ouvindo (20s)"));
    bluetooth.setTimeout(150);
    unsigned long t0 = millis();
    // ⭐⭐ v2.23 — tAbs agora é STATIC. Antes era reinicializado a cada chamada
    // de atenderApp(), e como o loop() faz dormir()->atenderApp() em ciclo (com
    // PD3 alto o dormir() retorna em ~1ms pelo ramo IDLE), o teto JANELA_MAX
    // NÃO LIMITAVA NADA: um PIO6 latchado alto deixava o MCU acordado para
    // sempre, em IDLE (mA em vez de µA), sem nunca conseguir cortar o trilho
    // nem reconfigurar o módulo (at() e configModuloLeve() fazem early-return
    // com PD3 alto). Era um estado sem saída que só a queda da bateria encerrava.
    static unsigned long tAbs = 0;
    static bool tAbsArmado = false;
    if (!tAbsArmado || digitalRead(PIN_WAKE) == LOW) { tAbs = millis(); tAbsArmado = true; }
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
            // ⭐ v2.23 — LEITURA COM TETO. `readString()` acumula enquanto
            // chegar byte dentro do timeout, sem limite de tamanho. Com 538 B
            // de RAM livre, uma sessão de lixo contínuo (UART marginal — falha
            // documentada deste lote) fazia a String crescer até falhar o
            // realloc, fragmentando o heap contra a pilha.
            char linha[48];
            size_t n = bluetooth.readBytesUntil('\n', linha, sizeof(linha) - 1);
            linha[n] = 0;
            while (bluetooth.available() > 0) bluetooth.read();   // descarta excesso
            String txt(linha); txt.trim(); txt.toUpperCase();
            DBG(F("[app] txt: ")); DBGLN(txt);
            // Notificações do módulo (AT+NOTI1): conectou / desconectou. É AQUI que
            // toca a MELODIA de conexão — evento real, não wake espúrio.
            if (txt.startsWith("OK+CONN")) {
                if (!g_sessaoConectada) { g_sessaoConectada = true; sinalConectado(); }
                continue;
            }
            if (txt.startsWith("OK+LOST")) { g_sessaoConectada = false; continue; }
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
    // ⭐⭐ v2.23 — SAÍDA FORÇADA DO PD3 LATCHADO. Só tornar o tAbs estático
    // transformava o estado-sem-saída num laço apertado dormir(IDLE)/retorno
    // imediato — continuava queimando mA e sem cortar o trilho. Se a janela
    // ABSOLUTA estourou e o PD3 continua ALTO, o cliente sumiu sem gerar
    // OK+LOST ou o PIO6 do módulo latchou (falha conhecida do lote). Derruba à
    // força (aqui o AT PRECISA sair com PD3 alto — é exatamente o caso do
    // `forcar`) e, se ainda assim não ceder, reinicia: um reset é infinitamente
    // melhor que uma fechadura acordada para sempre drenando a bateria.
    if (millis() - tAbs >= JANELA_MAX && digitalRead(PIN_WAKE) == HIGH) {
        at("AT+DROP", 300, /*forcar=*/true);
        delay(400);
        if (digitalRead(PIN_WAKE) == HIGH) resetMCU();
        tAbsArmado = false;               // rearma a janela para a próxima sessão
    }
}

// ---- botão físico: toggle curto / reset total em 10s -------------------------
void atenderBotao() {
    // ⭐⭐ v2.23 — EXIGE QUE O BOTÃO TENHA SIDO SOLTO. Sem isto, um botão em
    // curto (infiltração, tecla presa, solda) produzia um loop infinito:
    // boot -> atenderApp vê PD2 baixo -> 10s -> apaga EE_MOD_CFG + reset ->
    // boot... a cada ~12s, para sempre. São ~4 escritas de EEPROM por volta =
    // 100k na célula 910 em ~14 dias, e cada boot entrava no provisionamento
    // pesado. Agora o botão só é honrado depois de ter sido visto SOLTO, e é
    // ignorado nos 2s iniciais pós-boot.
    static bool precisaSoltar = true;   // no 1º boot exige uma soltura
    if (millis() < 2000) return;        // janela morta pós-boot
    if (precisaSoltar) {
        if (digitalRead(PIN_BUTTON) != LOW) precisaSoltar = false;  // soltou: libera
        return;                          // ainda preso: não faz nada
    }
    delay(30);                                       // debounce
    if (digitalRead(PIN_BUTTON) != LOW) return;      // ruído
    precisaSoltar = true;                            // consome este toque
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

// dormir = SONO EM DOIS NÍVEIS (⭐ v2.12, base da economia de bateria):
//
//  · DESCONECTADO (PD3 LOW — 99,9% do tempo) -> SLEEP_MODE_PWR_DOWN (MCU ~µA).
//    Em power-down, interrupt por BORDA (INT0/INT1) NÃO dispara (a detecção de
//    borda precisa do clkIO, que para) — por isso o wake da CONEXÃO usa o
//    PCINT19 (PD3), habilitado no MESMO grupo PCINT2 que o SoftwareSerial já
//    usa pro RX (PD4): o ISR dele atende o evento à toa, mas o wake acontece.
//    Botão = INT0 por NÍVEL LOW (nível funciona em power-down). O cristal de
//    16MHz demora a religar no acordar — SEM problema aqui: quem acorda é a
//    CONEXÃO (PD3), e o app ainda leva centenas de ms até o 1º write. Módulo
//    clone sem borda no PIO6 ("ver.12"): os DADOS no RX (PCINT20) acordam; o
//    1º write pode se perder no arranque do cristal e o app/bancada retransmite
//    (comportamento antigo documentado — só vale pros clones).
//
//  · CONECTADO (PD3 HIGH) -> SLEEP_MODE_IDLE (lição da v2.9.1): o oscilador
//    continua rodando, wake por dado é INSTANTÂNEO e nenhum byte se perde ->
//    o 2º/3º/N-ésimo comando NA MESMA conexão funcionam. Power-down aqui era
//    o bug "2º comando não acontece; fechar o app e reabrir funciona" (o
//    OK+CONN da reconexão acordava o MCU antes do comando; na mesma conexão o
//    próprio comando era o despertador e morria no arranque do cristal).
//
// ADC desligado durante o power-down (restaurado ao acordar) + BOD desligado
// durante o sono (sleep_bod_disable; religa sozinho no wake).
// ⚠️ NÃO manda AT aqui: conectado (MODE2 túnel) vazaria como DADO pro app.
// Motor fica OUTPUT LOW (nunca Hi-Z — evita shoot-through na ponte H).
void dormir() {
    motorPara();
    // ⭐⭐ v2.20 — APAGA OS LEDs ANTES DE PERDER A ENERGIA. Os WS2812 GUARDAM o
    // último valor recebido; se a placa é cortada com eles em estado indefinido
    // (ou se acordam com lixo na alimentação parasita), ficam ACESOS puxando
    // corrente justamente no repouso — foi visto em campo: placa cortada, botão
    // morto e 2 LEDs acesos. Mandando "preto" agora, o valor travado é apagado,
    // e o pino de dados fica em nível baixo (nenhum dado novo é interpretado).
    if (!placa10) {
        // ⭐ v2.23 — FastLED.show() desliga as interrupções por ~90us e
        // corromperia um RX BLE em andamento (invariante declarada no topo do
        // arquivo e violada aqui: dormir() roda TAMBÉM com cliente conectado,
        // pelo ramo IDLE). A 2400 baud a margem é de 4,5x e por isso nunca
        // estourou — a 9600 seriam 87% de um bit e a corrupção viraria certeza.
        // Só mexe nos LEDs quando NÃO há ninguém conectado.
        if (digitalRead(PIN_WAKE) == LOW) {
            fill_solid(leds, NUM_LEDS, CRGB::Black);
            FastLED.show();
        }
        pinMode(PIN_LEDS, OUTPUT);
        digitalWrite(PIN_LEDS, LOW);
    } else {
        digitalWrite(PIN_LED10_1, LOW);
        digitalWrite(PIN_LED10_2, LOW);
        digitalWrite(PIN_LED10_3, LOW);
    }
    // HIBERNAÇÃO (toggle EE_HIBERNA): corta o trilho pelo MOSFET (receita do
    // FI_1_5_400). Chega aqui só quando OCIOSO+DESCONECTADO (atenderApp segura a
    // janela enquanto PD3 alto), então o at() não vaza pro app. O MCU DESLIGA no
    // AT+PIO80 e só volta por CONEXÃO (boot fresco). Requer BEFC000 (config de
    // hibernação) — com BEFC020 o módulo re-liga o PIO8 e o corte não pega.
    // Se o hardware NÃO cortar (placa sem o gate), o código segue pro IDLE abaixo.
    // ⭐ v2.13 MOSFET-AUTO (pino12/PIO2): NÃO há comando de corte — o módulo
    // corta a placa SOZINHO quando o auto-sleep (PWRM1) o derrubar. O MCU só
    // segue pro powerDown abaixo e morre quando o corte vier (seguro: dormindo
    // não há escrita de EEPROM em andamento).
    if (g_hiberna && !placa10 && !mosfetAuto() && digitalRead(PIN_WAKE) == LOW) {
        // ⭐⭐ v2.16.1 — A ORDEM É O SEGREDO (receita do goToSleep legado):
        // este módulo RE-APLICA a máscara BEFC no evento de DESCONEXÃO. Por
        // isso o corte tem de ser a ÚLTIMA palavra: primeiro AT+DROP (deixa o
        // módulo fazer o re-apply dele), depois uma folga para esse evento
        // assentar, e SÓ ENTÃO o AT+PIOx0 — que fica valendo até a próxima
        // conexão (aí o AFTC religa). É por isso que o corte do APP (mandado
        // CONECTADO) não colava: a desconexão que vinha depois o desfazia.
        // telemetria: tentei cortar — ⭐ v2.23 só grava em MODO SOAK (bancada).
        // Em campo isto era uma escrita de EEPROM POR CICLO DE SONO.
        if (EEPROM.read(EE_SOAK) == 1) {
            uint16_t c; EEPROM.get(EE_CUTS, c); if (c == 0xFFFF) c = 0;
            c++; EEPROM.put(EE_CUTS, c);
        }
        g_atOk = bleVivo() ? 1 : 0;   // ⭐ v2.23: ATOK era telemetria MORTA
                                      // (nunca atribuída, sempre "2").
        at("AT+DROP", 200);
        delay(120);               // curto: pausa longa deixava o módulo readormecer
                                  // (o re-apply do BEFC da desconexão acontece aqui)
        at("AT+PIO60", 100);      // arma a borda de wake (PIO6 baixo)
        char pio[12];             // corta pelo PIO do MOSFET (EEPROM 914, default 8)
        snprintf(pio, sizeof(pio), "AT+PIO%X0", g_pinMosfet);
        at(pio, 60);              // corta o MOSFET -> MCU morre aqui se cortou
        delay(200);
        at(pio, 60);              // 2ª ordem: cobre o caso do 1º comando ter
        delay(200);               // acordado o módulo em vez de ser executado
    }
    acordouBLE = false; acordouBtn = false;
    g_sessaoConectada = false;   // sessão encerrou -> melodia toca de novo no próximo OK+CONN

    if (digitalRead(PIN_WAKE) == LOW) {
        // ---- DESCONECTADO: PWR_DOWN profundo ----
        attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrBtn, LOW);
        PCICR  |= _BV(PCIE2);          // grupo PCINT2 ligado (o SoftwareSerial já liga,
        PCMSK2 |= _BV(PCINT19);        // garantia barata) + PD3 acorda na conexão
        uint8_t adcsraSalvo = ADCSRA;
        ADCSRA &= ~_BV(ADEN);          // ADC off no sono profundo
        set_sleep_mode(SLEEP_MODE_PWR_DOWN);
        cli();
        // Re-checa JÁ SEM interrupts: se a conexão/botão/dado chegou entre o
        // digitalRead lá de cima e este cli(), NÃO dorme (senão o evento que
        // já foi atendido pelo ISR se perderia e o MCU dormiria conectado).
        // sei()+sleep_cpu() consecutivos são atômicos (a instrução seguinte ao
        // SEI executa antes de qualquer interrupt pendente — garantia do AVR).
        if (digitalRead(PIN_WAKE) == LOW && !acordouBtn && !bluetooth.available()) {
            sleep_enable();
            sleep_bod_disable();       // BOD off SÓ durante o sono
            sei();
            sleep_cpu();
            sleep_disable();
        }
        sei();
        ADCSRA = adcsraSalvo;
        PCMSK2 &= ~_BV(PCINT19);
        detachInterrupt(digitalPinToInterrupt(PIN_BUTTON));
        if (digitalRead(PIN_WAKE) == HIGH) acordouBLE = true;
    } else {
        // ---- CONECTADO: IDLE (wake instantâneo, byte nenhum se perde) ----
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
}

// ⭐⭐ v2.16.2 — DETECTOR DE BOOT PARASITA (fim do bipe em loop com a placa
// cortada). Com a placa hibernando (gate cortado), o TX da UART do módulo
// (3,3V) vaza pelo diodo de proteção do pino RX e ALIMENTA fracamente o MCU.
// Ele tenta bootar, BIPA e — pior — manda os AT da config no boot, que ACORDAM
// o módulo e mantêm o TX alto: o ciclo se auto-alimenta e nunca para.
// Como distinguir: alimentado de VERDADE o MCU roda com VCC = 5V (StepUp MT3608);
// na alimentação parasita ele mal passa do brown-out (~3V). Medimos o VCC de
// dentro do chip (referência interna de 1,1V lida contra o VCC) e, se estiver
// baixo, o boot é FALSO: não bipa, não fala com o módulo, e dorme fundo — o
// módulo então adormece (PWRM1), solta o TX e o corte vira silêncio.
// ⚠️ v2.18.1: a PRIMEIRA conversão após trocar referência/canal é lixo — usá-la
// direto fazia a função devolver valores absurdos (e, no detector de boot da
// v2.16.2, ISSO MATAVA A PLACA: o MCU se mandava dormir para sempre a cada boot
// e a fechadura ficava muda até ser regravada — caso real 02/08 na 2910).
// Agora: descarta 2 conversões e devolve a MEDIANA de 3.
uint16_t lerVccMv() {
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);   // AVcc ref, bandgap 1V1
    delay(5);                                     // o bandgap precisa assentar
    for (uint8_t d = 0; d < 2; d++) {             // descarta as 2 primeiras
        ADCSRA |= _BV(ADSC); while (ADCSRA & _BV(ADSC)); (void)ADC;
    }
    uint16_t a[3];
    for (uint8_t i = 0; i < 3; i++) {
        ADCSRA |= _BV(ADSC); while (ADCSRA & _BV(ADSC));
        a[i] = ADC;
        delayMicroseconds(200);
    }
    // mediana (imune a uma leitura fora da curva)
    uint16_t med = (a[0] > a[1]) ? ((a[1] > a[2]) ? a[1] : (a[0] > a[2]) ? a[2] : a[0])
                                 : ((a[0] > a[2]) ? a[0] : (a[1] > a[2]) ? a[2] : a[1]);
    if (med < 50 || med > 1023) return 0;         // fora de faixa = inconclusivo
    return (uint16_t)(1125300UL / med);           // 1,1V * 1023 * 1000 / adc
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

    // ⭐ v2.12 — ECONOMIA ESTÁTICA (vale no ativo, no IDLE e no power-down).
    // NÃO desligar: TWI (INA219), TIM0 (millis), TIM1/TIM2 (tone/core),
    // USART0 (debug), ADC (bateria — desligado só durante o power-down).
    PRR  |= _BV(PRSPI);          // SPI nunca é usado (WS2812 é bit-bang)
    ACSR |= _BV(ACD);            // comparador analógico nunca é usado
    // Extras do 328PB (FI 1.5). O hex universal é compilado p/ 328P, então os
    // registradores novos do PB não têm NOME aqui — endereços conforme
    // iom328pb.h/DS40001906. No 328/328P real (FI 1.0) são reservados, por
    // isso o gate por placa.
    if (!placa10) {
        *(volatile uint8_t *)0x2D &= (uint8_t)~0x0F;  // DDRE : PE0..PE3 entrada
        *(volatile uint8_t *)0x2E |= 0x0F;   // PORTE: pullup — PE0..3 não têm
                                             // trilha na placa; flutuando drenam
        *(volatile uint8_t *)0x64 |= 0x10;   // PRR0.PRUSART1 (UART1 ociosa)
        *(volatile uint8_t *)0x65 |= 0x3D;   // PRR1: TIM3|SPI1|TIM4|PTC|TWI1
    }

    // (v2.16.2 tinha aqui um "detector de boot parasita" que mandava o MCU
    // dormir para sempre se o VCC parecesse baixo. REMOVIDO na v2.18.1: uma
    // leitura ruim do ADC bastava para a fechadura ficar MUDA até regravar
    // — risco inaceitável para um ganho que o BOOT SILENCIOSO (v2.16.3) já
    // entrega, quebrando a realimentação do loop sem tocar em energia.)

    // ⭐ v2.19: só segue o boot quando a energia for REAL (ver esperaEnergiaReal)
    if (EEPROM.read(EE_HIBERNA) == 1) esperaEnergiaReal();

    // ⭐ v2.18 TELEMETRIA: conta boots e, separadamente, os que vieram de
    // BROWN-OUT — a métrica que separa "religou pelo mosfet" de "morreu de
    // queda de tensão".
    // ⭐⭐ v2.23 — DUAS CORREÇÕES AQUI:
    //  (1) BODS só conta quando BORF está setado E PORF NÃO está. Sem isso todo
    //      power-on virava brown-out (ver comentário do RST em enviaInfo).
    //  (2) SÓ GRAVA EEPROM EM MODO SOAK. Estes contadores gravavam a cada boot
    //      — e o EE_CUTS, a cada ciclo de sono — reintroduzindo exatamente o
    //      desgaste que o firmware novo nasceu para eliminar (o antigo gravava
    //      6 bytes por ciclo). Medido no soak da 2910: 13 escritas em 39 min =
    //      ~480/dia = 100k (fim de vida da célula) em ~7 meses. Em campo a
    //      telemetria não serve para nada; na bancada, TST-SOAK a liga.
    if (EEPROM.read(EE_SOAK) == 1) {
        uint16_t n; EEPROM.get(EE_BOOTS, n); if (n == 0xFFFF) n = 0;
        n++; EEPROM.put(EE_BOOTS, n);
        if ((g_mcusr & _BV(BORF)) && !(g_mcusr & _BV(PORF))) {
            EEPROM.get(EE_BODS, n); if (n == 0xFFFF) n = 0;
            n++; EEPROM.put(EE_BODS, n);
        }
    }

    // ESTOU VIVO — a PRIMEIRA coisa, antes de tudo. Beep curto e AGUDO ao energizar.
    // Se não tocar = hardware/energia.
    beep(70, 2600);

#if FEATURE_SERIAL_DEBUG
    Serial.begin(DBG_BAUD);
    DBGLN(F("\n[boot] chavi_fi " FW_VERSION));
#endif

    randomSeed(analogRead(A0) ^ micros());
    // ⭐ v2.23 — PULL-UP REMOVIDO DO A0. O comentário da v2.12 dizia que o A0
    // "flutua de propósito", mas no esquemático da v2.7 o PC0 é o DIVISOR DO
    // TRILHO DE 5V — não está flutuando, tem resistores ligados nele. Ligar o
    // pull-up interno (20-50k para VCC) num divisor injeta corrente permanente
    // (50-100 µA, ou 10-15% de um orçamento de repouso de ~650 µA) e inviabiliza
    // qualquer leitura futura do trilho por esse canal. Deixar como entrada
    // pura: quem define o nível é o divisor.
    pinMode(A0, INPUT);

    // LEDs inicializados e APAGADOS. Nada aceso de forma CONTÍNUA no boot: os 3
    // WS2812 puxam corrente e, numa bateria fraca, seguravam o trilho baixo e
    // reiniciavam a fechadura em loop. Feedback visual = só PISCADAS curtas.
    // FI 1.0: LEDs discretos (7/8/9); o FastLED NUNCA é inicializado (o PB3 é
    // o motor B nessa placa — bit-bang de WS2812 ali chacoalharia o motor).
    // ⭐ v2.22: init via ledsInit() (idempotente) — o esperaEnergiaReal() acima
    // já pode ter inicializado, e um segundo addLeds empilharia controllers.
    ledsInit();
    ledCor(CRGB::Black);

    // serial (nome BLE) + estado da EEPROM
    for (uint8_t i = 0; i < 11; i++) {
        uint8_t c = EEPROM.read(EE_SERIAL + i);
        if (c == 0 || c == 0xFF) { serialFech[i] = 0; break; }
        serialFech[i] = c; serialFech[i + 1] = 0;
    }
    calibrationOk = EEPROM.read(EE_CALIB);
    // ⭐ v2.23: sanear em RAM escondia a corrupção — regrava a célula também,
    // senão o TST-INFO reporta um valor bonito sobre uma EEPROM suja.
    if (calibrationOk > 1) { calibrationOk = 0; EEPROM.update(EE_CALIB, 0); }
    EEPROM.get(EE_SEED01, seed01);
    EEPROM.get(EE_SEED02, seed02);
    // ⭐ v2.23 — EEPROM VIRGEM = 0xFFFFFFFF, não zero. Todos os outros bytes
    // eram normalizados; as seeds não. Consequências: o TST-INFO reportava
    // "SEEDS:OK" numa placa sem seeds (falso positivo na validação da bancada)
    // e a resposta do desafio (rA + v + seed01) estourava o unsigned long.
    if (seed01 == 0xFFFFFFFFUL) seed01 = 0;
    if (seed02 == 0xFFFFFFFFUL) seed02 = 0;

    // INA219 (detecção de batente do motor). Se não responder no I2C, o giro
    // cai no fallback por tempo — nunca trava o boot.
    // ⚠️ v2.23 — I2C SEM TIMEOUT: a Wire do MiniCore 3.0.1 (fixado no
    // sketch.yaml) é a 1.1, que NÃO tem `setWireTimeout()` — seus laços internos
    // de TWI são `while` infinitos. Um glitch no barramento (EMI do motor ou o
    // brown-out do arranque, ambos documentados na v2.17) trava o MCU DENTRO de
    // getCurrent_mA() com a ponte H energizada, e o motor fica ligado até a
    // bateria acabar. A proteção efetiva é o WATCHDOG armado em motorGira()
    // (reset em 8s -> o setup() reexecuta motorPara()); subir o MiniCore só por
    // causa disso mexeria no toolchain de 2.000+ placas e não compensa.
    inaOk = ina219.begin();
    if (inaOk) ina219.powerSave(true);
    g_moduloVers = EEPROM.read(EE_VERS_BLE);
    if (g_moduloVers == 0xFF || g_moduloVers > 40) g_moduloVers = 0;  // vazio/lixo
    g_moduloFam = EEPROM.read(EE_MOD_FAM);
    if (g_moduloFam > FAM_52) g_moduloFam = FAM_DESCONHECIDA;
    // Pino do MOSFET (gravado pelo gravar.sh/bancada; 90% da frota = 8).
    g_pinMosfet = EEPROM.read(EE_MOSFET);
    if ((g_pinMosfet < 4 || g_pinMosfet > 9) && g_pinMosfet != 12) g_pinMosfet = 8;
    g_wakeHib = (EEPROM.read(EE_HIB) == 1);
    if (g_wakeHib) EEPROM.update(EE_HIB, 0);
    // ⭐ v2.14.1 (generalizado; era só mosfet-auto): MCU nascendo com a CONEXÃO
    // já de pé (PD3 alto) = religamento por conexão — mosfet G1 religado pelo
    // AFTC (corte pós-uso do app!), pino-12 pelo PIO2, ou bateria trocada com
    // app conectado. Tem um cliente ESPERANDO: caminho rápido, sem config de
    // módulo (seria adiada de qualquer forma) e sem melodia. CRÍTICO p/ o
    // corte pós-uso: a sonda TST-PING do app chega ~1-2s após o connect e o
    // boot normal (identificação 5x) levava 2-4s = falso "firmware legado".
    if (digitalRead(PIN_WAKE) == HIGH) g_wakeHib = true;
    g_hiberna = (EEPROM.read(EE_HIBERNA) == 1);   // hibernação por MOSFET ligada?
    DBG(F("[boot] hiberna=")); DBGLN(g_hiberna);

    DBG(F("[boot] serial=")); DBG(serialFech[0] ? serialFech : "(fabrica)");
    DBG(F(" calib=")); DBG(calibrationOk);
    DBG(F(" seeds=")); DBG((seed01 && seed02) ? F("ok") : F("VAZIAS"));
    DBG(F(" versBLE=")); DBGLN(g_moduloVers);

    // Rádio: 9600 fixo (BAUD_MODULO). 1º boot após gravar = provisionamento completo
    // (converge baud + config + nome + reset); boots seguintes = config leve.
    // Boot de WAKE da hibernação = caminho RÁPIDO: o módulo já está configurado
    // e tem um app conectado ESPERANDO — nada de config/melodia, só atender.
    bluetooth.begin(BAUD_MODULO);
    if (g_wakeHib) {
        DBGLN(F("[boot] wake da hibernacao - atendendo direto"));
        return;                              // loop() atende já no 1º giro
    }
    // ⭐ PROVISIONAMENTO ADAPTATIVO (rápido no caso comum, sem storm):
    // Se o app conectar no meio, os AT vazam pro túnel e a verificação falha
    // (o módulo tunela o AT+VERS? em vez de responder). Antes o loop 3x + sweep
    // levava 60-80s e COLIDIA com a conexão da bancada. Agora:
    //  1) CAMINHO RÁPIDO (virgem/resetado JÁ está em 9600): config leve +
    //     verifica. ~3s. É o caso do campo (reset -> módulo volta a 9600).
    //  2) SÓ se falhar -> conversão pesada (sweep), no MÁXIMO 2 passadas.
    // Se PD3 estiver alto (app já conectado), nem tenta (vazaria) — adia.
    // ⭐⭐ v2.16.3 — BOOT SILENCIOSO EM PLACA JÁ PROVISIONADA (fim do loop
    // parasita, que virou F07 em campo). O MCU NÃO fala mais com o módulo no
    // boot quando a flag EE_MOD_CFG está marcada (a BANCADA é a dona da config
    // desde a v2.14 e a NVM do módulo guarda tudo entre ciclos de bateria).
    // PORQUÊ: com a placa cortada, o TX do módulo alimenta o MCU de forma
    // parasita; ele boota fraco e — ao mandar a dúzia de AT da "auto-cura" —
    // ACORDA o módulo, que mantém o TX alto: o ciclo nunca terminava (bipes,
    // primeiro acionamento degradado, F07). O firmware LEGADO não sofria disso
    // justamente porque só configurava o módulo no 1º boot.
    // Placa NÃO provisionada (gravação manual, sem bancada) mantém o
    // comportamento antigo: config + identificação + sweep.
    bool jaProvisionada = (EEPROM.read(EE_MOD_CFG) == MOD_CFG_MAGIC);
    if (jaProvisionada) {
        moduloOk = true;                           // config vive na NVM do módulo
        DBGLN(F("[boot] ja provisionada - boot silencioso (sem AT)"));
    } else if (digitalRead(PIN_WAKE) == HIGH) {
        DBGLN(F("[boot] conectado - provisionamento adiado"));
    } else {
        configModuloLeve();                        // caminho rápido
        moduloOk = (bleIdentificar() != 0);
        // ⭐ v2.13.3 ANTI-LOOP-DE-SUICÍDIO (caso 2910): o sweep pesado manda
        // AT+RESET — nas placas com mosfet o reboot do módulo derruba o gate e
        // CORTA o MCU no meio, que reboota e re-provisiona p/ sempre (bipe
        // agudo a cada ~2s; conectar pausa porque o provisionamento é adiado).
        // Regra: sweep SÓ se nunca provisionou (flag EE_MOD_CFG) e no máximo 3
        // tentativas (EE_PROV_TENT, zerado pelo seed.bin e no sucesso). Depois
        // disso o boot fica na config leve (sem reset — segura p/ o gate);
        // conserto adicional é pelo ar (bancada), que não depende do MCU.
        bool jaProv = (EEPROM.read(EE_MOD_CFG) == MOD_CFG_MAGIC);
        uint8_t tent = EEPROM.read(EE_PROV_TENT);
        if (tent == 0xFF) tent = 0;                // EEPROM virgem
        for (uint8_t t = 0; !moduloOk && !jaProv && tent < 3 && t < 2 &&
                            digitalRead(PIN_WAKE) == LOW; t++) {
            EEPROM.update(EE_PROV_TENT, ++tent);   // conta ANTES (pode morrer no reset)
            DBG(F("[boot] 9600 falhou - sweep passada ")); DBGLN(t + 1);
            bleProvisionar();                      // converte de qualquer baud -> 9600
            moduloOk = (bleIdentificar() != 0);
        }
        if (moduloOk) EEPROM.update(EE_PROV_TENT, 0);
    }
    // Verifica o módulo com RETRY p/ NÃO dar 4 beeps à toa: logo após o
    // provisionamento (AT+RESET) o módulo fica grogue e não responde AT+VERS? na
    // 1ª — era o falso "mudo" que tocava 4 graves mesmo com o BLE OK depois.
    for (uint8_t t = 0; !moduloOk && t < 5; t++) { delay(250); moduloOk = (bleIdentificar() != 0); }
    // v2.10.1: a flag de provisionado é HONESTA — só marca com o módulo vivo
    // (mudo fica sem flag e o próximo boot re-tenta na janela acordada dele).
    if (moduloOk) EEPROM.update(EE_MOD_CFG, MOD_CFG_MAGIC);
    DBG(F("[boot] moduloOk=")); DBGLN(moduloOk);

    // Feedback do boot: o aviso curto de sucesso toca na CONEXÃO (OK+CONN), não aqui.
    //   módulo OK  -> 2 piscadas VERDES (silencioso; "pronta")
    //   módulo MUDO -> 4 bipes GRAVES + vermelho (erro real de BLE) + diag de baud
    if (moduloOk) {
        piscar(CRGB::Green, 2);
    } else if (EEPROM.read(EE_MOD_CFG) == MOD_CFG_MAGIC) {
        // ⭐ v2.14: módulo mudo p/ CONSULTA mas JÁ PROVISIONADO (a bancada marca
        // a flag no seed.bin e configura o módulo PELO AR — caso dos R0 surdos
        // p/ AT do MCU, que funcionam 100% mesmo assim). Não é erro: sem 4
        // graves nem diagnóstico de baud a cada troca de bateria. 2 verdes.
        piscar(CRGB::Green, 2);
    } else {
        sinalModuloMudo();
        diagBaudBipes();
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
