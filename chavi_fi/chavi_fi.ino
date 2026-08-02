/*
 * chavi_fi.ino — Firmware BYPASS das fechaduras Chavi FI (setor imobiliário).
 *
 * Filosofia: confiabilidade > segurança (decisão do cliente). Não valida seed,
 * não valida token, aceita tudo — mas fala o MESMO protocolo dos ~1000 apps em
 * campo (desafio -> 2 saltos -> 3 writes -> comando), então nada muda no app.
 *
 * BAUD DO MÓDULO = 9600 (padrão de FÁBRICA — se o módulo resetar, volta pro
 * baud do firmware = auto-cura). O provisionamento do 1º boot converge módulos
 * em outros bauds via sweep às cegas (AT+BAUD2+AT+RESET em cada candidato — os
 * clones não respondem "OK" a um "AT" pelado, detecção por resposta não é
 * confiável). Boots seguintes = só config leve (sem reset).
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

#define FW_VERSION   "2.18.0"

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
uint16_t g_vccMinGiro = 0;       // menor VCC visto no último giro (mV)
bool g_motorAbortouVcc = false;  // giro parou por queda de trilho (v2.17)
uint16_t lerVccMv();             // (definida antes do setup)
bool g_sessaoConectada = false; // já tocou a melodia de "conectou" nesta sessão BLE
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

// ABRIR/FECHAR com SUCESSO: fanfarra do Rocky + 3 piscadas VERDES = "conseguiu!".
// A melodia toca 1× por acionamento concluído (não pesa como pesaria na conexão,
// que repete a cada reconexão do app — por isso a CONEXÃO fica com o aviso curto
// de 2 notas e o ACIONAMENTO ganha a fanfarra completa).
void fbComandoOk() {
    melodiaRocky();
    piscar(CRGB::Green, 3);
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
#define VCC_MOTOR_MIN_MV 4300      // trilho saudável = 5V (StepUp); abaixo disto
                                   // o próximo passo é o brown-out
void motorGira(bool sentidoA) {
    g_motorAbortouVcc = false;
    g_vccMinGiro = 0;
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
            // trilho afundando? para AGORA (antes do brown-out levar o MCU)
            if (millis() - t0 > MOTOR_ARRANQUE_MS) {
                uint16_t v = lerVccMv();
                if (v && (g_vccMinGiro == 0 || v < g_vccMinGiro)) g_vccMinGiro = v;
                if (v && v < VCC_MOTOR_MIN_MV) { g_motorAbortouVcc = true; break; }
            }
        }
        motorPara();
        ina219.powerSave(true);
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
    } else if (MOTOR_RECUO_MS > 0) {
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
    }
}

// ---- módulo BLE (sempre a 2400) ----------------------------------------------

// Manda um comando AT e descarta a resposta (não dependemos dela — os clones
// nem sempre respondem). O delay dá tempo do módulo processar.
// Terminado em '\r' como o FI_1_0/FI_1_0_400 de produção: o lote de módulos
// ANTIGO (ver.03/04 das FI 1.0) exige o CR; o lote novo (ver.05) tolera —
// o FI_1_0_400 sempre mandou com '\r' nos mesmos módulos "Soft AT 5.2".
void at(const char* c, uint16_t w = 150) {
    // ⛔ TRAVA CRÍTICA: se o app está CONECTADO (PD3 alto), o módulo está em
    // MODE2 túnel e NÃO interpreta AT — ele REPASSA o "AT+..." como DADO pro app
    // (visto na bancada: "⟵ AT", "⟵ AT+NAME003FI002734" + lixo). AT é só p/
    // config, que só roda DESCONECTADO. Conectado, não manda nada.
    if (digitalRead(PIN_WAKE) == HIGH) return;
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

// MCUSR guarda o MOTIVO do último reset (power-on / brown-out / externo /
// watchdog). Precisa ser lido ANTES do init do core; a seção .init3 roda antes
// do main(). É o que distingue "religou pelo mosfet" de "morreu de brown-out".
uint8_t g_mcusr __attribute__((section(".noinit")));
void capturaMcusr(void) __attribute__((naked, used, section(".init3")));
void capturaMcusr(void) { g_mcusr = MCUSR; MCUSR = 0; }

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
    //         (CUTS≈BOOTS = corte funciona; CUTS subindo com BOOTS parado =
    //          o módulo ignorou o comando)
    //  VCC    tensão do trilho agora · VCCMIN a MENOR vista no último giro
    { uint16_t bo, bd, ct;
      EEPROM.get(EE_BOOTS, bo); EEPROM.get(EE_BODS, bd); EEPROM.get(EE_CUTS, ct);
      if (bo == 0xFFFF) bo = 0; if (bd == 0xFFFF) bd = 0; if (ct == 0xFFFF) ct = 0;
      snprintf_P(buf, sizeof(buf), PSTR("RST:%c"),
                 (g_mcusr & _BV(BORF)) ? 'B' : (g_mcusr & _BV(WDRF)) ? 'W'
                 : (g_mcusr & _BV(EXTRF)) ? 'E' : 'P');
      enviaLinha(buf);
      snprintf_P(buf, sizeof(buf), PSTR("BOOTS:%u"), bo);   enviaLinha(buf);
      snprintf_P(buf, sizeof(buf), PSTR("BODS:%u"), bd);    enviaLinha(buf);
      snprintf_P(buf, sizeof(buf), PSTR("CUTS:%u"), ct);    enviaLinha(buf); }
    snprintf_P(buf, sizeof(buf), PSTR("VCC:%u"), lerVccMv());       enviaLinha(buf);
    snprintf_P(buf, sizeof(buf), PSTR("VCCMIN:%u"), g_vccMinGiro);  enviaLinha(buf);
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
        at("AT+DROP", 500);                  // derruba a conexão -> módulo sai do túnel
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
        { uint16_t c; EEPROM.get(EE_CUTS, c); if (c == 0xFFFF) c = 0;
          c++; EEPROM.put(EE_CUTS, c); }        // telemetria: tentei cortar
        at("AT+DROP", 200);
        delay(400);               // o re-apply do BEFC da desconexão acontece AQUI
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
uint16_t lerVccMv() {
    ADMUX  = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);  // AVcc ref, canal = bandgap 1V1
    delay(3);                       // o bandgap precisa assentar
    ADCSRA |= _BV(ADSC);
    while (ADCSRA & _BV(ADSC));
    uint16_t adc = ADC;
    if (adc == 0) return 0;
    return (uint16_t)(1125300UL / adc);          // 1,1V * 1023 * 1000 / adc
}
#define VCC_MIN_BOOT_MV 4200        // abaixo disto o boot é parasita (real = ~5V)

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

    // ⭐ v2.16.2: BOOT PARASITA? (placa cortada sendo alimentada pelo TX do
    // módulo). Silêncio absoluto, nenhum AT, e power-down imediato — sem isso
    // o ciclo bipe->AT->acorda módulo->TX alto->bipe nunca terminava.
    // Guarda: só vale nas placas com mosfet (as sem gate nunca ficam cortadas)
    // e o teste é repetido (uma leitura isolada pode pegar o StepUp subindo).
    if (EEPROM.read(EE_HIBERNA) == 1) {          // (g_hiberna só é lido adiante)
        uint16_t vcc = lerVccMv();
        if (vcc && vcc < VCC_MIN_BOOT_MV) {
            delay(50);
            uint16_t vcc2 = lerVccMv();
            if (vcc2 && vcc2 < VCC_MIN_BOOT_MV) {
                ADCSRA &= ~_BV(ADEN);              // ADC off
                set_sleep_mode(SLEEP_MODE_PWR_DOWN);
                sleep_enable(); sleep_bod_disable(); sleep_cpu();   // não volta
            }
        }
    }

    // ⭐ v2.18 TELEMETRIA: conta boots e, separadamente, os que vieram de
    // BROWN-OUT (BORF no MCUSR) — a métrica que separa "religou pelo mosfet"
    // (power-on/PORF) de "morreu de queda de tensão".
    { uint16_t n; EEPROM.get(EE_BOOTS, n); if (n == 0xFFFF) n = 0;
      n++; EEPROM.put(EE_BOOTS, n);
      if (g_mcusr & _BV(BORF)) {
          EEPROM.get(EE_BODS, n); if (n == 0xFFFF) n = 0;
          n++; EEPROM.put(EE_BODS, n);
      } }

    // ESTOU VIVO — a PRIMEIRA coisa, antes de tudo. Beep curto e AGUDO ao energizar.
    // Se não tocar = hardware/energia.
    beep(70, 2600);

#if FEATURE_SERIAL_DEBUG
    Serial.begin(DBG_BAUD);
    DBGLN(F("\n[boot] chavi_fi " FW_VERSION));
#endif

    randomSeed(analogRead(A0) ^ micros());
    pinMode(A0, INPUT_PULLUP);   // v2.12: A0 só serve pra semente (flutuando de
                                 // propósito na leitura acima); depois dela, pullup
                                 // pra não ficar flutuando o resto do tempo

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
