/*
  FI 3.4 teste

  Author: Christian Piltz Araújo
  Date: 17/01/2023

  Description: teste de retorno no fim de curso variável
*/
/*

  FI Final - Version Final Git - v8.1

  Author: João Vitor Galdino Souto Alvares
  Date: 04/03/2021

  Description: Versão final da FI.

        -> Arduino

          This program is utilizing 28242 (91%) bytes of the memory FLASH
          The maximum is of 32256 (32KB) bytes of memory FLASH

          This program is utilizing 1175 (57%) bytes of the memory RAM
          The maximum is of 2048 (2KB) bytes of memory RAM

*/

//*****************************************************************************************************************
// Library(ies)
//*****************************************************************************************************************

#include <Adafruit_INA219.h>  // Library to INA219
#include <EEPROM.h>           // Library to read/write EEPROM
#include <TimeLib.h>          // Library to get/set the RTC and the timestamp
#include <Wire.h>
#include <avr/sleep.h>  // Library to sleep
#include <avr/wdt.h>    // Library to watchdog timer
#include <stdio.h>
#include <stdlib.h>  // Library to calculate basics

#include "LowPower.h"  // Library to low power device
#include "RTClib.h"
#include "SoftwareSerial.h"  // Library to communicate BLE1010 with ATMEGA328P
#include "version.h"
#include "SerialNumber.h"
#include "utils.h"

//*****************************************************************************************************************
// Contant(s)
//*****************************************************************************************************************

// Hardware mapping
#define pinRxBLE1010 4
#define pinTxBLE1010 5
#define pinToBuzzer 6
#define pinToLED01 7
#define pinToLED02 8
#define pinToLED03 9
// To motor
#define pinTurn01 10
#define pinTurn02 11
// To battery level
#define pinToBattery01 A1  // Bateria
#define pinToBattery02 A0  // 5V
#define pinSDA A4
#define pinSCL A5
// To wakeup uC
#define pinToPushButton 2
#define pinWakeuC 3
// To Hall Sensor
#define pinToVCCHALLSensor01 A2
#define pinToVCCHALLSensor02 A7
#define pinToHallSensor01 A3
#define pinToHallSensor02 A6

// Macro(s) generic(s)
// To debug
// #define DEBUG
// #define DEBUG_VAI_DE_CHAVI

// To serial
#define SERIAL_BAUD 9600
#define CMDBUFFER_SIZE 32

// To time
#define timeToWait 1
#define timeToWaitBLE1010 500
#define timeToRotation 6000

// #define timeToLineUP        500 // original aqui
#define timeToLineUP 1000  // teste aqui

// #define timeOutGoToSleep      5000
#define timeToResetDanceWait 15000
#define timeToSoftStart 1500

// To commands in the BLE101  0
// To receive BLE1010
#define testBLE1010 "AT\r"  // Command AT to test
#define desactDefPasBLE1010 "AT+TYPE0\r"
#define modeRCBLE1010 "AT+MODE2\r"
#define roleSlaveBLE1010 "AT+ROLE0\r"
#define baud9600BLE1010 "AT+BAUD2\r"
#define actNotiBLE1010 "AT+NOTI1\r"
#define preConnBLE1010 "AT+BEFC000\r"
#define posConnBLE1010 "AT+AFTC008\r"
#define toStatePIO60 "AT+PIO60\r"
#define resetBLE1010 "AT+RESET\r"
#define lostConnecBLE1010 "AT+DROP\r"
// To ask BLE1010
#define askMACBLE1010 "AT+ADDR?\r"  // Command AT to ask the MAC adress

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

// To PWM
#define minValuePWM 0
#define maxValuePWM 255

// To sound brand
#define maxNotes 3
#define timeToTone 1000

// To battery level
#define constBatteryLevel 4.2 / 1024
#define maxValueBattery 4.1
#define minValueBattery 3.2
#define percBattery 100
#define showTheBatteryLevel 1000

// To INA219
#define INA_Address 0x45
#define nSamples 100
#define stallMotor 300
#define timeoutMotor 10000
#define turnOne1 75
#define turnOne2 110
#define turnOne3 -10

// To setup deveice
#define token03SetupDevice 150993
#define tokenSetupSeed 140197
#define tokenCalibration 190720
#define timeSetupComplete 5000
#define testMotor 1509

// To calibration
#define commandToStartCali "CALIBRACAO-FI"
#define commandToDoorOpen "PORTA-ABERTA"
#define commandToDoorClose "PORTA-FECHADA"

// To panic
#define valueToPanic 5
#define timeToPanicSegurite 5000

//*****************************************************************************************************************
// Prototype of the function(s)
//*****************************************************************************************************************

// To setup
void setupSerials();
void setupPins();
void setupInterrupts();
void setupBLE01();
void setupADC();
void setupEEPROM();
void setupI2C();
void seedSetup();
void receiveSeed();
void sendSeedOk();
void changeName();
void interrupt01();
void interrupt02();
char processCharInput(char* cmdBuffer, const char c);

// To operate
void functionPBOperate();
void readBLE1010();
void operateAllOk();
void setupDeviceComplete();
void readCalibration();
void turnMotor();
void goToSleep();
void readBatteryLevel();
void disconnectBLE1010();
void activeBLE1010Connect();
unsigned long getpass_do_lolis(unsigned long difference, unsigned long seed);
void readINA219();
void interfaceFI();
void turnTheMotor01();
void turnTheMotor02();
void soundBrand();
void showBatteryLevel();
void functionPanic();
void functionDelta();
unsigned long send_saltos(unsigned long random_input, unsigned long seed);

//*****************************************************************************************************************
// Object(s)
//*****************************************************************************************************************

// To BLE1010
SoftwareSerial bluetooth(pinTxBLE1010, pinRxBLE1010);  // TX, RX (Bluetooth)

// To RTC
RTC_DS1307 rtc;
// Time timeT;

// To INA219
Adafruit_INA219 ina219(INA_Address);

//*****************************************************************************************************************
// Global variable(s)
//*****************************************************************************************************************

// Variables to setup BLE1010
String command = "";
String numberMacAddress = "";
String newNameChavi = "";
String answerConnBLE1010 = "OK+CONN";
String answerDiscBLE1010 = "OK+LOST";
char b1;
constexpr unsigned long finalToken = 0;
constexpr unsigned long finalPin = 0;
char numberMacTokenChar[10];
char numberMacPinChar[10];
constexpr long inputPin = 0;
// To FSM and pseudo-random generator
constexpr unsigned long macAddressHexDevic = 0;   // This variable will be the last 8 number hex of the MAC in the  bluetooth
constexpr unsigned long macAddressHexDevic2 = 0;  // This variable will be the last 8 number hex of the MAC in the  bluetooth

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
char flagVerifierSeed = 0x00;
char flagReadBLE1010 = 0x00;
char flagSeedSetup = 0x00;
unsigned long numberToken01;
unsigned long numberToken02;
unsigned long numberToken03;
unsigned long numberToken04;
unsigned long token01Pass;
unsigned long token02Pass;
unsigned long token01Future;
unsigned long token02Future;
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

