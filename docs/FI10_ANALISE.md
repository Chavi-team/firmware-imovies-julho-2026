# FI 1.0 — Análise do firmware legado × firmware novo (chavi_fi)

> Relatório de engenharia reversa, 05/07/2026. Objetivo: reconciliar o firmware
> novo (`Imoveis/firmware/chavi_fi/chavi_fi.ino`, v2.6.3) com as placas **FI 1.0**
> (silk "JOAO. V. ALVARES 10/2020 v2.1") e explicar/resgatar as duas unidades
> mortas na bateria (**CH001FI000718** e **CH001FI000629**).
>
> Fontes da verdade analisadas (arquivo:linha citados ao longo):
> - `Firmware-Antigo/src-tauri/resources/firmware/FI_1_0/FI_1_0.ino` (2731 l., "FI Final v8.1", 04/03/2021 + patch 17/01/2023)
> - `Firmware-Antigo/src-tauri/resources/firmware/FI_1_0_400/FI_1_0_400.ino` (2445 l., "FI 400", NMOS como chave, 07/06/2021 + 25/01/2023)
> - `Firmware-Antigo/src-tauri/resources/firmware/FI_1_5/FI_1_5.ino` e `FI_1_5_400/FI_1_5_400.ino` (contraste)
> - `Firmware-Antigo/src-tauri/resources/firmware/include/*` (SerialNumber.h, interrupt.h, utils.h, version.h)
> - `Firmware-Antigo/src-tauri/resources/{upload, upload2.sh, seedGenerator.py, firmware/FI_1_0_400/ble.py}`
> - `Firmware-Antigo/src/at.js` (esteira Tauri: `ConfiguracaoAT`, `calcularHexBefcAftc`)
> - `Firmware-Antigo/src-tauri/resources/firmware/FI15400/include/SerialNumber1.h` (header gerado por dispositivo)
> - Fotos `~/Downloads/IMG_9808–9813, 9816` (placa 1.0 v2.1, placas 1.5 v2.5, bateria)

---

## 1. As TRÊS variantes de FI 1.0 e como distinguí-las

O ponto mais importante da análise: **"FI 1.0" não é uma placa só**. O mesmo PCB
v2.1 existe em dois estados elétricos, e o firmware de produção era escolhido por
unidade na hora de gravar (`upload`, `upload2.sh -mosfet`):

| | **FI_1_0** (sem mosfet) | **FI_1_0_400** (com mosfet) |
|---|---|---|
| Chave de energia | nenhuma — MCU sempre alimentado | **NMOS em plaquinha verde** emendada no cabo da BATERIA (retrofit); gate = fio verde vindo de um **PIO do módulo BLE** |
| Firmware `.ino` | `FI_1_0/FI_1_0.ino` | `FI_1_0_400/FI_1_0_400.ino` compilado com `-DpinMosfetBle=7\|8\|9` (`upload:28-37`) |
| Baud MCU↔módulo | **9600** (`SERIAL_BAUD 9600`, FI_1_0.ino:82,607-611; módulo gravado com `AT+BAUD2` no 1º boot, :819) | **2400** (`bluetooth.begin(2400)` + `AT+UART1` + `AT+BAUD0` **todo boot**, FI_1_0_400.ino:617-628) |
| Dormir | `LowPower.powerDown(SLEEP_FOREVER)` (:1856) após DROP+PIO60 | **corta o próprio trilho**: `AT+DROP` → `AT+PIO61` → `AT+PIO70/80/90` (gatePinLow) após 25 s; o `powerDown` está **comentado** (:1588-1637) |
| Acordar | módulo sobe PIO6 → PD3/INT1 RISING (`AFTC008`) | módulo **religa o trilho** na conexão (`AFTC0x8`) e na desconexão (`BEFC0x0` com bit do mosfet=1) → MCU dá boot |
| Wake config | `AT+BEFC000` / `AT+AFTC008` (:105-106) | `AT+BEFC010/020/040` + `AT+AFTC018/028/048` conforme pino 7/8/9 (:110-135) |

O firmware da terceira 1.0 (que **liga** com a mesma bateria) mantém o `BEFC` do
módulo em NVM — é o `BEFC` que liga o trilho quando a bateria é inserida. Ou seja:
**nessas placas, quem "dá o power-on" na eletrônica é a NVM do módulo BLE**, não a
bateria. Apagar essa NVM (AT+RENEW) = placa morta na bateria. É exatamente o caso
das 0718/0629.

**Como distinguir em bancada (sem esquema):**
1. **Visual**: plaquinha verde com SOT-23 emendada nos fios do conector BATERIA +
   fio verde saindo de uma castelação do módulo BLE (fotos IMG_9816 — 1.0 v2.1;
   IMG_9808/9809/9813 — 1.5 v2.5 tem o MESMO retrofit). Sem plaquinha = sem mosfet.
2. **Comportamental**: `TST-HIB` do chavi_fi — silêncio após o DROP = cortou (tem
   mosfet e o gate é o PIO8); 3 bipes graves = não cortou (sem mosfet **ou gate em
   outro PIO** — ver §5).
3. **Elétrico**: com bateria, medir VCC do MCU (pino 4/6 do header FIRMWARE):
   sem mosfet ≈ VBAT sempre; com mosfet, varia com o PIO.
4. **Pelo firmware antigo gravado**: módulo respondendo a 2400 → era _400; a 9600 → era FI_1_0 puro.

**O firmware novo NÃO precisa de um 3º tipo de placa**: pinout idêntico; os
comandos de PIO/BEFC são inócuos numa placa sem mosfet (PIOs não conectados) e o
`dormir()` já tem fallback `powerDown` quando o corte não acontece
(chavi_fi.ino:790-798). Duas ressalvas: (a) **não** setar PIO6=1 no BEFC (placa sem
mosfet depende da borda do PIO6 p/ acordar — mitigado pelo data-wake); (b) o RENEW
é fatal só nas COM mosfet, mas como não dá pra saber antes, a regra vira "nunca" (§7).

---

