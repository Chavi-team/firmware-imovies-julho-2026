/*
  Firmware nova produção FIs - FI 1.5

  Atualizacao FI = 360 - Alteracoes SOFT para adequacao da FI com a comunicacao dos BLE v3 e v4, adicionado delay de 50ms no setupCalibration para compatibilizar com o app iOS
                       - Alterado bluetooth.println para SendDataBLE

*/

//*****************************************************************************************************************
// Library(ies)
//*****************************************************************************************************************

#include <Adafruit_INA219.h>  // Library to INA219
#include <EEPROM.h>           // Library to read/write EEPROM
#include <FastLED.h>
#include <Wire.h>
#include <avr/wdt.h>  // Library to watchdog timerDEBUG
#include <stdlib.h>   // Library to calculate basics
#include <string.h>

#include "LowPower.h"        // Library to low power device
#include "SoftwareSerial.h"  // Library to communicate BLE1010 with ATMEGA328P
#include "interface.h"
#include "interrupt.h"
#include "version.h"
#include "utils.h"
#include "debug.h"
#include "SerialNumber.h"

//*****************************************************************************************************************
// Contant(s)
//*****************************************************************************************************************

// Hardware mapping
#define pinRxBLE1010 PIN_PD4
#define pinTxBLE1010 PIN_PD5
#define pinBuzzer PIN_PD6
#define pinLEDs PIN_PB3

// To motor
#define pinTurn01 PIN_PB1
#define pinTurn02 PIN_PB2

// To battery level
#define pinBattery01 PIN_PC1  // Bateria
#define pinBattery02 PIN_PC0  // 5V

// To wakeup uC
#define pinPushButton PIN_PD2
#define pinWakeuC PIN_PD3

// To LEDs
#define NUM_LEDS 3
#define MAX_BRIGHT 150

// To serial
#define BAUDSerial 9600  // SOFT - FI = 360 - Alterada a macro de SERIAL para BAUDSerial para nao conflitar com SERIAL de outra library que tinha valor 1
#define CMDBUFFER_SIZE 32

// To time
#define timeToWait 1
#define timeToWaitBLE1010 500
#define timeToRotation 6000

#define timeToLineUP 1000

#define timeOutGoToSleep 20000
#define timeToSoftStart 1500
#define timeoutMotorProduction 5000

// To commands in the BLE1010
// To receive BLE1010
#define DelimSerial "AT+UTIM0"  // SOFT - FI = 360 - Comando para alterar comportamento de rx dados na serial, 0 = Delimitador (\n ou \r), 1 = Timeout Serial
#define testBLE1010 "AT"        // Command AT to test
#define desactDefPasBLE1010 "AT+TYPE0"
#define modeRCBLE1010 "AT+MODE2"
#define roleSlaveBLE1010 "AT+ROLE0"
#define delimitBLE1010 "AT+DELI3"
#define baud9600BLE1010 "AT+BAUD2"
#define actNotiBLE1010 "AT+NOTI1"
#define preConnBLE1010 "AT+BEFC020"
#define posConnBLE1010 "AT+AFTC028"
#define toStatePIO60 "AT+PIO61"
#define nameBLE1010producao "AT+NAMECHAVIFIPR"
#define resetBLE1010 "AT+RESET"
#define lostConnecBLE1010 "AT+DROP"
#define ENDFrameVer03 '\n'  // SOFT - FI = 360 - Final de frame para BLE v03
#define ENDFrameVer04 '\0'  // SOFT - FI = 360 - Final de frame para BLE v04

// Define os comandos BLE e as respostas esperadas para diferentes versões do BLE
#define PFXVersaoBLE "Soft AT 5.2 ver."     // Prefixo da string de resposta da versao do BLE
#define Versao03BLE "Soft AT 5.2 ver.03\n"  // Resposta da versao do BLE para Rev03
#define Versao04BLE "Soft AT 5.2 ver.04\n"  // Resposta da versao do BLE para Rev04

#define ENDWriteCommand '\0'  // SOFT - FI = 360 - Finalizador de string para comandos
#define ENDWriteData '\r'     // SOFT - FI = 360 - Finalizador de string para dados

#define SEEDDelimData '\n'   // SOFT - FI = 360 - Delimitador de dados para o envio das seeds durante o setup inicial no App, onde compatibiliza a v03 com v04 do BLE
#define CALIBDelimData '\n'  // SOFT - FI = 360 - Delimitador de dados para o envio das respostas de calibracao para o App, onde compatibliza a v04 com a v04 do BLE

#define setStatusPIO6_BLE1010 "AT+STATUS8"
// To ask BLE1010
#define askMACBLE1010 "AT+ADDR?"  // Command AT to ask the MAC adress
#define versBLE1010 "AT+VERS?"
#define SIZEPfxVersaoBLE 16  // SOFT - FI = 360 - Tamanho da string de prefixo da versao de BLE
#define SIZEVersaoBLE 2      // SOFT - FI = 360 - Tamanho da versao do BLE
#define SIZEAddrMAC 20       // SOFT - FI = 360 - Tamanho da resposta do MAC Address do BLE
#define VersaoFW(V) (V)      // SOFT - FI = 360 - Macro para versao de FW do BLE

// To EEPROM
#define setupSeedOk 1
#define generalSetupOk 2
#define calibrationOk 3
#define verifierCalibration 4
#define address01Seed01 5
#define address01Seed02 15
#define address01Seed03 25
#define address01Seed04 35
#define address01MAC 45
#define configurationDevice 100
#define setupProductionOk 150

// To PWM
#define minValuePWM 0
#define maxValuePWM MAX_BRIGHT

// To sound brand
#define maxNotes 3
#define timeToTone 500
#define toneDefault 1000

// To battery level
#define constBatteryLevel 5 / 1024
#define constBatteryLevelAnalog 0.0047875855  // Novo Calculo
#define maxValueBattery 4.08
#define minValueBattery 3.40
#define percBattery 100
#define showTheBatteryLevel 1000
#define voltageThresholdLow 3.40       // Adicione esta linha para definir o limite de tensão baixa
#define voltageThresholdCritical 3.25  // Adicione esta linha para definir o limite de tensão crítica

// To INA219
#define INA_Address 0x45
#define nSamples 100
#define stallMotor 300
#define timeoutMotor 10000
#define turnOne3 -10

// To setup deveice
#define token03SetupDevice 150993
#define tokenSetupSeed 140197
#define tokenCalibration 190720
#define testMotor 1509

// To calibration
#define commandToStartCali "CALIBRACAO-FI"
#define commandToDoorOpen "PORTA-ABERTA"
#define commandToDoorClose "PORTA-FECHADA"

// To production
#define commandToBLE1010 "CHAVI-PRODUCAO"
#define commandToMotor "MOTOR-TESTE"
#define commandToAuto "TESTE-AUTO"
#define commandToBotao "TESTE-BOTAO"

// To panic
#define valueToPanic 5
#define timeToPanicSegurite 5000

// sleep

unsigned long previousMillis = 0;
const unsigned long interval = 60000;  // 60 segundos em milissegundos
bool sleepCalled = true;
volatile bool bleActivated = false;

//*****************************************************************************************************************
// Prototype of the function(s)
//*****************************************************************************************************************

// To setup
void setupSerials();
void setupPins();
void setupInterrupts();
void setupBLE01(bool prodOK = true);
void setupADC();
void setupEEPROM();
void setupI2C();
void seedSetup();
void receiveSeed();
void sendSeedOk();
void changeName(bool lenta = true);
void interruptSetup();
void pushButtonInterrupt();
void bluetoohInterrupt();
void setupProduction();
void operateProduction();
char processCharInput(char *cmdBuffer, const char c);
void setupCalibration();

// To operate
void turnOnLEDsDelay(CRGB crgb = CRGB(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT), int atraso = 0);
void turnOnLEDs(CRGB crgb = CRGB(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT));
void turnOffLEDs();
void stopMotor();
void rotateMotor01();
void rotateMotor02();
void functionPBOperate();
void operateAllOk();
void readBLE1010();
void setupDeviceComplete();
void readCalibration();
void turnMotor();
float readCurrentInmA();
void goToSleep();
void interfaceFI(Interface flagInterface);
void readBatteryLevel();
void disconnectBLE1010();
void activeBLE1010Connect();
unsigned long getpass_do_lolis(unsigned long difference, unsigned long seed);
void readINA219();
void turnTheMotor01();
void turnTheMotor02();
void soundBrand();
void showBatteryLevel();
void functionPanic();
void functionDelta();
unsigned long send_saltos(unsigned long random_input, unsigned long seed);
void toneDefaultByTime(unsigned int duration, unsigned int frequency = toneDefault);
String RespostaBLE(bool lenta = true);
String rotinaWriteBluetooth(const char *str, bool lenta = true);
void setupConfigurationDevice();
void clearCMDBuffer();
char CheckVersBLE(void);                                 // SOFT - FI = 360 - Funcao para identificar versao do BLE, para saber se precisa do \n ou nao, separa v03 de v04 do BLE
void SendDataBLE(const char *TxData, char AddData = 0);  // SOFT - FI = 360 - Funcao para enviar os dados de transmissao para o BLE, compatibliza v03 e v04 do BLE