// To RTC
unsigned long difference;
unsigned long difference2;
unsigned long difference3;
unsigned long valueTimestamp = 0;
unsigned long valueTimestamp2 = 0;
unsigned long pointZero = 1568901600;  // 19 de setembro de 2019 às 14h00
unsigned long interval = 900;
unsigned long timeOutGoToSleep = 15000;

// To RTC
// To converte timestamp
struct unix {
    long get(int y, int m = 0, int d = 0, int h = 0, int i = 0, int s = 0) {
        setTime(h, i, s, d, m, y);
        // adjustTime(+10800);                         // +3 hour to Brazil
        return now();
    }

} unix;

char diasDaSemana[7][12] = {"Domingo", "Segunda", "Terca", "Quarta", "Quinta", "Sexta", "Sabado"};  // Dias da semana

// To sound brand
int sBrandOpen[] = {262, 392, 330};
int sBrandClose[] = {349, 330, 262};
char flagSoundBrand = 0x00;
int toneDefault = 1000;
int timeToToneSeedSetup = 3000;
int timeToToneDefault = 300;

// To Interrupt
char flagInterrupt = 0x00;
char flagInterfaceFI = 0x00;
char flagSelectInterrupt = 0x00;

// To motor
int flagStateMotor = 0;

// To battery
float batteryLevelFloat = 0;
float batteryLevel5VFloat = 0;
float batteryLevel = 0;
float batteryLevel5V = 0;
// float differenceBatteryLevel     = 0;
float percBatteryLevel = 0;
float oldPercBatteryLevel = 100;
float percBatteryLevel5V = 0;
int statusDoorOpen = 1000;
int statusDoorClose = 2000;
char flagShowBatteryLevel = 0x00;
float maxScaleBattery = maxValueBattery - minValueBattery;

// To time
unsigned long timeOutDKCount = 0;
char flagPressButton = 0x00;

// To INA219
float shuntVoltage = 0;
float busVoltage = 0;
float currentInmA = 0;
float currentInmAOld = 0;
float deltaD = 0;
float analogBattery = 0;
float loadVoltage = 0;
unsigned long timeoutStallMotor = 0;
unsigned long timeoutStallMotor02 = 0;
unsigned long timeoutStallMotor03 = 0;
unsigned long timeTime = 0;
unsigned long timeTime2 = 0;
char flagStallMotor = 0x00;

// To reset
unsigned long timeToResetDance = 0;
char flagSetupDevice = 0x00;
int counterResetDance = 0;

// To setup device complete
unsigned long timeToSetupDeviceComplete = 0;
String receiveData = "";
String receiveData02 = "";
String bit7 = "";
String bit6 = "";
String bit5 = "";
String bit4 = "";
String bit3 = "";
String bit2 = "";
String bit1 = "";
String bit0 = "";
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

// To panic
unsigned long timeToPanic = 0;
char flagToPanic = 0x00;

// To close only
unsigned int timeToCloseTheDoor;
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

//*****************************************************************************************************************
// Interrupt 00
//*****************************************************************************************************************

void interruptSetup() {
    flagPressButton = 0x01;

    resetDance();

}  // end interruptSetup

//*****************************************************************************************************************
// Function reset dance
//*****************************************************************************************************************

void resetDance() {
    if (flagPressButton == 0x01) {
        flagInterfaceFI = 0x04;
        interfaceFI();

        while (!digitalRead(pinToPushButton)) {
            counterResetDance++;

            if (counterResetDance == RESET_INTERVAL) {
                counterResetDance = 0;

                flagInterfaceFI = 0x07;
                interfaceFI();

                flagInterfaceFI = 0x02;
                interfaceFI();

                EEPROM.update(setupSeedOk, 0);

                funcReset();

            }  // end if

            delayInterrupt(1000);

        }  // end while

        counterResetDance = 0;
        flagPressButton = 0x00;

    }  // end while

}  // end resetDance

//*****************************************************************************************************************
// Interrupt 00
//*****************************************************************************************************************

void interrupt01() {
    flagInterrupt = 0x01;

}  // end interrupt01

//*****************************************************************************************************************
// Interrupt 01
//*****************************************************************************************************************

void interrupt02() {
    flagInterrupt = 0x02;

}  // end interrupt02

//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
// Initial settings
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************

void setup() {
    /*

    while(1)
    {


      for (int i = 0; i < EEPROM.length(); i++) EEPROM.update(i, 0);

      while(1){}

    } // end while

    */

    if (EEPROM.read(setupSeedOk) != 0x01) {
        setupSerials();
        setupPins();
        setupEEPROM();
        flagInterfaceFI = 0x07;
        interfaceFI();
        setupBLE01();
        interfaceFI();
        setupI2C();
        flagInterfaceFI = 0x08;
        interfaceFI();
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

        interfaceFI();

        delay(500 * timeToWait);
        funcReset();

    }  // end if

    if ((EEPROM.read(setupSeedOk) == 0x01)) {
        setupSerials();
        setupPins();
        setupEEPROM();
        flagInterfaceFI = 0x03;
        interfaceFI();
        flagInterfaceFI = 0x01;
        flagSelectInterrupt = 0x01;
        setupInterrupts();
        interfaceFI();
        setupADC();
        interfaceFI();
        setupI2C();
        interfaceFI();
        flagSelectInterrupt = 0x02;
        setupInterrupts();
        interfaceFI();

    }  // end else

    changeName();
}  // end setup

//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
// Function to setup production and operate - START
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************

//*****************************************************************************************************************
// Process char input
//*****************************************************************************************************************

char processCharInput(char* cmdBuffer, const char c) {
    // Store the character in the input buffer
    // Ignore control characters and special ascii characters
    if (c >= 32 && c <= 126) {
        if (strlen(cmdBuffer) < CMDBUFFER_SIZE)
            strncat(cmdBuffer, &c, 1);  // Add it to the buffer
        else
            return '\n';

    }  // end if

    // Backspace
    else if ((c == 8 || c == 127) && cmdBuffer[0] != 0)
        cmdBuffer[strlen(cmdBuffer) - 1] = 0;

    return c;

}  // end processCharInput

//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
// Function to setup production - END
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************

//*****************************************************************************************************************
// Function to setup serials
//*****************************************************************************************************************

void setupSerials() {
    Serial.begin(SERIAL_BAUD);     // Monitor Serial Arduino
    bluetooth.begin(SERIAL_BAUD);  // Terminal BLE

}  // end setupSerials

//*****************************************************************************************************************
// Function to setup pins
//*****************************************************************************************************************

void setupPins() {
    pinMode(pinToLED01, OUTPUT);
    pinMode(pinToLED02, OUTPUT);
    pinMode(pinToLED03, OUTPUT);
    pinMode(pinToBuzzer, OUTPUT);
    pinMode(pinTurn01, OUTPUT);
    pinMode(pinTurn02, OUTPUT);

    pinMode(pinToVCCHALLSensor01, OUTPUT);
    pinMode(pinToVCCHALLSensor02, OUTPUT);
    pinMode(pinToHallSensor01, INPUT);
    pinMode(pinToHallSensor02, INPUT);

    digitalWrite(pinToLED01, LOW);
    digitalWrite(pinToLED02, LOW);
    digitalWrite(pinToLED03, LOW);
    digitalWrite(pinToBuzzer, LOW);
    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, LOW);

}  // end setupPins