## 2. Pinagem completa — FI_1_0 × FI_1_0_400 × FI_1_5 × chavi_fi (placa10)

Fonte: FI_1_0.ino:53-74, FI_1_0_400.ino:55-76, FI_1_5.ino:34-49, chavi_fi.ino:81-95.

| Função | FI_1_0 / FI_1_0_400 | FI_1_5 | chavi_fi (placa10=1) | OK? |
|---|---|---|---|---|
| BLE RX do MCU (módulo TX) | pino 4 = PD4 (`pinRxBLE1010`; construtor `SoftwareSerial(5,4)` → RX=PD5? ver nota) | PD4 | `SoftwareSerial(PD5, PD4)` | ✅ idêntico |
| BLE TX do MCU | pino 5 = PD5 | PD5 | idem | ✅ |
| Buzzer | 6 = PD6 | PD6 | PD6 | ✅ |
| LED1/LED2/LED3 | 7/8/9 = PD7/PB0/PB1 (discretos; LED3 com PWM no fade, :2561) | 3×WS2812 no PB3 | PD7/PB0/PB1 discretos, sem FastLED | ✅ |
| Motor A/B | **10/11 = PB2/PB3** (`pinTurn01/02`) | **PB1/PB2** | PB2/PB3 quando placa10 (chavi_fi:807) | ✅ |
| Bateria ADC | A1=PC1 (`pinToBattery01`) | PC1 | A1 | ✅ (escala difere, §6.4) |
| 5V ADC | A0=PC0 (`pinToBattery02`, só `pinMode INPUT`, nunca lido) | PC0 | A0 usado só no `randomSeed` | ✅ vestigial |
| I2C (INA219 0x45 + RTC DS1307) | A4/A5 | A4/A5 (sem RTC) | Wire/INA219 0x45 | ✅ (RTC não usado — ok, §6.6) |
| Botão | 2 = PD2/INT0, FALLING | PD2 | PD2 FALLING | ✅ |
| Wake do módulo | 3 = PD3/INT1, RISING | PD3 | PD3 RISING + data-wake PCINT | ✅ |
| **Hall VCC 01** | **A2 = PC2, `pinMode(OUTPUT)` e NUNCA escrito HIGH** (FI_1_0.ino:625-635) | — | **não configurado (flutuando)** | ⚠️ ver §6.3 |
| **Hall VCC 02** | **A7** — `pinMode(OUTPUT)` é INÓCUO (A6/A7 do ATmega328 são só entrada analógica) | — | não configurado | ✅ vestigial de fato |
| Hall IN 01/02 | A3/A6 `INPUT`, **nunca lidos** em lugar nenhum (nem no turnMotor) | — | — | ✅ vestigiais |
| ISP | header 2x3 "FIRMWARE" (MISO SCK RST / VCC MOSI GND — foto 9816) | idem | — | — |
| Serial HW | header GND/VCC/RX/TX/DTR na borda (foto), PD0/PD1 | idem | debug opcional PD1 | — |

> Nota do construtor: o legado declara `SoftwareSerial bluetooth(pinTxBLE1010, pinRxBLE1010)`
> = `(5,4)` → **RX=PD5, TX=PD4** (a assinatura é `(rx,tx)`; os NOMES das macros estão
> trocados no legado). O chavi_fi faz `bluetooth(PIN_BLE_TX, PIN_BLE_RX)` = `(PD5,PD4)`
> → **o mesmo par físico, na mesma ordem**. Comprovado em bancada (o BLE funciona nas 1.5
> e a 0629 respondia AT quando alimentada). Nenhuma divergência real.

**Conclusão da pinagem: o branch `placa10` do chavi_fi está correto.** Os hall
sensors são vestigiais no legado (VCC nunca energizado, entradas nunca lidas — o
`turnMotor`/`readINA219` usa SÓ a corrente do INA219 e timeout). Não há nenhum pino
"que precisa ficar HIGH" escondido no MCU; **o pino que segura o trilho fica no
MÓDULO BLE (PIO), não no ATmega**.

---

## 3. Arquitetura de energia (com evidências)

### 3.1 FI_1_0 (sem mosfet)
- `goToSleep()` FI_1_0.ino:1843-1861: `timeOutGoToSleep=15000` → `disconnectBLE1010()`
  (AT+DROP, :1946) → `activeBLE1010Connect()` (**AT+PIO60**, :1971 — arma a borda de wake)
  → attach INT0/INT1 → `LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF)`.
- Nada corta trilho nenhum. Nenhum `savePinState/clearPinState` (isso é só da FI_1_5,
  `include/interrupt.h:18-53`, que grava DDR/PORT na EEPROM 200-205 a cada sono — o
  desgaste de EEPROM documentado no CONTEXTO).
- Wake: `AT+BEFC000`/`AT+AFTC008` (:105-106) = PIO6 baixo antes / alto depois da
  conexão → borda RISING no PD3.

### 3.2 FI_1_0_400 (com mosfet) — a arquitetura das placas mortas
- `#if pinMosfetBle == 7|8|9` (FI_1_0_400.ino:110-135) define:
  - `preConn = AT+BEFC010|020|040` — **bit do mosfet=1, PIO6=0** → trilho LIGADO
    enquanto desconectado (é isso que liga o MCU quando a bateria entra!);
  - `posConn = AT+AFTC018|028|048` — mosfet=1 + PIO6=1 (wake) durante conexão;
  - `gatePinLow/High = AT+PIO70/71|80/81|90/91`.
- `goToSleep()` :1588-1637: espera 25 s de `startTime`, então `AT+DROP` → `AT+PIO61`
  → **`gatePinLow` (corta o PRÓPRIO trilho — o MCU morre aqui)**. O
  `LowPower.powerDown` está comentado (:1624-1635). O ciclo de vida é:
  bateria entra → BEFC liga trilho → boot → 25 s ouvindo → corta →
  conexão religa (AFTC) → atende → desconexão religa (BEFC) → 25 s → corta.