//*****************************************************************************************************************
// Object(s)
//*****************************************************************************************************************

// To BLE1010
SoftwareSerial bluetooth(pinTxBLE1010, pinRxBLE1010);  // TX, RX (Bluetooth)

// To INA219
Adafruit_INA219 ina219(INA_Address);

// To LEDs
CRGB leds[NUM_LEDS];

CRGB corOK(0, MAX_BRIGHT, 0);
CRGB corNOTOK(MAX_BRIGHT, 0, 0);

//*****************************************************************************************************************
// Global variable(s)
//*****************************************************************************************************************

// Variables to setup BLE1010
char VersBLE = 0;  // SOFT - FI = 360 - Variavel que armazena a versao do BLE, para durante o codigo tomar decisoes baseado nesse valor
String command = "";
String numberMacAddress = "";

char b1;

// To seed
unsigned long inputBLE1010;
unsigned long inputSeed01;
unsigned long inputSeed02;
unsigned long inputSeed03;
unsigned long inputSeed04;
unsigned long inputTimeStamp;
unsigned long verifierSeed;
int contadorSeed = 0;
int counterToken = 0;
int flagSeedOk = 0;
char flagSetupSeed = 0x00;
char flagReadBLE1010 = 0x00;
char flagSeedSetup = 0x00;
unsigned long numberToken01;
unsigned long numberToken02;
unsigned long numberToken03;
unsigned long numberToken04;
unsigned long timeOutReceiveiSeed = 0;
unsigned long timeOutSeed = 30000;
char flagTimeoutSeed = 0x00;

// To token
unsigned long token01 = 0;
unsigned long token02 = 0;
unsigned long token03 = 0;
unsigned long token04 = 0;
unsigned long receiveToken = 0;
unsigned long receiveRandom = 0;
unsigned long randNumber = 0;
unsigned long saltos_1 = 0;
unsigned long saltos_2 = 0;

// To sound brand
constexpr int sBrandOpen[] = {262, 392, 330};
constexpr int sBrandClose[] = {349, 330, 262};
char flagSoundBrand = 0x00;
constexpr int timeToToneSeedSetup = 3000;
constexpr int timeToToneDefault = 300;

// To Interrupt
volatile InterruptSource flagInterrupt = NONE;
char flagSelectInterrupt = 0x00;

// To motor
int flagStateMotor = 0;

// To battery
float batteryLevelFloat = 0;
float percBatteryLevel = 0;
float oldPercBatteryLevel = 100;
float batteryLevel = 0;
float batteryLevelAnalog = 0;
float batteryLevelAnalogddp = 0;

#define statusDoorOpen 1000
#define statusDoorClose 2000

char flagShowBatteryLevel = 0x00;
const float maxScaleBattery = maxValueBattery - minValueBattery;

// To time
unsigned long timeOutDKCount = 0;
volatile char flagPressButton = 0x00;

// To INA219
float currentInmA = 0;
float deltaD = 0;
unsigned long timeoutStallMotor = 0;
unsigned long timeoutStallMotor02 = 0;
unsigned long timeoutStallMotor03 = 0;
char flagStallMotor = 0x00;

// To reset
volatile int counterResetDance = 0;

// To setup device complete
char flagProximityOpening = 0x00;
char flagWarningSound = 0x00;
char flagLightWarning = 0x00;
char flagLockTurns = 0x00;
char flagLockTurnsError01 = 0x00;
char flagLockTurnsError02 = 0x00;
char flagButton = 0x00;
char flagAutomaticClosing = 0x00;
char flagDoorUnloockingWarning = 0x00;
char flagOpenDoorWarning = 0x00;

// To production
int countMotorProduction = 0;
char flagOperateProduction = 0x00;

// To panic
char flagToPanic = 0x00;

// To close only
unsigned long timeToCloseTheDoor;
unsigned long timeToClose = 0;

// To calibration
char flagCalibrationFI01 = 0x00;
char flagCalibrationFI02 = 0x00;
char flagCalibrationFI03 = 0x00;
char flagTurnMotor = 0x00;
char flagStatusDoor = 0x00;
char flagStatusDoor02 = 0x00;
char flagVerifierCalibration = 0x00;
char flagLineUpMotor = 0x00;
int counterLockTurns01 = 0;
int counterLockTurns02 = 0;
static char cmdBuffer[CMDBUFFER_SIZE] = "";

//*****************************************************************************************************************
// To reset device
//*****************************************************************************************************************

void (*funcReset)() = 0;

void interruptSetup() {
    flagPressButton = 0x01;
}

//*****************************************************************************************************************
// Function reset dance
//*****************************************************************************************************************

void resetDance() {
    counterResetDance = 0;

    while (!digitalRead(pinPushButton)) {
        interfaceFI(ON);
        counterResetDance++;

        if (counterResetDance == RESET_INTERVAL) {
            counterResetDance = 0;

            interfaceFI(ON_TONESEED_OFF);  // 0x07

            delay(3000);

            EEPROM.update(setupSeedOk, 0x00);

            funcReset();
        }

        delayInterrupt(1000);
    }

    counterResetDance = 0;
    flagPressButton = 0x00;
}

//*****************************************************************************************************************
// Interrupt 00
//*****************************************************************************************************************

void pushButtonInterrupt() {
    flagInterrupt = PUSH_BUTTON;
}

//*****************************************************************************************************************
// Interrupt 01
//*****************************************************************************************************************

void bluetoohInterrupt() {
    flagInterrupt = BLUETOOTH;
}

// SOFT - FI = 360 -
// CheckVers BLE
// params - none
// return char = Aux, estado compaacao prefixo e resposta
char CheckVersBLE(void) {
    if (VersBLE != 0) {
        return VersBLE;
    }  // Sai se a versão já é conhecida

    char Aux;              // SOFT - FI = 360 - Variavel Auxiliar
    char TmpStr[3] = "";   // SOFT - FI = 360 - Str temporaria
    String RespVers = "";  // SOFT - FI = 360 - Resposta Versao

    RespVers = rotinaWriteBluetooth(versBLE1010, true);                       // SOFT - FI = 360 - Requisita a versao do BLE
    Aux = strncmp(RespVers.c_str(), PFXVersaoBLE, SIZEPfxVersaoBLE);          // SOFT - FI = 360 - Compara resposta do BLE se tem o prefixo da versao
    if (!Aux) {                                                               // SOFT - FI = 360 - Aux = 0 -> Prefixo = Resposta, Aux != 0 -> Prefixo != Resposta
        strncpy(TmpStr, &RespVers.c_str()[SIZEPfxVersaoBLE], SIZEVersaoBLE);  // SOFT - FI = 360 -
        VersBLE = (TmpStr[0] - '0') * 10 + TmpStr[1] - '0';                   // SOFT - FI = 360 -
    } else {
        asm("nop");  // SOFT - FI = 360 - Deu erro na leitura da versao, nao faz nada
    }

    EEPROM.put(0x300, VersBLE);
    return Aux;  // SOFT - FI = 360 - Retorna Aux
}