//*****************************************************************************************************************
// Function to setup EEPROM
//*****************************************************************************************************************

void setupEEPROM() {
    if (EEPROM.read(setupSeedOk) != 0x01) {
        flagSetupSeed = 0x01;
        flagInterfaceFI = 0x01;

        // setupConfigurationDevice();

    }  // end if

    else {
        flagSetupSeed = 0x00;
        flagVerifierSeed = 0x00;

        setupConfigurationDevice();

        flagSeedSetup = EEPROM.read(setupSeedOk);
        flagReadBLE1010 = EEPROM.read(generalSetupOk);
        inputSeed01 = EEPROM.get(address01Seed01, inputSeed01);
        inputSeed02 = EEPROM.get(address01Seed02, inputSeed02);
        inputSeed03 = EEPROM.get(address01Seed03, inputSeed03);
        inputSeed04 = EEPROM.get(address01Seed04, inputSeed04);

    }  // end else
}  // end setupEEPROM

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

}  // end setupConfigurationDevice

//*****************************************************************************************************************
// Function to setup serial BLE 01
//*****************************************************************************************************************

void setupBLE01() {
    flagInterfaceFI = 0x01;

    interfaceFI();

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To test commands AT BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    bluetooth.write(testBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To desactived password BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(desactDefPasBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To mode receive data BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(modeRCBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To mode role slave BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(roleSlaveBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To baud rate 9600 BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(baud9600BLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To active the notify BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(actNotiBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To pre connection BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(preConnBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To pos connection BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(posConnBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To state after setup PIO6 connection BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(toStatePIO60);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To set the name BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write("AT+NAMECHAVIFI\r");

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // Ask the value of the MAC Address
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(askMACBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                numberMacAddress = command;
                numberMacAddress = command.substring(9);

                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To reset BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(resetBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To drop BLE1010
    //------------------------------------
    //------------------------------------
    //------------------------------------

    interfaceFI();

    bluetooth.write(lostConnecBLE1010);

    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

    flagInterfaceFI = 0x04;

    interfaceFI();

}  // end setupBLE01

//*****************************************************************************************************************
// Function to setup ADC
//*****************************************************************************************************************

void setupADC() {
    pinMode(pinToBattery01, INPUT);  // Setup pin to battery level of the battery
    pinMode(pinToBattery02, INPUT);  // Setup pin to battery level of the 5V

}  // end setupADC

//*****************************************************************************************************************
// Function to setup RTC
//*****************************************************************************************************************

void setupI2C() {
    ina219.begin();
    rtc.begin();

}  // end setupI2C

//*****************************************************************************************************************
// Function to seed setup
//*****************************************************************************************************************

void seedSetup() {
    flagInterfaceFI = 0x04;
    interfaceFI();

    while (flagSetupSeed == 0x01) {
        timeOutReceiveiSeed = millis();

        while (millis() - timeOutReceiveiSeed < timeOutSeed) {
            receiveSeed();

        }  // end while

        if (flagTimeoutSeed == 0x01) {
            disconnectBLE1010();
            funcReset();

        }  // end if

        timeOutReceiveiSeed = 0;
        sendSeedOk();

    }  // end if

    delay(1000 * timeToWait);
    disconnectBLE1010();
    changeName();

    flagInterfaceFI = 0x05;

    EEPROM.update(setupSeedOk, 1);
    EEPROM.update(generalSetupOk, 1);

    delay(1000 * timeToWait);

}  // end seedSetup

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

                digitalWrite(pinTurn01, HIGH);
                digitalWrite(pinTurn02, LOW);
                readINA219();
                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, LOW);

                functionPanic();
                delay(10 * timeToWait);

                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, HIGH);
                delay(timeToLineUP);
                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, LOW);
                functionPanic();

                delay(2000 * timeToWait);

                // Turn02
                flagStallMotor = 0x01;

                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, HIGH);
                readINA219();
                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, LOW);

                functionPanic();
                delay(10 * timeToWait);

                digitalWrite(pinTurn01, HIGH);
                digitalWrite(pinTurn02, LOW);
                delay(timeToLineUP);
                digitalWrite(pinTurn01, LOW);
                digitalWrite(pinTurn02, LOW);
                functionPanic();

                contadorSeed = 0;

                bluetooth.println(11);

            }  // end else if

            if (contadorSeed == 1) {
                inputSeed01 = inputBLE1010;
                flagTimeoutSeed = 0x01;

            }  // end else if

            else if (contadorSeed == 2) {
                inputSeed02 = inputBLE1010;
                flagTimeoutSeed = 0x01;

            }  // end else if

            else if (contadorSeed == 3) {
                inputSeed03 = inputBLE1010;
                flagTimeoutSeed = 0x01;

            }  // end else if

            else if (contadorSeed == 4) {
                inputSeed04 = inputBLE1010;
                flagTimeoutSeed = 0x01;

            }  // end else if

            else if (contadorSeed == 5) {
                inputTimeStamp = inputBLE1010;
                timeOutSeed = 0;
                flagTimeoutSeed = 0x00;
                flagSeedOk = 0x01;

            }  // end else if

        }  // end if

    }  // end if

}  // end receiveSeed

//*****************************************************************************************************************
// Send seed
//*****************************************************************************************************************

void sendSeedOk() {
    while (flagSeedOk == 0x01) {
        time_t t = inputTimeStamp;
        setTime(t);

        rtc.adjust(DateTime(year(), month(), day(), hour(), minute(), second()));

        EEPROM.put(address01Seed01, inputSeed01);
        EEPROM.put(address01Seed02, inputSeed02);
        EEPROM.put(address01Seed03, inputSeed03);
        EEPROM.put(address01Seed04, inputSeed04);
        EEPROM.put(address01MAC, address01MAC);

        bluetooth.println(inputSeed01);
        bluetooth.println(inputSeed02);
        bluetooth.println(inputSeed03);
        bluetooth.println(inputSeed04);
        bluetooth.println(numberMacAddress);
        bluetooth.println(FI_version);

        flagSetupSeed = 0x00;
        flagSeedOk = 0x00;
        flagVerifierSeed = 0x01;
        contadorSeed = 0;

    }  // end if

}  // end sendSeedOk

//*****************************************************************************************************************
// Function to change name
//*****************************************************************************************************************

void changeName() {
    //------------------------------------
    //------------------------------------
    //------------------------------------
    // To set the name CHAVI
    //------------------------------------
    //------------------------------------
    //------------------------------------
    bluetooth.println(nameBLE1010);

    delay(2 * timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

}  // end changeName

//*****************************************************************************************************************
// Function to setup interrupts
//*****************************************************************************************************************

void setupInterrupts() {
    if (flagSelectInterrupt == 0x01) attachInterrupt(digitalPinToInterrupt(pinToPushButton), interruptSetup, FALLING);

    if (flagSelectInterrupt == 0x02) {
        attachInterrupt(digitalPinToInterrupt(pinToPushButton), interrupt01, FALLING);
        attachInterrupt(digitalPinToInterrupt(pinWakeuC), interrupt02, RISING);

        flagInterfaceFI = 0x03;

        interfaceFI();

        flagInterfaceFI = 0x05;

    }  // end

}  // end setupInterrupts

//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
// End settings
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************
//*****************************************************************************************************************

void loop() {
    if (flagButton == 0x01) functionPBOperate();

    readBLE1010();

    goToSleep();

}  // end loop

//*****************************************************************************************************************
// Function to push button operate
//*****************************************************************************************************************

void functionPBOperate() {
    if (flagInterrupt == 0x01) {
        readBatteryLevel();

        while (!digitalRead(pinToPushButton)) {
            counterResetDance++;

            Serial.println("VAI DE CHAVI / VAI DE CHAVI /  VAI DE CHAVI ");
            Serial.print("VAI DE CHAVI / VAI DE CHAVI /  VAI DE CHAVI ");
            Serial.println();
            delay(100 * timeToWait);

            if (counterResetDance == 25) {
                flagInterfaceFI = 0x03;
                interfaceFI();

            }  // end if

            else if (counterResetDance == 50) {
                flagInterfaceFI = 0x03;
                interfaceFI();

            }  // end if

        }  // end while

        if ((counterResetDance >= 25) && (counterResetDance < 50)) {
            counterResetDance = 0;
            timeToClose = millis();

            if (flagAutomaticClosing == 0x00) timeToCloseTheDoor = 0;
            if (flagAutomaticClosing == 0x01) timeToCloseTheDoor = 3000;
            if (flagAutomaticClosing == 0x02) timeToCloseTheDoor = 5000;
            if (flagAutomaticClosing == 0x03) timeToCloseTheDoor = 10000;
            if (flagAutomaticClosing == 0x04) timeToCloseTheDoor = 15000;
            if (flagAutomaticClosing == 0x05) timeToCloseTheDoor = 30000;

            flagInterfaceFI = 0x03;
            interfaceFI();

            while (millis() - timeToClose < timeToCloseTheDoor) {
                flagShowBatteryLevel = 0x00;

            }  // end while

        }  // end if

        if (counterResetDance >= 50) {
            flagShowBatteryLevel = 0x01;
            flagInterrupt = 0x00;
            counterResetDance = 0;

            if (flagLightWarning == 0x00) {
                flagInterfaceFI = 0x03;
                interfaceFI();
                delay(300 * timeToWait);
                flagInterfaceFI = 0x03;
                interfaceFI();
                delay(300 * timeToWait);

            }  // end if

            else if (flagWarningSound == 0x01) {
                tone(pinToBuzzer, toneDefault, timeToToneDefault);
                delay(timeToToneDefault);
                noTone(pinToBuzzer);

            }  // end if

            readBatteryLevel();

            if (flagLightWarning != 0x00) {
                showBatteryLevel();
            }

        }  // end if

        if (!flagShowBatteryLevel == 0x01) {
            if (flagStateMotor)
                turnTheMotor01();
            else
                turnTheMotor02();

            readBatteryLevel();
            if (flagLightWarning != 0x00) {
                showBatteryLevel();
            }

            flagShowBatteryLevel = 0x00;
            flagInterrupt = 0x00;

        }  // end if

        flagShowBatteryLevel = 0x00;
        timeToClose = 0;
        counterResetDance = 0;

    }  // end if

}  // end functionPBOperate

//*****************************************************************************************************************
// Function to read BLE1010
//*****************************************************************************************************************

void readBLE1010() {
    if (flagInterrupt == 0x02) {
        timeOutDKCount = millis();

        while (millis() - timeOutDKCount < timeOutGoToSleep) {
            if (bluetooth.available() > 0) {
                receiveRandom = bluetooth.parseInt();

                if (receiveRandom != 0) {
                    timeOutDKCount = millis();
                    if (counterToken == 0) {
                        while (bluetooth.available() > 0) {
                            Serial.printf("Buffer %d\n", bluetooth.available());
                            Serial.println("Aguardando app");
                            long bufferApp = bluetooth.parseInt();
                            Serial.printf("Buffer lido %d\n", bufferApp);

                            if (bufferApp == 1) {
                                Serial.println("App conectada");
                                break;
                            }

                            Serial.println("Fim do loop. recomecando");
                        }

                        Serial.println("saiu do loop");
                        delay(3500);

                        delay(20);
                        saltos_1 = send_saltos(receiveRandom, inputSeed01);
                        // bluetooth.println(receiveRandom+5);
                        delay(20);
                        saltos_2 = send_saltos(receiveRandom, inputSeed02);
                        // bluetooth.println(receiveRandom+10);
                        delay(20);

                        numberToken01 = getpass_do_lolis(saltos_1, inputSeed01);
                        numberToken02 = getpass_do_lolis(saltos_2, inputSeed02);
                        numberToken03 = getpass_do_lolis(saltos_1, inputSeed03);
                        numberToken04 = getpass_do_lolis(saltos_2, inputSeed04);

                        //        Serial.print("Aleatório gerado: ");
                        //        Serial.println(receiveRandom);
                        //
                        //        //Serial.print("Salto 1: ");
                        //        Serial.println(saltos_1);
                        //
                        //        Serial.print("Salto 2: ");
                        //        Serial.println(saltos_2);
                        //
                        //        Serial.print("Token 1: ");
                        //        Serial.println(numberToken01);
                        //
                        //        Serial.print("Token 2: ");
                        //        Serial.println(numberToken02);
                        //
                        //        Serial.print("Token 3: ");
                        //        Serial.println(numberToken03);
                        //
                        //        Serial.print("Token 4: ");
                        //        Serial.println(numberToken04);

                        counterToken++;

                    } else if (counterToken == 1) {
                        token01 = receiveRandom;

                        //        Serial.print("Token 1 recebido: ");
                        //        Serial.println(token01);

                        counterToken++;
                    }

                    else if (counterToken == 2) {
                        token02 = receiveRandom;

                        //        Serial.print("Token 2 recebido: ");
                        //        Serial.println(token02);

                        counterToken++;
                    }

                    else if (counterToken == 3) {
                        token03 = receiveRandom;

                        //        Serial.print("Comando recebido: ");
                        //        Serial.println(token03);

                        counterToken++;
                    }

                    else
                        counterToken = 0;
                }

                if ((token01 != 0) && (token02 != 0) && (token03 != 0)) {
                    if ((token01 == numberToken01) && (token02 == numberToken02))

                    {
                        operateAllOk();
                        token01 = 0;
                        token02 = 0;
                        token03 = 0;
                        token04 = 0;
                        counterToken = 0;

                        timeOutDKCount = millis() - timeOutGoToSleep - 100;
                    }

                    if ((token01 == numberToken03) && (token02 == numberToken04)) {
                        if (token03 == token03SetupDevice) setupDeviceComplete();

                        // else if(token03   == tokenSetupSeed)    setupCalibration();
                        else if (token03 == tokenCalibration)
                            setupCalibration();

                        // else if(token03   == tokenCalibration)
                        else if (token03 == tokenSetupSeed) {
                            bluetooth.println(11);
                            delay(100);

                            EEPROM.update(setupSeedOk, 0);

                            funcReset();

                        }  // end else if

                        timeOutDKCount = millis() - timeOutGoToSleep - 100;
                    }
                }

                // bluetooth.println(receiveRandom+5);

                /*if(receiveToken != 0)
                {

                  counterToken++;

                  if(counterToken == 1)
                  {

                    token01 = receiveToken;

                  } // end

                  else if(counterToken == 2)
                  {

                    token02 = receiveToken;

                  } // end else if

                  else if(counterToken == 3)
                  {

                    token03     = receiveToken;
                    inputTimeStamp  = token03;
                    valueTimestamp2 = inputTimeStamp;

                  } // end else if

                  else if(counterToken == 4)
                  {

                    token04     = receiveToken;
                    counterToken  = 0;

                  } // end else if

                } // end if */

            }  // end if

            /*
            if((token01 != 0) && (token02 != 0) && (token03 != 0) && (token04 != 0))
            {

              upDateToken();

              if((token01   == numberToken01)   && (token02 == numberToken02))    operateAllOk();

              else if((token01 == numberToken03)  && (token02 == numberToken04))
              {

                if(token03      == token03SetupDevice)  setupDeviceComplete();

                //else if(token03   == tokenSetupSeed)    setupCalibration();
                else if(token03   == tokenCalibration)  setupCalibration();

                //else if(token03   == tokenCalibration)
                else if(token03   == tokenSetupSeed)
                {

                  bluetooth.println(11);
                  delay(100);

                  EEPROM.update(setupSeedOk, 0);

                  funcReset();

                } // end else if

              } // end else if

              else
              {

                tokenPassAndFuture();

                if(((token01 == token01Pass) &&
                  (token02 == token02Pass)) ||
                  ((token01 == token01Future) &&
                  (token02 == token02Future)))  operateAllOk();

              } //

              timeOutGoToSleep = 0;

              token01     = 0;
              token02     = 0;
              token03     = 0;
              token04     = 0;

            } // end if */

        }  // end while

        token01 = 0;
        token02 = 0;
        token03 = 0;
        token04 = 0;
        counterToken = 0;
        timeOutDKCount = 0;

    }  // end if

}  // end readBLE1010

//*****************************************************************************************************************
// Function to operate with all ok
//*****************************************************************************************************************

void operateAllOk() {
    // time_t t = inputTimeStamp;
    // setTime(t);

    // rtc.adjust(DateTime(year(), month(), day(), hour(), minute(), second()));

    if (token03 == 1)  // token03 é o comando abrir ou fechar
    {
        if (EEPROM.read(calibrationOk) == 0x00)
            flagStateMotor = 0;  // o calibrationOK = 0x00 é porta sentido horário
        else if (EEPROM.read(calibrationOk) == 0x01)
            flagStateMotor = 1;  // o calibrationOK = 0x01 é porta sentido anti horário

    }  // end if

    if (token03 == 2) {
        if (EEPROM.read(calibrationOk) == 0x00)
            flagStateMotor = 1;
        else if (EEPROM.read(calibrationOk) == 0x01)
            flagStateMotor = 0;

    }  // end if

    readBatteryLevel();

    if (flagStateMotor) {  //
        bluetooth.println(statusDoorOpen + batteryLevel);
        Serial.println("Girando o motor 1");
        // delay(15000);
        turnTheMotor01();
        Serial.println("Girou o motor 1");
    }  // end if
    else {
        bluetooth.println(statusDoorClose + batteryLevel);

        Serial.println("Girando o motor 2");
        turnTheMotor02();
        // delay(15000);
        Serial.println("Girou o motor 2");
    }  // end else

    if (flagLightWarning != 0x00) {
        showBatteryLevel();
    }

}  // end operateAllOk

//*****************************************************************************************************************
// Function to setup device complete
//*****************************************************************************************************************

void setupDeviceComplete()  // a parte de configurações da fechdaruda
{
    char Preferences[32];

    flagInterfaceFI = 0x03;
    interfaceFI();

    bluetooth.println(22);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            //      Serial.print("Comando de configuração: ");
            //      Serial.println(command);

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

                //        Serial.print(Preferences);
                //        Serial.print("Configuração de proximidade: ");
                //        Serial.println((char)(flagProximityOpening+0x30));
                //        Serial.print("Configuração de som: ");
                //        Serial.println((char)(flagWarningSound+0x30));
                //        Serial.print("Configuração de luz: ");
                //        Serial.println((char)(flagLightWarning+0x30));

                b1 = 0;
                command = "";

            }  // end if
            else if (b1 == '\n' || b1 == '\r') {
                b1 = 0;
                command = "";
            }

        }  // end while
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

    }  // end if

}  // end setupDeviceComplete