- **Codificação BEFC/AFTC (confirmada em 3 lugares)**: 3 dígitos hex = 12 bits,
  bit N = PIO(N+3) → bit3=PIO6, bit4=PIO7, bit5=PIO8, bit6=PIO9.
  Comentário FI_1_0_400.ino:113 ("028 é 0000 0010 1000, pino 8 e pino 6"),
  `at.js:515-541` (`calcularHexBefcAftc`: `mosfetBit = pin-3`, `pin6Bit = 3`) e
  `upload2.sh:73-99` (mesma conta em bash).

### 3.3 De onde vem o pino do mosfet (e a pegadinha)
- **`upload` (produção antiga)**: FI_1_0_400 exige pino **7, 8 ou 9** (:28-37) e compila
  com `-DpinMosfetBle=`.
- **`at.js` (esteira Tauri atual)**: a tela oferece **pinos 4, 5, 6 e 7** (+ campo
  custom, default "4"!) — `at.js:439,455` — e grava por BLE (OTA) o lote
  `AT+SHIELD1 → AT+BAUD0 → AT+PWRM1 → AT+MODE2 → AT+BEFC<hex> → AT+AFTC<hex> →
  AT+NAME<serial> → AT+RESET`, com terminador **`\r\n`** (`at.js:335,562-571`).
- **`upload2.sh`**: aceita pino **3..14** e manda por OTA (`ble.py`)
  `AT+SHIELD1 AT+BAUD0 AT+PWRM1 [AT+BEFC AT+AFTC] AT+VERS?`, e o comentário final
  manda **"Reinsert the battery"** — confirmando que o power-on da placa depende do
  BEFC gravado.
- ⚠️ **Consequência**: o pino do gate NÃO é fixo por modelo — foi escolhido por
  unidade pelo técnico do retrofit. O universo documentado é **{7,8,9}** no fluxo
  antigo e **{4,5,6,7,custom}** no novo. O chavi_fi hoje levanta só 7/8/9
  (`AT+PIO71/81/91` + `BEFC070/AFTC078`, chavi_fi.ino:386-389) — e o `BEFC070`
  **escreve 0 nos bits de PIO4 e PIO5**: se o gate de uma unidade estiver em 4 ou 5,
  o comando "de resgate" mantém (ou força) o trilho DESLIGADO.

### 3.4 O retrofit físico (fotos)
- **IMG_9816 (FI 1.0 v2.1, a geração das mortas)**: plaquinha verde com transistor
  emendada nos fios do conector BATERIA (canto sup. direito); módulo BLE clone
  branco sem shield metálico, antena em meandro, datecode "**pci 15-23**" (semana
  15/2023 → módulo TROCADO em manutenção, não é o de 2020) com **fio verde de bodge**
  soldado numa castelação da borda inferior do módulo indo a um pad na borda esquerda
  da placa (o gate do NMOS e/ou o wake — o pad original do footprint antigo não casa
  com o pinout do módulo novo). Disco "+BAT+" com adesivo QR = porta-moeda do **RTC
  DS1307** (o FI_1_0 usa `RTClib`, :215). Dois indutores "220" = 2 conversores DC-DC.
  Header ISP 2x3 "FIRMWARE" e header serial GND/VCC/RX/TX/DTR na borda inferior.
  R-SHUNT R100 (0R1) do INA219.
- **IMG_9808/9809/9812/9813 (FI 1.5 v2.5, CHRISTIAN ARAUJO 06/2023, CH003FI2449)**:
  MESMO retrofit — plaquinha verde com SOT-23 no cabo da bateria + fio verde do
  módulo. Ou seja, a linha "_400" existe nas duas gerações e é retrofit manual.
- **IMG_9810**: traseira (etiqueta serial CH003 FI 2449, ANATEL 05118-16-10070).
- **IMG_9811**: cartucho de bateria laranja ("FRENTE", contatos de mola).
- Diferenças 1.0 × 1.5 visíveis: 1.0 tem RTC+coin cell, 3 LEDs amarelos discretos,
  MCU TQFP com cristal ao lado; 1.5 tem WS2812 (LED1-3 RGB), sem coin cell, layout 06/2023.

---

## 4. Módulo BLE do lote 1.0 — comandos AT, terminadores, bauds

### 4.1 Tabela de comandos por variante (TODOS os usos reais)