//*****************************************************************************************************************
// Initial settings
//*****************************************************************************************************************
long startTime;
void setup() {
    startTime = millis();
    // Inicializações necessárias
    Serial.begin(9600);
    // Outras inicializações, se houver
    Serial.println("Iniciando...");
    setupSerials();
    setupPins();

    if (EEPROM.read(setupSeedOk) != 0x01) {  // entra aqui se não está configurado
        setupEEPROM();
        interfaceFI(ON_TONESEED_OFF);  // 0x07
        CheckVersBLE();                // SOFT - FI = 360 - Confere a versao do BLE antes de executar comandos e enviar dados
        setupBLE01(true);
        setupI2C();
        interfaceFI(ON_TONESEED);  // 0x08
        seedSetup();

        EEPROM.update(calibrationOk, 0x00);
        EEPROM.update(verifierCalibration, 0x00);
        EEPROM.update(configurationDevice + 1, 0x01);
        EEPROM.update(configurationDevice + 2, 0x01);
        EEPROM.update(configurationDevice + 3, 0x00);
        EEPROM.update(configurationDevice + 4, 0x01);
        EEPROM.update(configurationDevice + 5, 0x01);
        flagVerifierCalibration = EEPROM.read(calibrationOk);
        flagStatusDoor = EEPROM.read(verifierCalibration);
        flagWarningSound = EEPROM.read(configurationDevice + 1);
        flagLightWarning = EEPROM.read(configurationDevice + 2);
        flagButton = EEPROM.read(configurationDevice + 4);

        interfaceFI(OFF);  // 0x05

        delay(500 * timeToWait);
        funcReset();
    }

    if ((EEPROM.read(setupProductionOk) == 0x01) && (EEPROM.read(setupSeedOk) == 0x01)) {
        setupEEPROM();

        CheckVersBLE();                     // SOFT - FI = 360 - Confere a versao do BLE antes de executar comandos e enviar dados
            interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

        flagSelectInterrupt = 0x01;  // Habilita função de reset do botão
        setupInterrupts();

        setupADC();
        setupI2C();
        interfaceFI(FADEIN_FADEOUT);

        if (flagPressButton == 0x01) {
            resetDance();
        }

        flagSelectInterrupt = 0x02;  // Desabilita função de reset do botão
        setupInterrupts();

        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
        changeName();
    }
}

//*****************************************************************************************************************
// Function to setup production and operate - START
//*****************************************************************************************************************

void setupProduction() {
    while (flagOperateProduction == 0x01) {
        char c;

        if (bluetooth.available() > 0) {
            while (bluetooth.available()) {
                c = processCharInput(cmdBuffer, bluetooth.read());

                if (c == '\n') {
                    operateProduction();
                    clearCMDBuffer();
                }
            }
        }
    }
}

void funcCounterMotorProduction() {
    timeoutStallMotor = millis();
    while (millis() - timeoutStallMotor < timeoutMotorProduction) {
        currentInmA = readCurrentInmA();
        if ((abs(currentInmA)) > 10) {
            countMotorProduction++;
        }
        currentInmA = 0;
    }
    stopMotor();
}

void operateProduction() {
    //*******************************
    // To test BLE1010
    //*******************************
    if (strcmp(commandToBLE1010, cmdBuffer) == 0) {
        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

        SendDataBLE("11");  // SOFT - FI = 360
        delay(1000 * timeToWait);
    } else if (strcmp(commandToMotor, cmdBuffer) == 0) {
        //*******************************
        // To test MOTOR
        //*******************************
        rotateMotor02();
        funcCounterMotorProduction();

        delay(2000 * timeToWait);

        rotateMotor01();
        funcCounterMotorProduction();

        if (countMotorProduction >= 15) {
            countMotorProduction = 0;

            SendDataBLE("11");  // SOFT - FI = 360
        } else {
            Serial.println(__LINE__);
            SendDataBLE("22");  // SOFT - FI = 360
        }

        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
    } else if (strcmp(commandToAuto, cmdBuffer) == 0) {
        //*******************************
        // To test PERIPHERALS
        //*******************************
        // To test LEDs/SOUND BRAND/BUZZER
        //*******************************
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT), 250 * timeToWait);          // Branco
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, 0, 0), 250 * timeToWait);                            // Vermelho
        turnOnLEDsDelay(CRGB(0, MAX_BRIGHT, 0), 250 * timeToWait);                            // Verde
        turnOnLEDsDelay(CRGB(0, 0, MAX_BRIGHT), 250 * timeToWait);                            // Azul
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, MAX_BRIGHT, 0), 250 * timeToWait);                   //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, 0, MAX_BRIGHT), 250 * timeToWait);                   //
        turnOnLEDsDelay(CRGB(0, MAX_BRIGHT, MAX_BRIGHT), 250 * timeToWait);                   //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT / 2, MAX_BRIGHT, MAX_BRIGHT), 250 * timeToWait);      //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, MAX_BRIGHT / 2, MAX_BRIGHT), 250 * timeToWait);      //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, MAX_BRIGHT, MAX_BRIGHT / 2), 250 * timeToWait);      //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT / 2, MAX_BRIGHT / 2, MAX_BRIGHT), 250 * timeToWait);  //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT, MAX_BRIGHT / 2, MAX_BRIGHT / 2), 250 * timeToWait);  //
        turnOnLEDsDelay(CRGB(MAX_BRIGHT / 2, MAX_BRIGHT, MAX_BRIGHT / 2), 250 * timeToWait);  //

        turnOffLEDs();

        delay(2000 * timeToWait);

        int i;
        for (i = 0; i < maxNotes - 1; ++i) {
            toneDefaultByTime(timeToTone, sBrandOpen[i]);
        }
        toneDefaultByTime(timeToTone * 2, sBrandOpen[i]);

        delay(2000 * timeToWait);

        for (i = 0; i < maxNotes - 1; ++i) {
            toneDefaultByTime(timeToTone, sBrandClose[i]);
        }
        toneDefaultByTime(timeToTone * 2, sBrandClose[i]);

        delay(2000 * timeToWait);

        SendDataBLE("11");  // SOFT - FI = 360
        delay(1000 * timeToWait);

        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
    } else if (strcmp(commandToBotao, cmdBuffer) == 0) {
        //*******************************
        // To test PUSH BUTTON
        //*******************************
        while (digitalRead(pinPushButton)) {
            interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
            delay(1000 * timeToWait);
        }

        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

        flagOperateProduction = 0x00;

        SendDataBLE("11");  // SOFT - FI = 360
        delay(1000 * timeToWait);
        disconnectBLE1010();
        delay(500 * timeToWait);
    }
}

//*****************************************************************************************************************
// Process char input
//*****************************************************************************************************************

char processCharInput(char *cmdBuffer, const char c) {
    // Store the character in the input buffer
    // Ignore control characters and special ascii characters
    if (c >= 32 && c <= 126) {
        if (strlen(cmdBuffer) < CMDBUFFER_SIZE) {
            strncat(cmdBuffer, &c, 1);  // Add it to the buffer
        } else {
            return '\n';
        }
    } else if ((c == 8 || c == 127) && cmdBuffer[0] != 0) {  // Backspace
        cmdBuffer[strlen(cmdBuffer) - 1] = 0;
    }

    return c;
}

//*****************************************************************************************************************
// Function to setup production - END
//*****************************************************************************************************************

//*****************************************************************************************************************
// Function to setup serials
//*****************************************************************************************************************

void setupSerials() {
    Serial.begin(BAUDSerial);     // Monitor Serial Arduino
    // bluetooth.begin(BAUDSerial);  // Terminal BLE
    // bluetooth.write("AT+UART1");
    // bluetooth.write("AT+BAUD0");
    // bluetooth.end();
    bluetooth.begin(2400);
}

//*****************************************************************************************************************
// Function to setup pins
//*****************************************************************************************************************

void setupPins() {
    FastLED.addLeds<WS2812, pinLEDs, GRB>(leds, NUM_LEDS);
    pinMode(pinBuzzer, OUTPUT);
    pinMode(pinTurn01, OUTPUT);
    pinMode(pinTurn02, OUTPUT);
    pinMode(pinPushButton, INPUT_PULLUP);

    turnOffLEDs();
    digitalWrite(pinBuzzer, LOW);
    stopMotor();
}

//*****************************************************************************************************************
// Function to setup EEPROM
//*****************************************************************************************************************

void setupEEPROM() {
    VersBLE = EEPROM.read(0x300);
    if (EEPROM.read(setupSeedOk) != 0x01) {
        flagSetupSeed = 0x01;
    } else {
        flagSetupSeed = 0x00;

        setupConfigurationDevice();

        flagSeedSetup = EEPROM.read(setupSeedOk);
        flagReadBLE1010 = EEPROM.read(generalSetupOk);
        inputSeed01 = EEPROM.get(address01Seed01, inputSeed01);
        inputSeed02 = EEPROM.get(address01Seed02, inputSeed02);
        inputSeed03 = EEPROM.get(address01Seed03, inputSeed03);
        inputSeed04 = EEPROM.get(address01Seed04, inputSeed04);
    }
}

//*****************************************************************************************************************
// Function to setup configuration device
//*****************************************************************************************************************