//*****************************************************************************************************************
// Function to go sleep ATMEGA328P
//*****************************************************************************************************************

void goToSleep() {
    //  Serial.println("Vou dormir");

    timeOutGoToSleep = 15000;

    disconnectBLE1010();
    activeBLE1010Connect();
    flagInterrupt = 0x00;

    attachInterrupt(digitalPinToInterrupt(pinToPushButton), interrupt01, FALLING);
    attachInterrupt(digitalPinToInterrupt(pinWakeuC), interrupt02, RISING);

    // Enter power down state with ADC and BOD module disabled. Wake up when wake up pin is low.
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);

    detachInterrupt(digitalPinToInterrupt(pinToPushButton));
    detachInterrupt(digitalPinToInterrupt(pinWakeuC));

}  // end goToSleep

//*****************************************************************************************************************
// Function to battery level
//*****************************************************************************************************************

void readBatteryLevel() {
    /*

    ACRESENTAR O POLINÔMIO DESCOBERTO COM A REGRESSÃO LINEAR PARA O CÁLCULO MAIS EFICIENTE

    */

    // To 3.3V
    // batteryLevel5VFloat  = analogRead(pinToBattery01);
    // batteryLevel5V   = (batteryLevel5VFloat*constBatteryLevel);
    // To 3.3V
    batteryLevelFloat = 0;
    for (int j = 0; j < nSamples; j++) {
        batteryLevelFloat += analogRead(pinToBattery01);
    }
    batteryLevelFloat /= nSamples;
    batteryLevel = (batteryLevelFloat * constBatteryLevel);

    if (batteryLevel > maxValueBattery) {
        // batteryLevel = maxValueBattery;
        percBatteryLevel = (((maxScaleBattery - (maxValueBattery - batteryLevel)) * percBattery) / maxScaleBattery);

    }  // end if

    else if (batteryLevel < minValueBattery) {
        // batteryLevel = minValueBattery;
        percBatteryLevel = (((maxScaleBattery - (maxValueBattery - batteryLevel)) * percBattery) / maxScaleBattery);

    }  // end else if

    else
        percBatteryLevel = (((maxScaleBattery - (maxValueBattery - batteryLevel)) * percBattery) / maxScaleBattery);

    if (percBatteryLevel > oldPercBatteryLevel)
        percBatteryLevel = oldPercBatteryLevel;
    else
        oldPercBatteryLevel = percBatteryLevel;

}  // end readBatteryLevel