| Comando | FI_1_0 | FI_1_0_400 | FI_1_5(_400) | esteira at.js / upload2.sh | chavi_fi |
|---|---|---|---|---|---|
| Terminador | **`\r`** em tudo (macros :99-111) | **`\r`** | **NENHUM** (macros sem \r, :74-103; `changeName` até *remove* o `\r` do SerialNumber.h via `substring(0,18)`, :1156) | **`\r\n`** (at.js:335) | `\r` (`at()`, :310-315) |
| `AT` (teste) | 1º boot | todo 1º boot + implícito | 1º boot | — | 4× (`bleResponde`) |
| `AT+TYPE0` | ✔ 1º boot | ✔ 1º boot | ✔ | — | ✔ todo boot |
| `AT+MODE2` | ✔ | ✔ | ✔ | ✔ | ✔ |
| `AT+ROLE0` | ✔ | ✔ | ✔ | — | ✔ |
| `AT+BAUD2` (9600) | ✔ 1º boot (:819) | ✗ | ✔ | — | ✗ |
| `AT+BAUD0` (2400) | ✗ | ✔ **todo boot** (:623) | ✗ | ✔ | ✔ (provisionamento) |
| `AT+UART1` | ✗ | ✔ todo boot (:620 — opcode obscuro, provavelmente ignorado pelo clone) | ✗ | ✗ | ✗ |
| `AT+NOTI1` | ✔ | ✔ | ✔ | — | ✔ |
| `AT+DELI3` | ✗ | ✗ | ✔ (:77,892) | — | ✔ |
| `AT+UTIM0` | ✗ | ✗ | **definido** (:72) mas **nunca enviado** | — | ✗ |
| `AT+BEFC`/`AT+AFTC` | `000`/`008` | `0x0`/`0x8` por pino | `000`/`008` (só ver.04+) | por pino (calcularHexBefcAftc) | `070`/`078` (placa10) ou `020`/`028` |
| `AT+PIO60` | ✔ (armar wake, a cada sono) | ✗ | ✔ | — | ✔ |
| `AT+PIO61` | ✗ | ✔ (antes do corte) | ✗ | — | ✔ (dormir) |
| `AT+PIO70/71/80/81/90/91` | ✗ | ✔ (gatePinLow/High) | `AT+PIO80` no goToSleep do FI_1_5_400:1457 | — | ✔ |
| `AT+STATUS6` / `AT+STATUS8` | ✗ | ✗ | ✔ p/ módulo **ver.03** (FI_1_5:100/910; FI_1_5_400 usa STATUS**8**:105) | — | ✔ (ver.03) |
| `AT+PWRM1` | ✗ | ✔ 1º boot (:720) | FI_1_5_400:801 | ✔ | ✔ (provisionamento) |
| `AT+SHIELD1` | ✗ | ✗ | ✗ | ✔ | ✔ (provisionamento) |
| `AT+NAME<x>` | `CHAVIFI` no 1º boot (:964) + **`AT+NAME<serial>` TODO boot** (`changeName()`, :553/1272-1299, via `println` → com `\r\n`) | idem (:564,839,1085-1108) | idem via `rotinaWriteBluetooth` | ✔ | provisionamento |
| `AT+ADDR?` | ✔ 1º boot | ✔ 1º boot (grava na EEPROM 45) | ✔ | — | ✗ |
| `AT+VERS?` | ✗ | via upload2.sh | ✔ (`CheckVersBLE`, :448) | ✔ | ✔ |
| `AT+RESET` | ✔ 1º boot (:1025) | **COMENTADO** (:862-893 — o _400 nunca reseta o módulo!) | ✔ | ✔ (fim do lote) | ✔ (provisionamento) |
| `AT+DROP` | ✔ (a cada sono) | ✔ | ✔ | — | ✔ |
| **`AT+RENEW` / `AT+DEFAULT` / `AT+IMME` / `AT+ADTY` / `AT+START`** | **✗ em TODO o legado e em TODA a esteira** | ✗ | ✗ | ✗ | **✔ só o chavi_fi (bleProvisionar PASSO 0, :427-434)** |

**Achados-chave desta tabela:**
1. **Nada no ecossistema legado jamais fez factory-reset do módulo.** O `AT+RENEW`
   é invenção do fluxo novo — o módulo dessas placas passou anos acumulando a config
   correta na NVM e ninguém nunca a apagou. As duas 1.0 morreram na PRIMEIRA vez em
   que alguém rodou RENEW numa placa cuja energia depende da NVM.
2. Os terminadores variam (`\r`, nada, `\r\n`) e todos funcionaram em produção → o
   clone "Soft AT 5.2" tolera os três. O `\r` do chavi_fi está certo.
3. O `AT+PWRM1` aparece em TODAS as esteiras (at.js, upload2.sh, FI_1_0_400) →
   forte indício de que o default de fábrica/pós-RENEW do clone é o modo de
   economia (auto-sleep), no qual o módulo **ignora comandos AT curtos** até ser
   acordado (o próprio CONTEXTO registrou isso: "o 1º AT só acorda, não responde").
4. O FI_1_0_400 **não reseta o módulo nunca** e reafirma `AT+BAUD0` todo boot —
   a frota _400 vive a 2400 por reafirmação, não por reset.

### 4.2 Tabela de baud implícita
- FI_1_0: `baud9600BLE1010 = "AT+BAUD2"` + `SERIAL_BAUD 9600` → nesse lote/clone,
  **BAUD2 = 9600**.
- FI_1_0_400/esteiras: `AT+BAUD0` + `begin(2400)` → **BAUD0 = 2400**.
- Logo a tabela do lote segue o HM-10 clássico: 0=2400(?); nos HM-10 genuínos
  BAUD0=9600 — **neste clone é 2400**, comprovado pelo par código+campo. O CONTEXTO
  já registrou que `AT+BAUD2` é ambíguo entre sub-lotes (38400 em alguns). Módulo
  novo de fábrica: 9600.

### 4.3 ver.03 × ver.04 ("Soft AT 5.2 ver.NN")
- Identificação: `AT+VERS?` → `"Soft AT 5.2 ver.03\n"` / `ver.04` (FI_1_5:90-92).
- **Wake**: ver.03 usa `AT+STATUS6` (FI_1_5:905-910) — **não tem BEFC/AFTC**;
  ver.04+ usa `BEFC/AFTC`. O FI_1_5_400 usa `STATUS8` (o "espelho" do gate PIO8).
- **Framing TX**: ver.03 exige frame terminado em `'\n'` (`ENDFrameVer03`), ver.04
  em `'\0'` (`ENDFrameVer04`) — `SendDataBLE`, FI_1_5:2283-2312; todo dado sai com
  `ENDWriteData '\r'` + o byte de versão. O chavi_fi reproduz isso no formato dos
  saltos (`(g_moduloVers==3) ? "%lu\n" : "%lu"`, chavi_fi:733).
- **Importante p/ a placa 1.0**: o FI_1_0/_400 usa `println` (\r\n) e BEFC/AFTC
  incondicional — ou seja, os módulos que a linha 1.0 recebeu em produção/manutenção
  se comportam como ver.04 (o da foto é datecode 2023). O STATUS6 do chavi_fi para
  ver.03+placa10 não tem precedente na linha 1.0 (era só da FI_1_5 SEM mosfet) —
  inócuo, mas não confiar nele como caminho de wake.

### 4.4 O que o AT+RENEW faz nesses clones (deduzido)
O legado nunca usa RENEW, então só dá para deduzir do comportamento observado +
do que as esteiras reafirmam:
- Perde `NAME` (→ anúncio sem nome ✔ observado), `BAUD` (volta ao de fábrica,
  provavelmente 9600), `BEFC/AFTC/PIO` (→ gate do mosfet cai ✔ é o kill), `MODE`,
  `NOTI`, `TYPE` e **`PWRM`** (→ volta ao auto-sleep, já que toda esteira precisa
  mandar `PWRM1`).