void setupConfigurationDevice() {
    flagStatusDoor02 = EEPROM.read(calibrationOk);
    flagVerifierCalibration = EEPROM.read(verifierCalibration);
    flagProximityOpening = EEPROM.read(configurationDevice);
    flagWarningSound = EEPROM.read(configurationDevice + 1);
    flagLightWarning = EEPROM.read(configurationDevice + 2);
    flagLockTurns = EEPROM.read(configurationDevice + 3);
    flagButton = EEPROM.read(configurationDevice + 4);
    flagAutomaticClosing = EEPROM.read(configurationDevice + 5);
    flagDoorUnloockingWarning = EEPROM.read(configurationDevice + 6);
    flagOpenDoorWarning = EEPROM.read(configurationDevice + 7);
}

//*****************************************************************************************************************
// Function to setup serial BLE 01
//*****************************************************************************************************************

void setupBLE01(bool prodOK) {
    //------------------------------------
    // To test commands AT BLE1010
    //------------------------------------
    rotinaWriteBluetooth("AT+PWRM1");
    rotinaWriteBluetooth(testBLE1010);

    // FIXME Remover quando for implementado a interrupção por timer em interfaceFI
    interfaceFI(FADEIN_FADEOUT);

    //------------------------------------
    // To desactived password BLE1010
    //------------------------------------

    rotinaWriteBluetooth(desactDefPasBLE1010);

    //------------------------------------
    // To mode receive data BLE1010
    //------------------------------------

    // FIXME Remover quando for implementado a interrupção por timer em interfaceFI
    interfaceFI(FADEIN_FADEOUT);

    //------------------------------------
    // To mode role slave BLE1010
    //------------------------------------

    rotinaWriteBluetooth(roleSlaveBLE1010);

    // FIXME Remover quando for implementado a interrupção por timer em interfaceFI
    interfaceFI(FADEIN_FADEOUT);

    //------------------------------------
    // To work? BLE1010
    //------------------------------------
    rotinaWriteBluetooth(delimitBLE1010);

    //------------------------------------
    // To active the notify BLE1010
    //------------------------------------

    rotinaWriteBluetooth(actNotiBLE1010);

    // FIXME Remover quando for implementado a interrupção por timer em interfaceFI
    interfaceFI(FADEIN_FADEOUT);

    String auxiliarLocal = rotinaWriteBluetooth(versBLE1010);

    if (strcmp(auxiliarLocal.c_str(), Versao03BLE) == 0) {
        //------------------------------------
        // To work in this 'Versao03BLE' version
        //------------------------------------
        rotinaWriteBluetooth(setStatusPIO6_BLE1010);
    } else {
        //------------------------------------
        // To pre connection BLE1010
        //------------------------------------
        // rotinaWriteBluetooth(preConnBLE1010);

        //------------------------------------
        // To pos connection BLE1010
        //------------------------------------

        // rotinaWriteBluetooth(posConnBLE1010);
    }

    // FIXME Remover quando for implementado a interrupção por timer em interfaceFI
    interfaceFI(FADEIN_FADEOUT);
    //------------------------------------
    // To state after setup PIO6 connection BLE1010
    //------------------------------------

    rotinaWriteBluetooth(toStatePIO60);

    //------------------------------------
    // To set the name BLE1010
    //------------------------------------

    // FIXME Substituir por `rotinaWriteBluetooth(prodOK ? nameBLE1010 : nameBLE101producao)`
    if (prodOK) {
        rotinaWriteBluetooth("AT+NAMECHAVIFI");
    } else {
        rotinaWriteBluetooth(nameBLE1010producao);
    }
    // //------------------------------------
    // // Ask the value of the MAC Address
    // //------------------------------------

    interfaceFI(FADEIN_FADEOUT);     // 0x01
    bluetooth.write(askMACBLE1010);  // SOFT

    delay(timeToWaitBLE1010);
    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                numberMacAddress = command.substring(command.indexOf(':') + 1);

                b1 = 0;
                command = "";
            }
        }
    }

    interfaceFI(ON);  // 0x04
}

//*****************************************************************************************************************
// Function to setup ADC
//*****************************************************************************************************************

void setupADC() {
    pinMode(pinBattery01, INPUT);  // Setup pin to battery level of the battery
    pinMode(pinBattery02, INPUT);  // Setup pin to battery level of the 5V
}

//*****************************************************************************************************************
// Function to setup RTC
//*****************************************************************************************************************

void setupI2C() {
    ina219.begin();
}

//*****************************************************************************************************************
// Function to seed setup
//*****************************************************************************************************************

void seedSetup() {
    while (flagSetupSeed == 0x01) {
        timeOutReceiveiSeed = millis();

        while (millis() - timeOutReceiveiSeed < timeOutSeed) {
            receiveSeed();
        }

        if (flagTimeoutSeed == 0x01) {
            disconnectBLE1010();
            funcReset();
        }

        timeOutReceiveiSeed = 0;
        sendSeedOk();
    }

    delay(1000 * timeToWait);
    disconnectBLE1010();
    delay(2000);
    changeName();

    EEPROM.update(setupSeedOk, 0x01);
    EEPROM.update(generalSetupOk, 0x01);

    delay(1000 * timeToWait);
}

//*****************************************************************************************************************
// Receive seed
//*****************************************************************************************************************

void receiveSeed() {
    if (bluetooth.available() > 0) {
        inputBLE1010 = bluetooth.parseInt();

        if (inputBLE1010 != 0) {
            contadorSeed++;

            if (inputBLE1010 == testMotor) {
                // Turn01
                flagStallMotor = 0x01;

                rotateMotor01();
                readINA219();
                stopMotor();

                functionPanic();
                delay(10 * timeToWait);

                rotateMotor02();
                delay(timeToLineUP);
                stopMotor();

                functionPanic();
                delay(2000 * timeToWait);

                // Turn02
                flagStallMotor = 0x01;

                rotateMotor02();
                readINA219();
                stopMotor();

                functionPanic();
                delay(10 * timeToWait);

                rotateMotor01();
                delay(timeToLineUP);
                stopMotor();

                functionPanic();

                contadorSeed = 0;

                SendDataBLE("11");  // SOFT - FI = 360
            }

            if (contadorSeed == 1) {
                inputSeed01 = inputBLE1010;
                flagTimeoutSeed = 0x01;
            } else if (contadorSeed == 2) {
                inputSeed02 = inputBLE1010;
                flagTimeoutSeed = 0x01;
            } else if (contadorSeed == 3) {
                inputSeed03 = inputBLE1010;
                flagTimeoutSeed = 0x01;
            } else if (contadorSeed == 4) {
                inputSeed04 = inputBLE1010;
                flagTimeoutSeed = 0x01;
            } else if (contadorSeed == 5) {
                inputTimeStamp = inputBLE1010;
                timeOutSeed = 0;
                flagTimeoutSeed = 0x00;
                flagSeedOk = 0x01;
            }
        }
    }
}

//*****************************************************************************************************************
// Send seed
//*****************************************************************************************************************

void sendSeedOk() {
    char TempStr[CMDBUFFER_SIZE] = "";
    while (flagSeedOk == 0x01) {
        EEPROM.put(address01Seed01, inputSeed01);
        EEPROM.put(address01Seed02, inputSeed02);
        EEPROM.put(address01Seed03, inputSeed03);
        EEPROM.put(address01Seed04, inputSeed04);
        EEPROM.put(address01MAC, address01MAC);

        sprintf(TempStr, "%lu", inputSeed01);                  // SOFT - FI = 360
        SendDataBLE(TempStr, SEEDDelimData);                   // SOFT - FI = 360  // inputSeed01
        sprintf(TempStr, "%lu", inputSeed02);                  // SOFT - FI = 360
        SendDataBLE(TempStr, SEEDDelimData);                   // SOFT - FI = 360  // inputSeed02
        sprintf(TempStr, "%lu", inputSeed03);                  // SOFT - FI = 360
        SendDataBLE(TempStr, SEEDDelimData);                   // SOFT - FI = 360  // inputSeed03
        sprintf(TempStr, "%lu", inputSeed04);                  // SOFT - FI = 360
        SendDataBLE(TempStr, SEEDDelimData);                   // SOFT - FI = 360  // inputSeed04
        SendDataBLE(numberMacAddress.c_str(), SEEDDelimData);  // SOFT - FI = 360
        sprintf(TempStr, "%d", FI_version);                    // SOFT - FI = 360
        SendDataBLE(TempStr);                                  // SOFT - FI = 360// FI_version
        delay(12);                                             // SOFT - FI = 360// Delay para garantir que o pacote de dados finalize

        flagSetupSeed = 0x00;
        flagSeedOk = 0x00;

        contadorSeed = 0;
    }
}

//*****************************************************************************************************************
// Function to change name
//*****************************************************************************************************************