//*****************************************************************************************************************
// Function to up date token
//*****************************************************************************************************************

/*void upDateToken()
{

  DateTime agora    = rtc.now();
  valueTimestamp    = unix.get(agora.year(), agora.month(), agora.day(), agora.hour(), agora.minute(), agora.second());
  //valueTimestamp    = inputTimeStamp;
  difference      = ((valueTimestamp - pointZero)/(interval));

  macAddressHexDevic  = inputSeed01;
  macAddressHexDevic2 = macAddressHexDevic;
  numberToken01   = getpass(difference);

  macAddressHexDevic  = 0;
  macAddressHexDevic  = inputSeed02;
  macAddressHexDevic2 = macAddressHexDevic;
  numberToken02   = getpass(difference);

  macAddressHexDevic  = 0;
  macAddressHexDevic  = inputSeed03;
  macAddressHexDevic2 = macAddressHexDevic;
  numberToken03   = getpass(difference);

  macAddressHexDevic  = 0;
  macAddressHexDevic  = inputSeed04;
  macAddressHexDevic2 = macAddressHexDevic;
  numberToken04   = getpass(difference);

  macAddressHexDevic  = 0;

} // end upDateToken */

//*****************************************************************************************************************
// Function to disconnect BLE1010
//*****************************************************************************************************************