- Módulo em auto-sleep dos clones: anuncia em rajadas anômalas (possivelmente
  **não-conectável**) e **ignora AT curto na UART** até receber um "wake" (string
  longa, ≥80 chars no HM-10 clássico). Isso explica simultaneamente: o anúncio
  anônimo que não aceita conexão E o fato de os comandos de resgate "aceitos a
  2400" não terem tido efeito algum.

---

## 5. Por que as 0718/0629 morreram — e por que o resgate falhou

**Mecanismo da morte (confirmado pelo código):** as duas placas são FI 1.0 **com
retrofit NMOS** (plaquinha no cabo da bateria). O trilho do MCU só liga porque o
módulo BLE guarda `AT+BEFCxxx` (bit do gate=1) na NVM. O `bleProvisionar()` do
chavi_fi (chavi_fi.ino:427-434) mandou `AT+RENEW`/`AT+DEFAULT` em todos os bauds →
na hora em que o RENEW pegou, **o módulo derrubou o PIO do gate e cortou a energia
do próprio MCU no meio do provisionamento** (na bateria, nada depois disso executa).
Resultado: módulo em estado de fábrica (sem BEFC, sem nome, auto-sleep), MCU sem
energia — vivo só com USBasp.

**Por que `AT+PIO71/81/91 + BEFC070/AFTC078` a 2400 não ressuscitou** — hipóteses
ranqueadas (não são excludentes; o plano do §8 cobre todas):

1. **H1 — módulo em auto-sleep (PWRM default pós-RENEW) ignorando AT curto.**
   Toda esteira manda `AT+PWRM1`; pós-RENEW ele voltou ao default. Nenhum dos
   resgates mandou um preâmbulo de wake (string longa) nem `PWRM1` antes dos PIO.
   O "respondeu a 2400" do diagnóstico por bipes é fraco: `bleResponde()`
   (chavi_fi:321-331) aceita **qualquer byte** como resposta — lixo de baud errado
   ou eco parcial conta como "vivo".
2. **H2 — baud real ≠ 2400.** Pós-RENEW o módulo volta ao baud de fábrica (9600
   neste lote, possivelmente outro). Os comandos a 2400 viram lixo. Mesma
   observação sobre o falso-positivo do teste por "qualquer byte".
3. **H3 — gate fora de {7,8,9}.** A esteira `at.js` oferece pinos **4/5/6/7 com
   default 4** (at.js:439-457); `upload2.sh` aceita 3..14. Se o retrofit desta
   unidade usou PIO4/PIO5, então `BEFC070` além de não ligar o gate **escreve 0**
   no bit dele. E `AT+PIO41/PIO51` nunca foram tentados.
4. **H4 — a config de resgate nunca foi de fato enviada pelo firmware.**
   `configModuloLeve()` retorna cedo se `digitalRead(PIN_WAKE)==HIGH`
   (chavi_fi:374) — com o módulo pós-RENEW o PD3 pode estar alto/flutuando, e aí
   o firmware **pula silenciosamente** os PIO71/81/91 (inclusive dentro do
   `bleProvisionar`, :460 e :476, e no `diagBaudBipes`, :256).
5. **H5 — módulo semi-brickado pelo RENEW** (clones às vezes corrompem o estado
   com RENEW; o anúncio não-conectável e anônimo é compatível). Se H1-H4 falharem,
   é isso — resolve com troca do módulo ou bypass do mosfet.

Contra-evidência a favor de H1/H2 e contra o brick total: `upload2.sh`/at.js
**conectavam por BLE em módulo de fábrica** para configurá-lo — ou seja, o estado
de fábrica normal É conectável. O que observamos (não-conectável) é mais parecido
com auto-sleep do que com "fábrica".

---

## 6. Protocolo / timing do 1.0 (o que o chavi_fi precisa honrar)

1. **Leitura numérica**: `bluetooth.parseInt()` com timeout default de **1 s**, sem
   delimitador (FI_1_0.ino:1458; `setTimeout` nunca é chamado no legado 1.0). O
   chavi_fi usa `setTimeout(150)` — mais ágil, compatível.
2. **Desafio → saltos**: ao receber o 1º número, o FI_1_0 drena o buffer
   (:1462-1478) e espera **`delay(3500)`** antes de responder (código de espera do
   app deixado em produção!) — depois manda **2 linhas** `println(rand + N + seed_k)`
   com 20 ms entre elas (`send_saltos`, :2724-2731; `random(0,9999)` **sem
   randomSeed** — FIXME histórico). Ou seja: o app de campo tolera resposta até
   ~4,5 s depois do desafio; o chavi_fi respondendo em ~300 ms está folgado.
3. **Tokens**: `getpass_do_lolis` (:2040-2062) = LFSR taps 31/21/1/0, idêntico ao
   chavi_fi/CONTEXTO. Validação: abrir/fechar = seeds 1/2; setup(150993)/
   calibração(190720)/reset-seeds(140197) = seeds 3/4 (:1547-1581).
4. **Confirmação**: `println(1000|2000 + batteryLevel)` → `"1004.09\r\n"`
   (:1720/1727). ⚠️ escala da bateria do legado = `raw*4.2/1024` (:134) — o
   chavi_fi usa `5.0/1024` (chavi_fi:494,591): o número reportado ao app fica ~19%
   maior. Cosmético (indicador de bateria), mas vale alinhar para 4.2.
5. **Calibração** (:2334-2516): responde `println(11)` imediato; `CALIBRACAO-FI` →
   gira e `println(11)`; `PORTA-FECHADA` → `11` + `1`; `PORTA-ABERTA` → `11` + `2`;
   inválido → `22`. Motor da calibração gira até stall(INA)>300 mA ou 10 s. O
   chavi_fi segue a mesma máquina com os delays de 1150 ms exigidos pelo app atual.