void changeName(bool lenta) {
    //------------------------------------
    // To set the name CHAVI
    //------------------------------------

    String resp = rotinaWriteBluetooth(String(nameBLE1010).substring(0, 18).c_str(), lenta);
}

//*****************************************************************************************************************
// Function to setup interrupts
//*****************************************************************************************************************

void setupInterrupts() {
    if (flagSelectInterrupt == 0x01) {
        delay(50);
        attachInterrupt(digitalPinToInterrupt(pinPushButton), interruptSetup, FALLING);
    }

    if (flagSelectInterrupt == 0x02) {
        flagInterrupt = NONE;
        attachInterrupt(digitalPinToInterrupt(pinPushButton), pushButtonInterrupt, FALLING);
        attachInterrupt(digitalPinToInterrupt(pinWakeuC), bluetoohInterrupt, RISING);
    }
}

//*****************************************************************************************************************
// End settings
//*****************************************************************************************************************

void loop() {
    if (flagButton == 0x01) {
        functionPBOperate();
    }

    readBLE1010();

    goToSleep();
}

//*****************************************************************************************************************
// Function to push button operate
//*****************************************************************************************************************

void functionPBOperate() {
    if (flagInterrupt == PUSH_BUTTON) {
        readBatteryLevel();

        while (!digitalRead(pinPushButton)) {
            counterResetDance++;
            delay(100 * timeToWait);

            if (counterResetDance == 25 || counterResetDance == 50) {
                interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
            }
        }

        if ((counterResetDance >= 25) && (counterResetDance < 50)) {
            counterResetDance = 0;
            timeToClose = millis();

            // FIXME Substituir por switch case
            if (flagAutomaticClosing == 0x00) {
                timeToCloseTheDoor = 0;
            }
            if (flagAutomaticClosing == 0x01) {
                timeToCloseTheDoor = 3000;
            }
            if (flagAutomaticClosing == 0x02) {
                timeToCloseTheDoor = 5000;
            }
            if (flagAutomaticClosing == 0x03) {
                timeToCloseTheDoor = 10000;
            }
            if (flagAutomaticClosing == 0x04) {
                timeToCloseTheDoor = 15000;
            }
            if (flagAutomaticClosing == 0x05) {
                timeToCloseTheDoor = 30000;
            }

            interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

            while (millis() - timeToClose < timeToCloseTheDoor) {
                flagShowBatteryLevel = 0x00;
            }
        }

        if (counterResetDance >= 50) {
            flagShowBatteryLevel = 0x01;
            flagInterrupt = NONE;
            counterResetDance = 0;

            if (flagLightWarning == 0x00) {
                interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
                delay(300 * timeToWait);
                interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
                delay(300 * timeToWait);
            } else if (flagWarningSound == 0x01) {
                toneDefaultByTime(timeToToneDefault);
            }

            readBatteryLevel();

            if (flagLightWarning != 0x00) {
                showBatteryLevel();
            }
        }

        if (!flagShowBatteryLevel == 0x01) {
            if (flagStateMotor) {
                turnTheMotor01();
            } else {
                turnTheMotor02();
            }
            readBatteryLevel();
            if (flagLightWarning != 0x00) {
                showBatteryLevel();
            }

            flagShowBatteryLevel = 0x00;
            flagInterrupt = NONE;
        }

        flagShowBatteryLevel = 0x00;
        timeToClose = 0;
        counterResetDance = 0;
    }
}

//*****************************************************************************************************************
// Function to read BLE1010
//*****************************************************************************************************************

void readBLE1010() {
    // Serial.printf("%s:%d\n",__FUNCTION__, __LINE__);
    if (bluetooth.available() > 0) {
        timeOutDKCount = millis();

        while (millis() - timeOutDKCount < timeOutGoToSleep) {
            if (bluetooth.available() > 0) {
                receiveRandom = bluetooth.parseInt();
                Serial.printf("receiveRandom: %lu\n", receiveRandom);
                if (receiveRandom != 0) {
                    timeOutDKCount = millis();
                    if (counterToken == 0) {
                        Serial.printf("counterToken: %d\n", counterToken);
                        while (bluetooth.available() > 0) {
                            char bufferApp = bluetooth.read();
                            Serial.printf("Buffer not Empty %s\n", (char) bufferApp);

                            if (bufferApp == 1) {
                                Serial.println("bufferApp == 1. Saindo do while");
                                break;
                            }

                        }
                        delay(20);
                        saltos_1 = send_saltos(receiveRandom, inputSeed01);
                        Serial.printf("saltos_1: %lu\n", saltos_1);

                        delay(20);
                        saltos_2 = send_saltos(receiveRandom, inputSeed02);
                        Serial.printf("saltos_2: %lu\n", saltos_2);

                        delay(20);

                        numberToken01 = getpass_do_lolis(saltos_1, inputSeed01);
                        numberToken02 = getpass_do_lolis(saltos_2, inputSeed02);
                        numberToken03 = getpass_do_lolis(saltos_1, inputSeed03);
                        numberToken04 = getpass_do_lolis(saltos_2, inputSeed04);
                        Serial.printf("numberToken01: %lu\n", numberToken01);
                        Serial.printf("numberToken02: %lu\n", numberToken02);
                        Serial.printf("numberToken03: %lu\n", numberToken03);
                        Serial.printf("numberToken04: %lu\n", numberToken04);

                        counterToken++;
                    } else if (counterToken == 1) {
                        token01 = receiveRandom;
                        Serial.printf("token01: %lu\n", token01);

                        counterToken++;
                    } else if (counterToken == 2) {
                        token02 = receiveRandom;
                        Serial.printf("token02: %lu\n", token02);

                        counterToken++;
                    } else if (counterToken == 3) {
                        token03 = receiveRandom;
                        Serial.printf("token03: %lu\n", token03);

                        counterToken++;
                    } else {
                        counterToken = 0;
                    }
                }

                if ((token01 != 0) && (token02 != 0) && (token03 != 0)) {
                    if ((token01 == numberToken01) && (token02 == numberToken02)) {
                        operateAllOk();
                        token01 = 0;
                        token02 = 0;
                        token03 = 0;
                        token04 = 0;
                        counterToken = 0;

                        timeOutDKCount = millis() - timeOutGoToSleep - 100;
                    }

                    if ((token01 == numberToken03) && (token02 == numberToken04)) {
                        if (token03 == token03SetupDevice) {
                            setupDeviceComplete();
                        } else if (token03 == tokenCalibration) {
                            setupCalibration();
                        } else if (token03 == tokenSetupSeed) {
                            SendDataBLE("11");  // SOFT - FI = 360,
                            delay(100);

                            EEPROM.update(setupSeedOk, 0x00);

                            funcReset();
                        }

                        timeOutDKCount = millis() - timeOutGoToSleep - 100;
                    }
                }
            }
        }

        token01 = 0;
        token02 = 0;
        token03 = 0;
        token04 = 0;
        counterToken = 0;
        timeOutDKCount = 0;

        startTime = millis();
    }
    // Serial.printf("%s:%d\n",__FUNCTION__, __LINE__);
}

//*****************************************************************************************************************
// Function to operate with all ok
//*****************************************************************************************************************

void operateAllOk() {
    if (token03 == 1) {  // token03 é o comando abrir ou fechar
        if (EEPROM.read(calibrationOk) == 0x00) {
            flagStateMotor = 0;  // o calibrationOK = 0x00 é porta sentido horário
        } else if (EEPROM.read(calibrationOk) == 0x01) {
            flagStateMotor = 1;  // o calibrationOK = 0x01 é porta sentido anti horário
        }
    }

    if (token03 == 2) {
        if (EEPROM.read(calibrationOk) == 0x00) {
            flagStateMotor = 1;
        } else if (EEPROM.read(calibrationOk) == 0x01) {
            flagStateMotor = 0;
        }
    }

    readBatteryLevel();
    if (flagStateMotor) {
        //FIXME Alterar para mandar 2000
        bluetooth.println(statusDoorOpen + batteryLevel);  // atual (355)

        turnTheMotor01();
    } else {
        //FIXME Alterar para mandar 1000
        bluetooth.println(statusDoorClose + batteryLevel);  // atual (355)
        turnTheMotor02();
        // Verifica se 120 segundos se passaram desde que goToSleep foi chamado
    }

    if (flagLightWarning != 0x00) {
        showBatteryLevel();
    }
}

//*****************************************************************************************************************
// Function to setup device complete
//*****************************************************************************************************************