void disconnectBLE1010() {
    bluetooth.write(lostConnecBLE1010);
    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

}  // end disconnectBLE1010

//*****************************************************************************************************************
// Function to disconnect BLE1010
//*****************************************************************************************************************

void activeBLE1010Connect() {
    bluetooth.write(toStatePIO60);
    delay(timeToWaitBLE1010);

    if (bluetooth.available() > 0) {
        while (bluetooth.available()) {
            b1 = bluetooth.read();
            command += b1;

            if (b1 == '\n') {
                b1 = 0;
                command = "";

            }  // end if

        }  // end while

    }  // end if

}  // end activeBLE1010Connect

//*****************************************************************************************************************
// Function to verifier token pass and future
//*****************************************************************************************************************
/*
void tokenPassAndFuture()
{

  DateTime agora    = rtc.now();
  valueTimestamp    = unix.get(agora.year(), agora.month(), agora.day(), agora.hour(), agora.minute(), agora.second());
  //valueTimestamp    = inputTimeStamp;
  difference      = ((valueTimestamp - pointZero)/(interval));
  difference2     = difference - 1;

  // Token pass 01
  macAddressHexDevic  = inputSeed01;
  macAddressHexDevic2 = macAddressHexDevic;
  token01Pass     = getpass(difference2);
  macAddressHexDevic  = 0;

  // Token pass 02
  macAddressHexDevic  = inputSeed02;
  macAddressHexDevic2 = macAddressHexDevic;
  token02Pass     = getpass(difference2);
  macAddressHexDevic  = 0;

  valueTimestamp    = unix.get(agora.year(), agora.month(), agora.day(), agora.hour(), agora.minute(), agora.second());
  //valueTimestamp    = inputTimeStamp;
  difference      = ((valueTimestamp - pointZero)/(interval));
  difference3     = difference + 1;

  // Token Future 01
  macAddressHexDevic  = inputSeed01;
  macAddressHexDevic2 = macAddressHexDevic;
  token01Future   = getpass(difference2);
  macAddressHexDevic  = 0;

  // Token Future 02
  macAddressHexDevic  = inputSeed02;
  macAddressHexDevic2 = macAddressHexDevic;
  token02Future   = getpass(difference2);
  macAddressHexDevic  = 0;

} // end tokenPassAndFuture ******* tudo bobeira por causa do RTC */

//*****************************************************************************************************************
// Function to token
//*****************************************************************************************************************

unsigned long getpass_do_lolis(unsigned long difference, unsigned long seed) {
    unsigned long b[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    b[0] = 1;
    unsigned int y[4];

    for (uint8_t index = 1; index < 32; index++)  // Create the bit mask
    {
        b[index] = b[index - 1] << 1;

    }  // end for

    for (unsigned int j = 0; j < difference; j++) {
        y[0] = (b[31] & seed) ? 1 : 0;
        y[1] = (b[21] & seed) ? 1 : 0;
        y[2] = (b[1] & seed) ? 1 : 0;
        y[3] = (b[0] & seed) ? 1 : 0;
        seed = seed << 1 | (y[0] ^ y[1] ^ y[2] ^ y[3]);

    }  // end for

    return seed;

}  // end getpass

//*****************************************************************************************************************
// Function to turn the motor 01
//*****************************************************************************************************************

void turnTheMotor01() {
    flagStateMotor = 0;
    if (EEPROM.read(calibrationOk) == 0x00)
        flagSoundBrand = 0x01;
    else if (EEPROM.read(calibrationOk) == 0x01)
        flagSoundBrand = 0x02;
    flagStallMotor = 0x01;

    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, HIGH);
    if (flagLockTurns == 0x01) delay(timeToSoftStart);
    readINA219();
    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, LOW);

    functionPanic();
    delay(10 * timeToWait);

    if (flagLineUpMotor == 0x01) {
        digitalWrite(pinTurn01, HIGH);
        digitalWrite(pinTurn02, LOW);
        delay(timeToLineUP);
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, LOW);

        flagLineUpMotor = 0x00;

    }  // end if

    functionPanic();
    if (flagWarningSound == 0x01) {
        soundBrand();
    }

}  // end turnTheMotor01

//*****************************************************************************************************************
// Function to turn the motor 02
//*****************************************************************************************************************