6. **RTC DS1307**: o 1.0 tem RTC + coin cell e o seedSetup gravava timestamp
   (:1241-1244) — todo o uso de token-por-tempo está comentado ("tudo bobeira por
   causa do RTC", :2034). O chavi_fi ignorar o RTC é correto.
7. **Sono/janela**: FI_1_0 fica ~15 s ouvindo após wake (`timeOutGoToSleep`,
   :287/1846); FI_1_0_400 fica 25 s e então corta o trilho (:1589). A janela de
   20 s + teto 10 min do chavi_fi é compatível.
8. **Interrupções**: INT0=botão FALLING, INT1=wake RISING (:1305-1320) — idêntico.
9. **`funcReset` = salto p/ 0x0000** (:404) — o reset por watchdog do chavi_fi é
   superior (registradores/periféricos limpos).

---

## 7. Boot / fuses / EEPROM

- **Fuses de produção** (`upload`:50-57): `lfuse=0xFF` (**cristal externo 16 MHz**),
  `hfuse=0xD7` (EESAVE), `efuse=0xF7`, `lock=0xCF`. MCU: FI_1_0 = **m328** (328
  "puro"), FI_1_5 = m328pb.
- **Fuses do fluxo novo** (`tools/gravar.sh:53-55`): `lfuse=0xE2` (**RC interno
  8 MHz**), mesmos hfuse/efuse. O cristal de 16 MHz continua na placa, só não é
  usado. Consequências:
  - Um `.hex` legado (compilado p/ 16 MHz) gravado num chip com lfuse 0xE2 roda a
    **metade da velocidade** (UART 9600→4800 etc.). Para devolver uma placa ao
    firmware antigo, **restaurar lfuse=0xFF**.
  - **SoftwareSerial RX @ 8 MHz interno**: o RC tem cal de fábrica ±1-2 % típica
    (±10 % pior caso) e deriva com Vcc/temperatura; a 9600 a margem de amostragem
    do SoftwareSerial fica apertada (perda de byte foi observada em bancada); a
    **2400 a margem é confortável**. Decisão do chavi_fi (2400 fixo) é a correta
    para a frota _400 — e o provisionamento converge módulos 9600 (linha FI_1_0
    sem mosfet) para 2400 às cegas.
- **EEPROM (legado, confirmado)** — `docs/eeprom.md` + `seedGenerator.py`:
  `1` setupSeedOk, `2` generalSetupOk, `3` calibrationOk, `4` verifierCalibration,
  `5/15/25/35` seeds u32 LE, `45` MAC, `67` setupBleOk (só _400; write-only,
  vestigial), `100-107` flags, `150` setupProductionOk, `200-205` PIN_MODE/VALUE
  (só FI_1_5, o desgaste), `768` VersBLE, `769-779` serial sem "CH".
  `seedGenerator.py` grava: `[1]=1`, `[101/102/104/105]=1`, `[150]=1`, serial em
  769, seeds em 5/15/25/35 — ✔ compatível com o `gerar_seed.py` novo (que soma
  910=0, 912=placa). **Heurística do gerar_seed.py:80**: canal "001" → fi10 —
  cobre as CH001FI000718/629; para 1.0 fora do canal 001, passar `fi10` explícito.
- ⚠️ FI_1_0 tem um **reset total de EEPROM comentado** no setup (:486-498) e o
  FI_1_5 apaga TODA a EEPROM quando `setupProductionOk!=1` — com EESAVE ligado a
  EEPROM sobrevive ao chip-erase, então gravar um legado 1.5 por engano numa placa
  provisionada destruiria seeds/serial. Não gravar FI_1_5 em placa 1.0 (além do
  MCU divergir: m328 × m328pb).

---

## 8. PLANO DE RESGATE — CH001FI000718 e CH001FI000629

Princípios: (a) o módulo é alimentado direto da bateria; o MCU é quem está sem
energia — então o canal de resgate é a **UART do MCU alimentado pelo USBasp** (ou
um USB-TTL 3V3 direto nos pads RX/TX do módulo, se preferir tirar o firmware da
equação); (b) **acordar o módulo antes de qualquer comando**; (c) **cobrir todos os
bauds e todos os PIOs**; (d) **verificar com multímetro, não com bipe**.

**Passo 0 — preparação.** USBasp alimentando a placa (bateria fora). Multímetro no
**gate do SOT-23 da plaquinha verde** (o pino onde chega o fio verde) contra GND.
Anotar o estado (deve estar ~0 V).

**Passo 1 — build de resgate** (sketch mínimo ou flag no chavi_fi) que, em CADA
baud de `{9600, 2400, 38400, 19200, 57600, 4800, 115200, 1200}` faz, com `\r`:
1. **Wake**: envia ~90 caracteres `A` (sem terminador), espera 100 ms, `AT`, `AT`.
2. `AT+PWRM1` (tira do auto-sleep — H1).
3. Lote anti-resíduo: `AT+TYPE0`, `AT+MODE2`, `AT+ROLE0`, `AT+IMME0`, `AT+ADTY0`, `AT+NOTI1`.
4. **PIOs — todos**: `AT+PIO41`, `AT+PIO51`, `AT+PIO71`, `AT+PIO81`, `AT+PIO91`,
   `AT+PIOA1`, `AT+PIOB1` (pular PIO6). — cobre H3.
5. Persistência ampla: **`AT+BEFCFF7`** (todos os 12 bits menos o bit3/PIO6) e
   **`AT+AFTCFFF`**.
6. `AT+NAME001FI000718` (ou 629), `AT+BAUD0`, `AT+RESET`; após 1,5 s, re-aplicar
   o passo 4-5 a 2400.
   **PROIBIDO**: `AT+RENEW`/`AT+DEFAULT` em qualquer ponto.

**Passo 2 — verificação elétrica ao vivo.** Durante o passo 1, observar o gate no
multímetro: quando ele subir (~3-4 V), anotar **qual `AT+PIOx1` e qual baud**
fizeram efeito → esse é o pino do mosfet DESTA unidade e o baud real do módulo.
(Se o gate já subir no PWRM1/wake, era só o auto-sleep — H1 confirmada.)

**Passo 3 — prova da bateria.** Tirar o USBasp, inserir SÓ a bateria: 1 bipe de
boot = trilho voltou (o BEFC religou). Verificar pelo scan que o anúncio voltou a
ter nome e aceitar conexão; conectar e `TST-PING`.

**Passo 4 — se o módulo seguir mudo/anônimo em todos os bauds** (H5):
- Opção A (recomendada p/ voltar a operar já): **neutralizar o mosfet** — jumper
  dreno-fonte na plaquinha verde (ou remover a plaquinha e emendar o fio da
  bateria). A placa vira "FI 1.0 sem mosfet": com o chavi_fi ela dorme de
  `powerDown` e acorda por PD3/data-wake. Consumo em sono um pouco maior, função
  completa. Depois trocar o módulo com calma.
- Opção B: **trocar o módulo BLE** (mesmo clone das 1.5) e re-soldar o fio verde
  do gate num PIO conhecido (padronizar PIO8) → re-provisionar com o chavi_fi
  corrigido (§9).

**Passo 5 — pós-resgate.** Regravar o chavi_fi já com as correções do §9 (sem
RENEW), `EE_BOARD=1` no seed.bin (canal 001 já cai em fi10), validar
`TST-INFO`/`TST-HIB`/abrir/fechar, e etiquetar na placa o pino do gate descoberto.

---

## 9. Mudanças recomendadas no chavi_fi (priorizadas, arquivo:linha)

1. **[CRÍTICO] Nunca mais `AT+RENEW`/`AT+DEFAULT`** — `chavi_fi.ino:427-434`
   (bleProvisionar PASSO 0). Em placa com mosfet, o RENEW corta a energia do
   próprio MCU no meio do provisionamento e apaga o BEFC que faz a placa ligar
   com bateria (§5). Nenhum firmware/esteira legado jamais precisou de factory
   reset (§4.1). Remover o passo 0 (o lote ROLE0/IMME0/ADTY0 já descontamina) —
   no mínimo, condicioná-lo a `!placa10` E a uma flag explícita de bancada.
2. **[CRÍTICO] Ampliar a máscara do trilho na placa10** — `chavi_fi.ino:386-389`.
   O gate pode estar em PIO4..PIO9 (at.js:439/455 oferece 4-7 default 4; upload
   antigo 7-9). Trocar por `AT+PIO41/51/71/81/91` + **`AT+BEFCFF7`/`AT+AFTCFFF`**
   (o atual `BEFC070` escreve 0 em PIO4/5 — pode DESLIGAR o gate de uma unidade
   4/5). No `dormir()` (`:789`), cortar também `AT+PIO40/50` além de 70/80/90.
3. **[CRÍTICO] Não pular a config do trilho quando PD3 estiver alto** —
   `chavi_fi.ino:374`. O early-return do `configModuloLeve()` com `PIN_WAKE==HIGH`
   pode suprimir exatamente os comandos que religam o trilho (PD3 flutuando/alto
   pós-RENEW). Para placa10, separar "config de dados" (pulável conectado) de
   "config de energia" (PIOs/BEFC — mandar sempre; se houver túnel, o app ignora
   texto AT).
4. **[ALTO] Acordar o módulo antes de lotes AT**: preâmbulo de wake (string longa
   + `AT+PWRM1`) no início de `bleProvisionar` e do lote de resgate — módulos em
   auto-sleep ignoram AT curto (§4.4). Hoje só há `at("AT",120)` (:430), que pode
   não bastar.
5. **[ALTO] Deixar o teste de vida honesto** — `bleResponde`/`diagBaudBipes`
   (`:243-264, 321-331`) contam **qualquer byte** como resposta → falso-positivo
   de baud (foi o que enganou o resgate). Validar conteúdo (procurar `OK`/
   `Soft AT`/`+`) antes de bipar o índice do baud.
6. **[MÉDIO] Considerar `FEATURE_HIBERNA_MOSFET=0` para placa10** enquanto o wake
   por conexão (AFTC) não for validado nas 1.0 (`chavi_fi.ino:79,789`): numa 1.0
   com módulo suspeito, o corte é um brick-até-alguém-conectar. `powerDown` +
   data-wake já entrega a economia essencial.
7. **[MÉDIO] Hall VCC**: em placa10, `pinMode(A2, OUTPUT); digitalWrite(A2, LOW)`
   no `setup()` (espelha FI_1_0.ino:625-635) — hoje o A2 fica flutuando alimentando
   o pino VCC do sensor hall. A7 é input-only (o OUTPUT do legado ali sempre foi
   inócuo); A3/A6 nunca são lidos — sensores vestigiais, nada mais a fazer.
8. **[BAIXO] Escala da bateria**: usar `4.2/1024` (legado FI_1_0.ino:134) em
   `acionar()`/`enviaBateria()` (`chavi_fi.ino:494,591`) para o número `1004.xx`
   e o indicador do app baterem com a frota.
9. **[BAIXO] STATUS6 em placa10/ver.03** (`chavi_fi.ino:397`): a linha 1.0 de
   produção nunca usou STATUS (só a FI_1_5); manter é inócuo, mas documentar que
   o caminho real de wake da 1.0 é BEFC/AFTC + data-wake.
10. **[BAIXO] Bancada**: novo teste `TST-GATE` que varre `AT+PIOx1/x0` e pergunta/
    mede o efeito — identifica e etiqueta o pino do mosfet por unidade; e o
    checklist visual (plaquinha verde no cabo da bateria) para classificar
    mosfet × sem-mosfet antes de gravar.

---

## 10. Perguntas do escopo respondidas em uma linha

- **Pinagem placa10 do chavi_fi**: correta; únicos ajustes = hall VCC A2 (§9.7) e escala da bateria (§9.8).
- **Quem segura o trilho**: a NVM do módulo BLE (`BEFC`), pino de gate variável por unidade (4..9); nada no MCU.
- **FI_1_0 sem mosfet existe** e se distingue por inspeção da plaquinha na bateria / TST-HIB / baud (9600 × 2400).
- **3º tipo de placa no firmware**: desnecessário (comandos de PIO inócuos sem mosfet + fallback powerDown).
- **readBLE1010**: parseInt timeout 1 s sem delimitador; legado respondia saltos ~3,5 s depois do desafio — folga enorme de timing p/ o chavi_fi.
- **goToSleep**: 15 s + powerDown (1.0) × 25 s + corte de trilho sem powerDown (1.0_400).
- **RENEW/DEFAULT/IMME/ADTY**: não existem em nenhum código/esteira legada — pós-RENEW é território sem mapa; o observado (anúncio anônimo não-conectável) aponta p/ auto-sleep (PWRM default) e/ou semi-brick.
- **Clock**: produção 16 MHz cristal (lfuse 0xFF); novo 8 MHz interno (0xE2) — 2400 seguro, 9600 marginal no RC; p/ regravar firmware antigo, restaurar lfuse 0xFF.
- **EEPROM**: layout 1/2/3/4/5/15/25/35/45/67/100-107/150/200-205/768/769-779 confirmado (§7).

---

## 9. Ativação do MOSFET na FI 1.0 — firmware v2.28.0 / bancada v2.29.0 (02/09/2026)

Até aqui o firmware novo mantinha o trilho da 1.0 **sempre ligado de propósito**:
`configModuloLeve` erguia todos os gates candidatos (`AT+PIO41/51/71/81/91`) com
`BEFCFF7`/`AFTCFFF`, e o `dormir()` tinha `!placa10` na condição do corte. O motivo
está no §1 deste relatório: o retrofit da 1.0 foi feito **placa a placa** e o gate
ficou em PIOs diferentes ({7,8,9} no `upload`; {4,5,6,7} na esteira `at.js`).
Presumir o pino é o caminho das 0718/0629 — placa que não liga na bateria.

**A saída não é adivinhar, é medir.** O gate agora é DESCOBERTO pelo ar, com o
mesmo mecanismo que provou o gate da CH003FI002910 em 02/08/2026: corta um pino
por vez e vê qual deles cala o MCU. O corte é auto-evidente — a placa sem energia
não responde ao `TST-PING`, e o módulo (alimentado direto da bateria, fora da
chave) continua ali para receber o comando que a religa.

### O que mudou

| Onde | O quê |
|---|---|
| `chavi_fi.ino` | `EE_GATE10` (byte **926**): gate CONFIRMADO desta unidade (4,5,7,8,9). 0/0xFF = desconhecido → não corta |
| | `dormir()`: a 1.0 entra no corte **só** com `gate10Ok()`; a 1.5 mantém a condição de sempre |
| | `TST-GATE<n>` / `TST-GATE?`: grava/consulta o gate (responde `GATE10:NA` numa 1.5) |
| | `TST-INFO`: linha `GATE10:<n>` **só** na 1.0 |
| | `TST-HIB`: na 1.0 recusa sem gate (`HIB-SEM-GATE`) e **não** zera o BEFC |
| `bancada.py` | passo **"Descobrir e ativar (FI 1.0)"**: varre 8→7→9→5 pelo ar, religa, confirma o retorno do MCU, grava com `TST-GATE` e liga a hibernação |
| | `corrigir-ar` recusa mexer nas máscaras de uma 1.0 |
| `tools/at_ar.py` | AT pelo ar sem cabo e sem bancada: lê a config, ergue pino a pino até o MCU voltar (`--achar-gate`) e restaura máscaras (`--mascaras`) |

### ⛔ O que NÃO fazer: máscara "de segurança" para a 1.0 (erro de 02/09/2026)

A primeira versão desta entrega dava à 1.0 uma máscara própria — `BEFCFF7`/
`AFTCFFF`, "todos os candidatos altos, assim o gate errado não brica". Parecia
conservador. **Quebrou a primeira placa gravada com ela** (CH001FI001000): o
módulo truncou para `3F7`/`3FF`, o power-on passou a erguer PIO3,4,5,7,9,10,11
além do 8 — pinos que ninguém garantiu estarem livres numa 1.0 — e a placa
passou a estalar, com o MCU mudo em todos os testes.

A lição que fica: **placa gravada pela bancada faz boot silencioso** (o seed
marca `EE_MOD_CFG=0xC9`, e o `configModuloLeve` — que é quem mandaria `FF7` pela
UART — nunca roda). Quem define o estado elétrico do power-on de uma placa da
esteira é a receita da bancada, e só ela. Mexer ali não é "precaução": é mudar a
eletricidade da placa sem evidência.

As duas placas voltaram a receber o mesmo par (`BEFC020`/`AFTC028`, o que a frota
inteira roda). O corte da 1.0 **não depende de máscara**: é o `AT+PIO<x>0` no
gate confirmado.

### Por que um byte novo, e não o 914

A frota 1.0 já gravada tem `914 = 8` por **default** (a UI da bancada sempre mandou
8, com ou sem mosfet). Reinterpretar aquele byte faria placas em campo começarem a
cortar sozinhas só por atualizar o firmware, sem ninguém ter conferido onde está o
gate delas. O byte 926 nasce zerado: **1.0 só corta depois de alguém provar o pino**.

### As redes de segurança que ficaram

- A varredura só **ergue** pinos (`AT+PIO<x>1`); nunca corta às cegas.
- A varredura **não grava nada** enquanto o pino não se prova, e não grava se o
  MCU não voltar depois do religamento.
- PIO6 fica fora (é o wake/PD3) e PIO4 não é testável pelo ar (em `AT+MODE2` o
  remoto só controla PIO5..11) — uma 1.0 com gate no PIO4 segue sem corte.

### O que NÃO mudou

Nada da FI 1.5. A condição do corte dela é a mesma da v2.27.0 (`!placa10` continua
verdadeiro), as máscaras `BEFC020`/`AFTC028` saem idênticas do mesmo cálculo, o
byte 926 nunca é lido numa 1.5 e a ação nova recusa qualquer placa que não se
declare `PLACA:1.0` no `TST-INFO`.