void setupDeviceComplete() {  // a parte de configurações da fechdaruda
    char Preferences[32];

    interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03
        Serial.println(__LINE__);

    SendDataBLE("22");  // SOFT - FI = 360

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n' && (command.substring(0, 6) == "chavi:" || command.substring(0, 6) == "CHAVI:")) {
                command.toCharArray(Preferences, sizeof(Preferences));

                EEPROM.update(configurationDevice, Preferences[6] - 0x30);
                EEPROM.update(configurationDevice + 1, Preferences[7] - 0x30);
                EEPROM.update(configurationDevice + 2, Preferences[8] - 0x30);
                EEPROM.update(configurationDevice + 3, Preferences[9] - 0x30);
                EEPROM.update(configurationDevice + 4, Preferences[10] - 0x30);
                EEPROM.update(configurationDevice + 5, Preferences[11] - 0x30);
                EEPROM.update(configurationDevice + 6, Preferences[12] - 0x30);
                EEPROM.update(configurationDevice + 7, Preferences[13] - 0x30);

                flagProximityOpening = EEPROM.read(configurationDevice);
                flagWarningSound = EEPROM.read(configurationDevice + 1);
                flagLightWarning = EEPROM.read(configurationDevice + 2);
                flagLockTurns = EEPROM.read(configurationDevice + 3);
                flagButton = EEPROM.read(configurationDevice + 4);
                flagAutomaticClosing = EEPROM.read(configurationDevice + 5);
                flagDoorUnloockingWarning = EEPROM.read(configurationDevice + 6);
                flagOpenDoorWarning = EEPROM.read(configurationDevice + 7);

                b1 = 0;
                command = "";
            } else if (b1 == '\n' || b1 == '\r') {
                b1 = 0;
                command = "";
            }
        }
        /*
        Abertura por proximidade  = 1 (sim)
        Abertura por proximidade  = 0 (não) DEFAULT

        Aviso sonoro        = 1 (sim) DEFAULT
        Aviso sonoro        = 0 (não)

        Aviso luminoso        = 0 (não)
        Aviso luminoso        = 1 (3s)  DEFAULT
        Aviso luminoso        = 2 (5s)
        Aviso luminoso        = 3 (10s)

        Trancar com 2 voltas    = 1 (sim) DEFAULT
        Trancar com 2 voltas    = 0 (não)

        Botão           = 1 (sim) DEFAULT
        Botão           = 0 (não)

        Fechamento automático   = 0 (não)   DEFAULT
        Fechamento automático   = 1 (15s)
        Fechamento automático   = 2 (30s)
        Fechamento automático   = 3 (40s)
        Fechamento automático   = 4 (50s)

        Aviso de porta destrancada  = 0 (não) DEFAULT
        Aviso de porta destrancada  = 1 (sim)

        Aviso de porta aberta   = 0 (não) DEFAULT
        Aviso de porta aberta   = 1 (sim)

        Exemplo: CHAVI:0111 1000 = 0x78

        */
    }
}

//*****************************************************************************************************************
// Function to go sleep ATMEGA328P
//*****************************************************************************************************************

void goToSleep() {
    if (millis() - startTime < timeOutGoToSleep) {
        return;
    }

    Serial.println(F("Sleeping..."));
    disconnectBLE1010();
    activeBLE1010Connect();
    rotinaWriteBluetooth("AT+PIO80");
}

//*****************************************************************************************************************
// Function to battery level
//*****************************************************************************************************************

void readBatteryLevel() {
    batteryLevelFloat = 0;
    for (int j = 0; j < nSamples; j++) {
        batteryLevelFloat += analogRead(pinBattery01);
    }
    batteryLevelFloat /= nSamples;
    batteryLevel = (batteryLevelFloat * constBatteryLevel);

    // Adicione aqui o polinômio descoberto com regressão linear
    // Exemplo: batteryLevelFloat = a * batteryLevelFloat + b;

    float voltage = batteryLevelFloat * constBatteryLevel;

    // Adapte a lógica de limite de tensão conforme necessário
    if (voltage > maxValueBattery) {
        voltage = maxValueBattery;
    } else if (voltage < minValueBattery) {
        voltage = minValueBattery;
    }

    percBatteryLevel = (((maxScaleBattery - (maxValueBattery - voltage)) * percBattery) / maxScaleBattery);

    if (percBatteryLevel > oldPercBatteryLevel) {
        percBatteryLevel = oldPercBatteryLevel;
    } else {
        oldPercBatteryLevel = percBatteryLevel;
    }
}

//*****************************************************************************************************************
// Function to disconnect BLE1010
//*****************************************************************************************************************

void disconnectBLE1010() {
    rotinaWriteBluetooth(lostConnecBLE1010);
}

//*****************************************************************************************************************
// Function to disconnect BLE1010
//*****************************************************************************************************************

void activeBLE1010Connect() {
    rotinaWriteBluetooth(toStatePIO60);
}

//*****************************************************************************************************************
// Function to token
//*****************************************************************************************************************

unsigned long getpass_do_lolis(unsigned long difference, unsigned long seed) {
    unsigned long b[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    b[0] = 1;
    unsigned int y[4];

    for (uint8_t index = 1; index < 32; index++) {  // Create the bit mask
        b[index] = b[index - 1] << 1;
    }

    for (unsigned int j = 0; j < difference; j++) {
        y[0] = (b[31] & seed) ? 1 : 0;
        y[1] = (b[21] & seed) ? 1 : 0;
        y[2] = (b[1] & seed) ? 1 : 0;
        y[3] = (b[0] & seed) ? 1 : 0;
        seed = seed << 1 | (y[0] ^ y[1] ^ y[2] ^ y[3]);
    }

    return seed;
}

//*****************************************************************************************************************
// Function to turn the motor 01
//*****************************************************************************************************************

void turnTheMotor01() {
    flagStateMotor = 0;
    if (EEPROM.read(calibrationOk) == 0x00) {
        flagSoundBrand = 0x01;
    } else if (EEPROM.read(calibrationOk) == 0x01) {
        flagSoundBrand = 0x02;
    }
    flagStallMotor = 0x01;

    rotateMotor02();
    if (flagLockTurns == 0x01) {
        delay(timeToSoftStart);
    }
    readINA219();
    stopMotor();

    functionPanic();
    delay(10 * timeToWait);

    if (flagLineUpMotor == 0x01) {
        rotateMotor01();
        delay(timeToLineUP);
        stopMotor();

        flagLineUpMotor = 0x00;
    }

    functionPanic();
    if (flagWarningSound == 0x01) {
        soundBrand();
    }
}

//*****************************************************************************************************************
// Function to turn the motor 02
//*****************************************************************************************************************

void turnTheMotor02() {
    flagStateMotor = 1;
    if (EEPROM.read(calibrationOk) == 0x00) {
        flagSoundBrand = 0x02;
    } else if (EEPROM.read(calibrationOk) == 0x01) {
        flagSoundBrand = 0x01;
    }
    flagStallMotor = 0x01;

    rotateMotor01();
    if (flagLockTurns == 0x01) {
        delay(timeToSoftStart);
    }
    readINA219();
    stopMotor();

    functionPanic();
    delay(10 * timeToWait);

    if (flagLineUpMotor == 0x01) {
        rotateMotor02();
        delay(timeToLineUP);
        stopMotor();

        flagLineUpMotor = 0x00;
    }

    functionPanic();
    if (flagWarningSound == 0x01) {
        soundBrand();
    }
}

//*****************************************************************************************************************
// Function to read INA219
//*****************************************************************************************************************

void readINA219() {
    ina219.powerSave(false);

    timeoutStallMotor = millis();

    while (flagStallMotor == 0x01) {
        if ((flagLockTurns == 0x01) &&
            (flagVerifierCalibration == 0x01) &&
            (flagLockTurnsError01 == 0x00) &&
            (flagLockTurnsError02 == 0x00)) {
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 1)) {
                functionDelta();
            } else if ((flagStateMotor == 1) && (flagStatusDoor02 == 0)) {
                functionDelta();
            } else {
                currentInmA = readCurrentInmA();
            }
        } else {
            currentInmA = readCurrentInmA();
        }

        if (counterLockTurns01 == 1) {
            flagLockTurnsError01 = 0x01;
        }
        if (counterLockTurns02 == 1) {
            flagLockTurnsError02 = 0x01;
        }

        if ((flagLockTurns == 0x01) &&
            (flagVerifierCalibration == 0x01) &&
            (flagLockTurnsError01 == 0x00) &&
            (flagLockTurnsError02 == 0x00)) {
            // To 1-1
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 1)) {
                if (deltaD < turnOne3) {
                    counterLockTurns01++;

                    flagLineUpMotor = 0x00;
                    flagStallMotor = 0x00;
                    delay(1500 * timeToWait);
                    stopMotor();
                }

                if (abs(currentInmA) > stallMotor) {
                    flagStallMotor = 0x00;
                }
                if (millis() - timeoutStallMotor > timeoutMotor) {
                    flagStallMotor = 0x00;
                }
            } else if ((flagStateMotor == 1) && (flagStatusDoor02 == 0)) {  // To 0-0
                if (deltaD < turnOne3) {
                    counterLockTurns02++;

                    flagLineUpMotor = 0x00;
                    flagStallMotor = 0x00;
                    delay(1500 * timeToWait);
                    stopMotor();
                }

                if (abs(currentInmA) > stallMotor) {
                    flagStallMotor = 0x00;
                }
                if (millis() - timeoutStallMotor > timeoutMotor) {
                    flagStallMotor = 0x00;
                }
            } else {
                flagLineUpMotor = 0x01;

                if (abs(currentInmA) > stallMotor) {
                    flagStallMotor = 0x00;
                }
                if (millis() - timeoutStallMotor > timeoutMotor) {
                    flagStallMotor = 0x00;
                }
            }
        } else {  // To 0-1
            if ((flagStateMotor == 1) && (flagStatusDoor02 == 1)) {
                counterLockTurns01 = 0;
                flagLockTurnsError01 = 0x00;
            }

            // To 1-0
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 0)) {
                counterLockTurns02 = 0;
                flagLockTurnsError02 = 0x00;
            }

            flagLineUpMotor = 0x01;

            if (abs(currentInmA) > stallMotor) {
                flagStallMotor = 0x00;
            }

            if (millis() - timeoutStallMotor > timeoutMotor) {
                flagStallMotor = 0x00;
            }
        }
    }

    deltaD = 0;
    currentInmA = 0;
    timeoutStallMotor = 0;

    ina219.powerSave(true);
}