void turnTheMotor02() {
    flagStateMotor = 1;
    if (EEPROM.read(calibrationOk) == 0x00)
        flagSoundBrand = 0x02;
    else if (EEPROM.read(calibrationOk) == 0x01)
        flagSoundBrand = 0x01;
    flagStallMotor = 0x01;

    digitalWrite(pinTurn01, HIGH);
    digitalWrite(pinTurn02, LOW);
    if (flagLockTurns == 0x01) delay(timeToSoftStart);
    readINA219();
    digitalWrite(pinTurn01, LOW);
    digitalWrite(pinTurn02, LOW);

    functionPanic();
    delay(10 * timeToWait);

    if (flagLineUpMotor == 0x01) {
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, HIGH);
        delay(timeToLineUP);
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, LOW);

        flagLineUpMotor = 0x00;

    }  // end if

    functionPanic();
    if (flagWarningSound == 0x01) {
        soundBrand();
    }

}  // end turnTheMotor02

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
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 1))
                functionDelta();
            else if ((flagStateMotor == 1) && (flagStatusDoor02 == 0))
                functionDelta();
            else {
                for (int i = 0; i < nSamples; i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
                currentInmA /= nSamples;

            }  // end else

        }  // end if

        else {
            for (int i = 0; i < nSamples; i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
            currentInmA /= nSamples;

        }  // end else

        if (counterLockTurns01 == 1) flagLockTurnsError01 = 0x01;
        if (counterLockTurns02 == 1) flagLockTurnsError02 = 0x01;

        if ((flagLockTurns == 0x01) &&
            (flagVerifierCalibration == 0x01) &&
            (flagLockTurnsError01 == 0x00) &&
            (flagLockTurnsError02 == 0x00)) {
            // To 1-1
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 1)) {
                // if((abs(currentInmA) > turnOne1) && (abs(currentInmA) < turnOne2))
                if (deltaD < turnOne3) {
                    counterLockTurns01++;

                    flagLineUpMotor = 0x00;
                    flagStallMotor = 0x00;
                    delay(1500 * timeToWait);
                    digitalWrite(pinTurn01, LOW);
                    digitalWrite(pinTurn02, LOW);

                }  // end if

                if (abs(currentInmA) > stallMotor) flagStallMotor = 0x00;

                if (millis() - timeoutStallMotor > timeoutMotor) flagStallMotor = 0x00;

            }  // end if

            // To 0-0
            else if ((flagStateMotor == 1) && (flagStatusDoor02 == 0)) {
                // if((abs(currentInmA) > turnOne1) && (abs(currentInmA) < turnOne2))
                if (deltaD < turnOne3) {
                    counterLockTurns02++;

                    flagLineUpMotor = 0x00;
                    flagStallMotor = 0x00;
                    delay(1500 * timeToWait);
                    digitalWrite(pinTurn01, LOW);
                    digitalWrite(pinTurn02, LOW);

                }  // end if

                if (abs(currentInmA) > stallMotor) flagStallMotor = 0x00;

                if (millis() - timeoutStallMotor > timeoutMotor) flagStallMotor = 0x00;

            }  // end if

            else {
                flagLineUpMotor = 0x01;

                if (abs(currentInmA) > stallMotor) flagStallMotor = 0x00;

                if (millis() - timeoutStallMotor > timeoutMotor) flagStallMotor = 0x00;

            }  // end else

        }  // end if

        else {
            // To 0-1
            if ((flagStateMotor == 1) && (flagStatusDoor02 == 1)) {
                counterLockTurns01 = 0;
                flagLockTurnsError01 = 0x00;

            }  // end if

            // To 1-0
            if ((flagStateMotor == 0) && (flagStatusDoor02 == 0)) {
                counterLockTurns02 = 0;
                flagLockTurnsError02 = 0x00;

            }  // end if

            flagLineUpMotor = 0x01;

            if (abs(currentInmA) > stallMotor) flagStallMotor = 0x00;

            if (millis() - timeoutStallMotor > timeoutMotor) flagStallMotor = 0x00;

        }  // end if

    }  // end while

    timeTime = 0;
    timeTime2 = 0;
    deltaD = 0;
    currentInmA = 0;
    timeoutStallMotor = 0;

    ina219.powerSave(true);

}  // end readINA219

//*****************************************************************************************************************
// Function to panic
//*****************************************************************************************************************

void functionPanic() {
    ina219.powerSave(false);

    timeToPanic = millis();

    while (millis() - timeToPanic < timeToPanicSegurite) {
        for (int i = 0; i < nSamples; i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
        currentInmA /= nSamples;

        if (abs(currentInmA) >= valueToPanic) {
            flagToPanic = 0x01;

            digitalWrite(pinTurn01, LOW);
            digitalWrite(pinTurn02, LOW);

        }  // end if

        else {
            flagToPanic = 0x00;
            timeToPanic = 0;

        }  // end else

    }  // end while

    ina219.powerSave(true);

    if (flagToPanic == 0x01) {
        funcReset();
    }

    else {
        currentInmA = 0;
        timeToPanic = 0;

    }  // end else

}  // end functionPanic

//*****************************************************************************************************************
// Function to calculate derivada
//*****************************************************************************************************************

void functionDelta() {
    currentInmAOld = currentInmA;
    currentInmA = 0;
    timeTime = millis();
    for (int i = 0; i < (2 * nSamples); i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
    // for(int i = 0; i <nSamples; i++)  analogBattery = analogRead(pinADC) + analogBattery;

    currentInmA /= nSamples;
    timeTime2 = millis();

    deltaD = ((currentInmA - currentInmAOld) / (timeTime2 - timeTime)) * 100;

}  // end functionDelta

//*****************************************************************************************************************
// Function to setup calibration
//*****************************************************************************************************************

void setupCalibration() {
    flagInterfaceFI = 0x02;
    interfaceFI();

    bluetooth.println(11);

    flagCalibrationFI01 = 0x01;
    flagCalibrationFI02 = 0x01;

    ina219.powerSave(false);

    while (flagCalibrationFI01 == 0x01) {
        while (flagCalibrationFI02 == 0x01) {
            char c;

            if (bluetooth.available() > 0) {
                while (bluetooth.available()) {
                    c = processCharInput(cmdBuffer, bluetooth.read());

                    if (c == '\n') {
                        readCalibration();

                        timeoutStallMotor02 = millis();

                        cmdBuffer[0] = 0;

                    }  // end if

                }  // end while

            }  // end if

        }  // end while

        for (int i = 0; i < nSamples; i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
        currentInmA /= nSamples;

        if (abs(currentInmA) > stallMotor) {
            if (flagCalibrationFI03 == 0x01) flagCalibrationFI02 = 0x01;
            if (flagCalibrationFI03 == 0x02) {
                flagCalibrationFI02 = 0x00;
                flagCalibrationFI01 = 0x00;

            }  // end if

            if (flagTurnMotor == 0x01) flagTurnMotor = 0x02;
            if (flagTurnMotor == 0x03) flagTurnMotor = 0x04;

            turnMotor();

        }  // end if

        if (millis() - timeoutStallMotor02 > timeoutMotor) {
            if (flagCalibrationFI03 == 0x01) flagCalibrationFI02 = 0x01;
            if (flagCalibrationFI03 == 0x02) {
                flagCalibrationFI02 = 0x00;
                flagCalibrationFI01 = 0x00;

            }  // end if

            if (flagTurnMotor == 0x01) flagTurnMotor = 0x02;
            if (flagTurnMotor == 0x03) flagTurnMotor = 0x04;

            turnMotor();

        }  // end if

    }  // end while

    timeoutStallMotor03 = millis();

    while (flagStatusDoor == 0x01) {
        flagTurnMotor = 0x01;
        turnMotor();

        for (int i = 0; i < nSamples; i++) currentInmA = ina219.getCurrent_mA() + currentInmA;
        currentInmA /= nSamples;

        if (abs(currentInmA) > stallMotor) {
            flagTurnMotor = 0x02;
            turnMotor();
            flagStatusDoor = 0x00;

        }  // end while

        if (millis() - timeoutStallMotor03 > timeoutMotor) {
            flagTurnMotor = 0x02;
            turnMotor();
            flagStatusDoor = 0x00;

        }  // end if

    }  // end if

    ina219.powerSave(true);

    currentInmA = 0;
    timeoutStallMotor02 = 0;
    timeoutStallMotor03 = 0;

    disconnectBLE1010();
    delay(500 * timeToWait);

    flagVerifierCalibration = 0x01;
    EEPROM.update(verifierCalibration, flagVerifierCalibration);

    flagInterfaceFI = 0x02;
    interfaceFI();

}  // end setupCalibration

//*****************************************************************************************************************
// Function to read calibration
//*****************************************************************************************************************

void readCalibration() {
    if (strcmp(commandToStartCali, cmdBuffer) == 0) {
        flagInterfaceFI = 0x03;
        interfaceFI();

        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x01;
        flagTurnMotor = 0x01;

        turnMotor();

        bluetooth.println(11);
        delay(1000 * timeToWait);

    }  // end if

    else if (strcmp(commandToDoorClose, cmdBuffer) == 0) {
        flagInterfaceFI = 0x03;
        interfaceFI();

        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x02;
        flagTurnMotor = 0x03;

        flagStatusDoor = 0x00;
        flagStatusDoor02 = flagStatusDoor;
        flagStateMotor = 0;
        // digitalWrite(pinTurn01,   LOW);
        // digitalWrite(pinTurn02,   HIGH);
        bluetooth.println(11);
        bluetooth.println(1);

        EEPROM.update(calibrationOk, flagStatusDoor02);

        turnMotor();

        delay(1000 * timeToWait);

    }  // end if

    else if (strcmp(commandToDoorOpen, cmdBuffer) == 0) {
        flagCalibrationFI02 = 0x00;
        flagCalibrationFI03 = 0x02;
        flagTurnMotor = 0x03;

        flagStatusDoor = 0x01;
        flagStatusDoor02 = flagStatusDoor;
        flagStateMotor = 1;
        // digitalWrite(pinTurn01,   HIGH);
        // digitalWrite(pinTurn02,   LOW);
        bluetooth.println(11);
        bluetooth.println(2);

        EEPROM.update(calibrationOk, flagStatusDoor02);

        turnMotor();

        delay(1000 * timeToWait);

    }  // end if

    else {
        bluetooth.println(22);
        delay(1000 * timeToWait);

    }  // end else

}  // end readCalibration

//*****************************************************************************************************************
// Turn motor one
//*****************************************************************************************************************

void turnMotor() {
    if (flagTurnMotor == 0x01) {
        digitalWrite(pinTurn01, HIGH);
        digitalWrite(pinTurn02, LOW);

    }  // end if

    if (flagTurnMotor == 0x02) {
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, HIGH);
        delay(500 * timeToWait);
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, LOW);

    }  // end if

    if (flagTurnMotor == 0x03) {
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, HIGH);

    }  // end if

    if (flagTurnMotor == 0x04) {
        digitalWrite(pinTurn01, HIGH);
        digitalWrite(pinTurn02, LOW);
        delay(500 * timeToWait);
        digitalWrite(pinTurn01, LOW);
        digitalWrite(pinTurn02, LOW);

    }  // end if

}  // end turnMotor