//*****************************************************************************************************************
// Function to panic
//*****************************************************************************************************************

void functionPanic() {
    ina219.powerSave(false);

    unsigned long timeToPanic = millis();

    while (millis() - timeToPanic < timeToPanicSegurite) {
        currentInmA = readCurrentInmA();

        if (abs(currentInmA) >= valueToPanic) {
            flagToPanic = 0x01;

            stopMotor();
        } else {
            flagToPanic = 0x00;
            timeToPanic = 0;
        }
    }

    ina219.powerSave(true);

    if (flagToPanic == 0x01) {
        funcReset();
    } else {
        currentInmA = 0;
        timeToPanic = 0;
    }
}

//*****************************************************************************************************************
// Function to calculate derivada
//*****************************************************************************************************************

void functionDelta() {
    float currentInmAOld = currentInmA;
    currentInmA = 0;
    unsigned long timeTime = millis();

    // TODO Por que 2*nSamples?
    // TODO Usar função extraída `readCurrentInmA()`
    for (int i = 0; i < (2 * nSamples); i++) {
        currentInmA = ina219.getCurrent_mA() + currentInmA;
    }
    currentInmA /= nSamples;
    unsigned long timeTime2 = millis();

    deltaD = ((currentInmA - currentInmAOld) / (timeTime2 - timeTime)) * 100;
}

//*****************************************************************************************************************
// Function to setup calibration
//*****************************************************************************************************************

void setupCalibration() {
    delay(50);  // SOFT - Inserido para compatibilizar com iOS para iniciar calibracao
    clearCMDBuffer();

    interfaceFI(ON_SOUND_TONESEED_OFF);  // 0x02

    SendDataBLE("11", CALIBDelimData);  // SOFT - FI = 360

    flagCalibrationFI01 = 0x01;
    flagCalibrationFI02 = 0x01;

    ina219.powerSave(false);

    while (flagCalibrationFI01 == 0x01) {
        while (flagCalibrationFI02 == 0x01) {
            char c;

            if (bluetooth.available() > 0) {
                String cmd = bluetooth.readString();
                cmd.trim();
                cmd.toCharArray(cmdBuffer, 32);
                c = '\n';
                
                // while (bluetooth.available()) {
                    // c = processCharInput(cmdBuffer, lerBt);
                    if (c == '\n') {
                        readCalibration();

                        timeoutStallMotor02 = millis();
                        clearCMDBuffer();
                    }
                // }
            }
        }

        currentInmA = readCurrentInmA();

        if (abs(currentInmA) > stallMotor) {
            if (flagCalibrationFI03 == 0x01) {
                flagCalibrationFI02 = 0x01;
            }
            if (flagCalibrationFI03 == 0x02) {
                flagCalibrationFI02 = 0x00;
                flagCalibrationFI01 = 0x00;
            }

            if (flagTurnMotor == 0x01) {
                flagTurnMotor = 0x02;
            }
            if (flagTurnMotor == 0x03) {
                flagTurnMotor = 0x04;
            }
            turnMotor();
        }

        if (millis() - timeoutStallMotor02 > timeoutMotor) {
            if (flagCalibrationFI03 == 0x01) {
                flagCalibrationFI02 = 0x01;
            }
            if (flagCalibrationFI03 == 0x02) {
                flagCalibrationFI02 = 0x00;
                flagCalibrationFI01 = 0x00;
            }

            if (flagTurnMotor == 0x01) {
                flagTurnMotor = 0x02;
            }
            if (flagTurnMotor == 0x03) {
                flagTurnMotor = 0x04;
            }
            turnMotor();
        }
    }

    timeoutStallMotor03 = millis();

    while (flagStatusDoor == 0x01) {
        flagTurnMotor = 0x01;
        turnMotor();

        currentInmA = readCurrentInmA();

        if (abs(currentInmA) > stallMotor) {
            flagTurnMotor = 0x02;
            turnMotor();
            flagStatusDoor = 0x00;
        }

        if (millis() - timeoutStallMotor03 > timeoutMotor) {
            flagTurnMotor = 0x02;
            turnMotor();
            flagStatusDoor = 0x00;
        }
    }

    ina219.powerSave(true);

    currentInmA = 0;
    timeoutStallMotor02 = 0;
    timeoutStallMotor03 = 0;

    disconnectBLE1010();
    delay(500 * timeToWait);

    flagVerifierCalibration = 0x01;
    EEPROM.update(verifierCalibration, flagVerifierCalibration);

    interfaceFI(ON_SOUND_TONESEED_OFF);  // 0x02
}

//*****************************************************************************************************************
// Function to read calibration
//*****************************************************************************************************************

void readCalibration() {
    Serial.println(cmdBuffer);
    if (strcmp(commandToStartCali, cmdBuffer) == 0) {
        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x01;
        flagTurnMotor = 0x01;

        turnMotor();

        SendDataBLE("11", CALIBDelimData);  // SOFT - FI = 360
    } else if (strcmp(commandToDoorClose, cmdBuffer) == 0) {
        interfaceFI(ON_SOUND_TONEDEFAULT);  // 0x03

        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x02;
        flagTurnMotor = 0x03;

        flagStatusDoor = 0x00;
        flagStatusDoor02 = flagStatusDoor;
        flagStateMotor = 0;
        SendDataBLE("11", CALIBDelimData);  // SOFT - FI = 360
        SendDataBLE("1");                   // SOFT - FI = 360

        EEPROM.update(calibrationOk, flagStatusDoor02);

        turnMotor();
    } else if (strcmp(commandToDoorOpen, cmdBuffer) == 0) {
        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x02;
        flagTurnMotor = 0x03;

        flagStatusDoor = 0x01;
        flagStatusDoor02 = flagStatusDoor;
        flagStateMotor = 1;
        SendDataBLE("11", CALIBDelimData);  // SOFT - FI = 360
        SendDataBLE("2");                   // SOFT - FI = 360

        EEPROM.update(calibrationOk, flagStatusDoor02);

        turnMotor();
    } else {
        Serial.println(__LINE__);
        SendDataBLE("22");  // SOFT - FI = 360
        delay(1000 * timeToWait);
    }
}

//*****************************************************************************************************************
// Turn motor one
//*****************************************************************************************************************

void turnMotor() {
    if (flagTurnMotor == 0x01) {
        rotateMotor01();
    }

    if (flagTurnMotor == 0x02) {
        rotateMotor02();
        delay(500 * timeToWait);
        stopMotor();
    }

    if (flagTurnMotor == 0x03) {
        rotateMotor02();
    }

    if (flagTurnMotor == 0x04) {
        rotateMotor01();
        delay(500 * timeToWait);
        stopMotor();
    }
}