//*****************************************************************************************************************
// Function to interface LEDs
//*****************************************************************************************************************

void interfaceFI() {
    if (flagInterfaceFI == 0x01) {
        for (int i = minValuePWM; i < maxValuePWM; i++) {
            analogWrite(pinToLED03, i);
            delay(2 * timeToWait);

        }  // end for

        delay(50 * timeToWait);

        for (int j = maxValuePWM; j > minValuePWM; j--) {
            analogWrite(pinToLED03, j);
            delay(2 * timeToWait);

        }  // end for

    }  // end if

    else if (flagInterfaceFI == 0x02) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);
        if (flagWarningSound == 0x01) tone(pinToBuzzer, toneDefault, (timeToToneSeedSetup * timeToWait));
        delay(timeToToneSeedSetup);
        noTone(pinToBuzzer);
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, LOW);

    }  // end if

    else if (flagInterfaceFI == 0x03) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);
        if (flagWarningSound == 0x01) tone(pinToBuzzer, toneDefault, (timeToToneDefault * timeToWait));
        delay(timeToToneDefault);
        noTone(pinToBuzzer);
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, LOW);

    }  // end if

    else if (flagInterfaceFI == 0x04) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);

    }  // end if

    else if (flagInterfaceFI == 0x05) {
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, LOW);

    }  // end if

    else if (flagInterfaceFI == 0x06) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);
        delay(500 * timeToWait);
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, LOW);

    }  // end if

    else if (flagInterfaceFI == 0x07) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);
        tone(pinToBuzzer, toneDefault, (timeToToneSeedSetup * timeToWait));
        delay(timeToToneSeedSetup);
        noTone(pinToBuzzer);
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, LOW);

    }  // end if

    else if (flagInterfaceFI == 0x08) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);
        tone(pinToBuzzer, toneDefault, (timeToToneSeedSetup * timeToWait));
        delay(timeToToneSeedSetup);
        noTone(pinToBuzzer);

    }  // end if

}  // end interfaceFI

//*****************************************************************************************************************
// Function to sound brand
//*****************************************************************************************************************

void soundBrand() {
    if (flagSoundBrand == 0x01) {
        for (int i = 0; i < maxNotes; i++) {
            if (flagWarningSound == 0x01) tone(pinToBuzzer, sBrandOpen[i], timeToTone);
            delay(500 * timeToWait);

        }  // end for

        delay(500 * timeToWait);
        noTone(pinToBuzzer);

    }  // end if

    else if (flagSoundBrand == 0x02) {
        for (int i = 0; i < maxNotes; i++) {
            if (flagWarningSound == 0x01) tone(pinToBuzzer, sBrandClose[i], timeToTone);
            delay(500 * timeToWait);

        }  // end for

        delay(500 * timeToWait);
        noTone(pinToBuzzer);

    }  // end else

}  // end soundBrand

//*****************************************************************************************************************
// Function to show battery level
//*****************************************************************************************************************

void showBatteryLevel() {
    if ((percBatteryLevel <= 100) && (percBatteryLevel > 60)) {
        digitalWrite(pinToLED01, HIGH);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);

    }  // end if

    else if ((percBatteryLevel <= 60) && (percBatteryLevel > 30)) {
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, HIGH);
        digitalWrite(pinToLED03, HIGH);

    }  // end else if

    else if ((percBatteryLevel <= 30) && (percBatteryLevel >= 0)) {
        digitalWrite(pinToLED01, LOW);
        digitalWrite(pinToLED02, LOW);
        digitalWrite(pinToLED03, HIGH);

    }  // end else if

    if (flagLightWarning == 0x01) delay(3 * showTheBatteryLevel);
    if (flagLightWarning == 0x02) delay(5 * showTheBatteryLevel);
    if (flagLightWarning == 0x03) delay(10 * showTheBatteryLevel);

    digitalWrite(pinToLED01, LOW);
    digitalWrite(pinToLED02, LOW);
    digitalWrite(pinToLED03, LOW);

}  // end showBatteryLevel

//*****************************************************************************************************************
// Function to deliver the number of jumps (with random generation)
//*

unsigned long send_saltos(unsigned long random_input, unsigned long seed) {
    randNumber = random(0, 9999);

    // randNumber = 1313;
    bluetooth.println(randNumber + random_input + seed);

    return randNumber;
}