/*
 * Função para exibir estado nos leds.
 * @param flagInterface Estado que será mostrado.
 */
void interfaceFI(Interface flagInterface) {
    // TODO Extrair função para o header `interface.h`.

    if (flagInterface == FADEIN_FADEOUT) {
        turnOffLEDs();
        // FIXME Substituir loops por interrupção por timer
        for (int i = minValuePWM; i <= maxValuePWM; i++) {
            leds[2] = CRGB(0, i, 0);
            FastLED.show();
            delay(2 * timeToWait);
        }

        delay(50 * timeToWait);

        for (int j = maxValuePWM; j >= minValuePWM; j--) {
            leds[2] = CRGB(0, j, 0);
            FastLED.show();
            delay(2 * timeToWait);
        }
    } else if (flagInterface == ON_SOUND_TONESEED_OFF) {
        turnOnLEDs(corOK);
        if (flagWarningSound == 0x01) {
            toneDefaultByTime(timeToToneSeedSetup * timeToWait);
        }
        turnOffLEDs();
    } else if (flagInterface == ON_SOUND_TONEDEFAULT) {
        turnOnLEDs(corOK);
        if (flagWarningSound == 0x01) {
            toneDefaultByTime(timeToToneDefault * timeToWait);
        }
        turnOffLEDs();
    } else if (flagInterface == ON) {
        turnOnLEDs(corOK);
    } else if (flagInterface == OFF) {
        turnOffLEDs();
    } else if (flagInterface == ON_DELAY_OFF) {
        turnOnLEDsDelay(corOK, 500 * timeToWait);
        turnOffLEDs();
    } else if (flagInterface == ON_TONESEED_OFF) {
        turnOnLEDs(corOK);
        toneDefaultByTime(timeToToneSeedSetup * timeToWait);
        turnOffLEDs();
    } else if (flagInterface == ON_TONESEED) {
        turnOnLEDs(corOK);
        toneDefaultByTime(timeToToneSeedSetup * timeToWait);
    }
}

//*****************************************************************************************************************
// Function to sound brand
//*****************************************************************************************************************

void soundBrand() {
    if (flagWarningSound == 0x01) {
        int i;

        if (flagSoundBrand == 0x01) {
            for (i = 0; i < maxNotes - 1; ++i) {
                toneDefaultByTime(timeToTone, sBrandOpen[i]);
            }
            toneDefaultByTime(timeToTone * 2, sBrandOpen[i]);
        } else if (flagSoundBrand == 0x02) {
            for (i = 0; i < maxNotes - 1; ++i) {
                toneDefaultByTime(timeToTone, sBrandClose[i]);
            }
            toneDefaultByTime(timeToTone * 2, sBrandClose[i]);
        }
    }
}

//*****************************************************************************************************************
// Function to show battery level
//*****************************************************************************************************************

void showBatteryLevel() {
    // Ajuste a lógica de exibição conforme necessário
    if ((percBatteryLevel <= 100) && (percBatteryLevel > 30)) {
        turnOnLEDs(corOK);
    } else if ((percBatteryLevel <= 30) && (percBatteryLevel >= 0)) {
        turnOnLEDs(corNOTOK);
    }

    if (flagLightWarning == 0x01) {
        nonBlockingDelay(3 * showTheBatteryLevel);
    }
    if (flagLightWarning == 0x02) {
        nonBlockingDelay(5 * showTheBatteryLevel);
    }
    if (flagLightWarning == 0x03) {
        nonBlockingDelay(10 * showTheBatteryLevel);
    }
    turnOffLEDs();
}

//*****************************************************************************************************************
// Function to deliver the number of jumps (with random generation)
//*

unsigned long send_saltos(unsigned long random_input, unsigned long seed) {
    char TempStr[CMDBUFFER_SIZE] = "";

    randNumber = random(0, 9999);  // FIXME Chamar randomSeed para não gerar sempre os mesmos números aleatórios

    if (VersBLE <= VersaoFW(3)) {
        sprintf(TempStr, "%lu\n\n", randNumber + random_input + seed);
    } else {
        sprintf(TempStr, "%lu\n", randNumber + random_input + seed);
    }
    bluetooth.write(TempStr);

    return randNumber;
}

void turnOnLEDsDelay(CRGB crgb, int atraso) {
    turnOnLEDs(crgb);
    delay(atraso);
}

// Ligar leds GREEN ->
void turnOnLEDs(CRGB crgb) {
    for (int count = 0; count < NUM_LEDS; count++) {
        leds[count] = crgb;
    }
    FastLED.show();
}

void turnOffLEDs() {
    for (int count = 0; count < NUM_LEDS; count++) {
        leds[count] = CRGB(0, 0, 0);
    }
    FastLED.show();
}

void stopMotor() {
    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, LOW);
}

void rotateMotor01() {
    digitalWrite(pinTurn01, HIGH);
    digitalWrite(pinTurn02, LOW);
}

void rotateMotor02() {
    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, HIGH);
}

void toneDefaultByTime(unsigned int duration, unsigned int frequency) {
    tone(pinBuzzer, frequency, duration);
    delay(duration);
    noTone(pinBuzzer);
}

String RespostaBLE(bool lenta) {
    delay(100);
    if (lenta) delay(timeToWaitBLE1010);
    String retorno;
    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;
            if (b1 == '\n') {
                retorno = command;
                b1 = 0;
                command = "";
            }
        }
    }
    return retorno;
}

String rotinaWriteBluetooth(const char *str, bool lenta) {
    bluetooth.write(str);
    String resposta = RespostaBLE(lenta);
    return resposta;
}

void clearCMDBuffer() {
    for (uint8_t Aux = 0; Aux < CMDBUFFER_SIZE; ++Aux) {
        cmdBuffer[Aux] = 0;
    }
}

// SOFT - FI = 360
// SendDataBLE
// params, char* TxData, string de dados a ser enviado,
//         char AddData, byte que deve ser adicionado ao fim da string
// return, none
void SendDataBLE(const char *TxData, char AddData) {
    uint8_t Aux;                       // SOFT - FI = 360 - Variavel auxiliar para calculos e processamento
    char TempTx[CMDBUFFER_SIZE] = "";  // SOFT - FI = 360 - Array Temporario para armazenar string e manipular bytes de controle

    strcpy(TempTx, TxData);        // SOFT - FI = 360 - Copia A string TxData para TempTx
    Aux = strlen(TempTx);          // SOFT - FI = 360 - Calcula tamanho da string TempTx e guarda em Aux
    TempTx[Aux++] = ENDWriteData;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de ENDWriteData
    switch (VersBLE) {             // SOFT - FI = 360 - Verifica qual a versao do BLE para adicionar o ultimo byte corretamente e compatibilizar com o App
        case VersaoFW(0):
        case VersaoFW(1):
        case VersaoFW(2):
        case VersaoFW(3):
            TempTx[Aux++] = ENDFrameVer03;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de ENDFrameVer03
            break;

        case VersaoFW(4):
            if (AddData) {                // SOFT - FI = 360 - AddData!= 0 deve adicionar um byte no final da string TempTx, necessario para compatibilizar processos de SETUP e CALIB do app
                TempTx[Aux++] = AddData;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de AddData
            }
            TempTx[Aux++] = ENDFrameVer04;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de EndFrameVer04
            break;

        default:
            if (AddData) {                // SOFT - FI = 360 - AddData!= 0 deve adicionar um byte no final da string TempTx, necessario para compatibilizar processos de SETUP e CALIB do app
                TempTx[Aux++] = AddData;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de AddData
            }
            TempTx[Aux++] = ENDFrameVer04;  // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte de EndFrameVer04
            break;
    }
    TempTx[Aux] = '\0';       // SOFT - FI = 360 - Carrega na posicao Aux do aray TempTx o byte terminador de string
    bluetooth.print(TempTx);  // SOFT - FI = 360 - Envia o dado para a serial do BLE
}

// Função de atraso não bloqueante
void nonBlockingDelay(unsigned long duration) {
    unsigned long startTime = millis();
    while (millis() - startTime < duration) {
        // Aguarda o tempo sem bloquear outras operações
        // Pode ser usado para executar outras tarefas enquanto aguarda
    }
}

/*
 * Retorna o valor médio da corrente medida no sensor INA219.
 * @return corrente em miliamperes.
 */
float readCurrentInmA() {
    float current = 0.0f;
    for (int i = 0; i < nSamples; i++) {
        current += ina219.getCurrent_mA();
    }
    return current / nSamples;
}