# Firmware novo das fechaduras Chavi (setor Imobiliário) — CONTEXTO

> Documento vivo. Consolidado em 04/07/2026 a partir da análise cruzada de:
> `app-imoveis` (produção, ~1000 aparelhos), `app-tester` (bancada),
> `backend` (Laravel `api-imoveis`), o firmware antigo (`FI_1_0/1_5/*_400`) e o
> tooling de gravação (`Firmware-Antigo/src-tauri/resources`).
> **Sempre confirme contra o código atual antes de tratar algo aqui como fato.**

## Missão

Acabar com a chave física. O produto tem uma única obrigação inegociável:
**quando o app manda abrir, a porta abre — sempre.** Para esta aplicação
(imobiliária) a decisão do cliente é explícita: **confiabilidade > segurança**.
Podemos simular/burlar seeds, saltos e tokens desde que a fechadura obedeça.

## O que é a fechadura (hardware)

- **MCU:** ATmega328 (`FI_1_0`) ou ATmega328PB (`FI_1_5`), 16 MHz cristal externo,
  core **MiniCore** (`MiniCore:avr:328`). Fuses de produção: `lfuse=0xFF hfuse=0xD7
  efuse=0xF7 lock=0xCF`. O `hfuse=0xD7` liga **EESAVE** → a EEPROM sobrevive a um
  chip-erase (importante para o provisionamento).
- **Rádio BLE:** módulo serial AT tipo **HM-10 / MLT-BT05 / "Soft AT 5.2"**
  (chamado "BLE1010" no código). Fala UART com o MCU via `SoftwareSerial`
  (RX=PD4, TX=PD5). Expõe **serviço `FFE0`, característica `FFE1`** (write sem
  resposta + notify). É o módulo que faz o advertising e acorda o MCU por um pino.
- **Motor:** ponte H simples, 2 pinos de sentido (`PB1`/`PB2`). Sem fim-de-curso;
  o batente é detectado por **corrente** via **INA219** (I²C, `0x45`).
- **Periféricos:** 3 LEDs WS2812 (`PB3`), buzzer (`PD6`), botão (`PD2`/INT0),
  pino de wake vindo do módulo BLE (`PD3`/INT1), ADC de bateria (`PC1`) e de 5V (`PC0`).
- **Energia:** bateria + `LowPower.powerDown(SLEEP_FOREVER)`. A fechadura fica
  dormindo e só acorda por: botão, ou o módulo BLE subindo o pino de wake quando
  um celular conecta.

## O protocolo (contrato que o firmware novo DEVE honrar)

Este é o contrato que os ~1000 apps em campo já falam. O firmware novo tem que
casar byte a byte, senão quebra a base instalada.

### Camada BLE
- Serviço **`FFE0`**, característica **`FFE1`** (UUID curto de 16 bits).
- `FFE1`: o app **escreve** com `writeWithoutResponse` e **assina notify**.
- **Sem bonding/pareamento** (`AT+TYPE0`). O app nunca pareia.
- **Nome do advertising:**
  - Fechadura **provisionada** → o **serial** sem o "CH" (ex.: `003FI002465`).
    O app procura por esse nome (`removeCH(serial)`).
  - Fechadura **de fábrica** (ainda sem seeds) → `CHAVIFI` / `CHAVIFIPR`
    (o `app-tester` procura por esses nomes para cadastrar).
- O firmware antigo **não** anuncia o serviço FFE0 no advertisement, só o nome.
  O app tolera isso (casa por nome). O novo pode anunciar FFE0 também (opcional).

### Handshake de acionamento (desafio-resposta com LFSR)
Sequência exata (lado app, de `app-imoveis/lib/custom_code/actions/`):
1. App conecta, o módulo acorda o MCU (borda no pino de wake).
2. App envia **um número aleatório `N`** (string decimal, `0..1_999_999`) — 1 write.
3. Fechadura responde **DOIS "saltos"**, um por notificação/linha:
   `resposta_k = randFW + N + seed_k`  (k = 1,2), onde `randFW = random(0,9999)`.
   O app recupera `salto_k = resposta_k − N − seed_k` (= `randFW`).
4. App gera 2 tokens via **LFSR de 32 bits** a partir das seeds e envia
   **3 writes separados, sem terminador**: `tokenA`, `tokenB`, `comando`.
   - `comando`: no **app-imoveis** `abrir="2"`, `fechar="1"`.
     No **app-tester** é o inverso (`abrir="1"`, `fechar="2"`).
     ⚠️ O sentido físico real depende da **calibração** gravada na fechadura
     (`calibrationOk`), não do texto do comando. Ver "Pegadinha da calibração".
5. Fechadura confirma respondendo **uma linha numérica**: `1000+bateria` (ex.
   `1004.12`) ou `2000+bateria` (`2004.13`). O app aceita qualquer
   `^\d{2,}(\.\d+)?$`. A string `"0"`/`"00"` é **recusa** (nunca use para OK).

**LFSR do token (idêntico nos 3 lugares — app, tester, firmware, `calibracao.py`):**
```
a = seed & 0xFFFFFFFF
repita 'salto' vezes:
    y0 = bit31(a); y1 = bit21(a); y2 = bit1(a); y3 = bit0(a)
    a = ((a << 1) | (y0 ^ y1 ^ y2 ^ y3)) & 0xFFFFFFFF
token = a
```
Taps **31, 21, 1, 0**. `salto` = o `randFW` que a própria fechadura sorteou e
embutiu na resposta do passo 3. Como a fechadura conhece `randFW`, ela recomputa
os mesmos tokens e compara. **Convenção de sinal: `resposta = seed + N + salto`.**

- **Abrir/fechar** usam tokens de `seed[0]`/`seed[1]`.
- **Setup/calibração/reset** usam tokens de `seed[2]`/`seed[3]` + um código:
  - `150993` = configurar flags (recebe `chavi:XXXXXXXX`).
  - `190720` = calibrar (recebe `CALIBRACAO-FI`, depois `PORTA-ABERTA`/`PORTA-FECHADA`).
  - `140197` = apagar seeds (volta ao modo setup).

### Seeds (a origem da verdade)
- Determinísticas do serial: `seed_k = int(sha256(serial + "CHAVI" + k)[:8], 16) % 429496729`.
  (⚠️ o `% 429496729` é ~1/10 de 2³², é um bug histórico já em produção — **manter**.)
- Backend (`DeviceSeedHelper.php`), gerador de bancada (`seedGenerator.py`) e app
  calculam **as mesmas 4 seeds**. O `SEED_SECRET` está hardcoded como `"CHAVI"`.
- No firmware as seeds ficam na **EEPROM** (little-endian) em `5,15,25,35`.

### QR codes
- QR da fechadura **e** da bateria = uma **URL**:
  `https://chavi.com.br/bemvindo/CH003FI002846`. Todos extraem o serial pelo
  último segmento do path. Serial: `CH{GGG}FI{BBB}{NNN}`. Bateria correspondente:
  `CH {GGG} BAT {BBB}.{NNN}`.

## Por que o firmware ANTIGO falha (causas-raiz, não sintomas)

Os problemas relatados (não acha fechadura, acha e não abre, wake ruim, lentidão,
cadastro burocrático, EEPROM no escuro, watchdog suspeito) rastreiam para:

1. **Baud desalinhado módulo↔MCU.** O firmware fala `AT+BAUD2`, a bancada fala
   `AT+BAUD0`; nos clones "Soft AT 5.2" a tabela de baud difere do HM-10 genuíno.
   Resultado medido em campo: módulos em 2400/38400 quando o MCU espera 9600 →
   **UART vira lixo → MCU fica mudo → falha em tudo** (erros F05/F07 no app).
   *(Fix parcial de hoje: `sincronizarBaudBLE`. No novo firmware isso é nativo e
   roda em todo boot.)*
2. **Config do módulo perdida.** Fechadura já provisionada não reexecutava
   `setupBLE01`, então perdia `TYPE0`/wake/`BEFC`/`AFTC` e passava a pedir
   pareamento ou não acordar o MCU. *(Fix de hoje: `ensureModuleConfig` no boot.)*
3. **Desgaste de EEPROM.** `savePinState()` grava **6 bytes de EEPROM a cada ciclo
   de sono**, e `restorePinState()` roda **dentro da ISR**. A 100k escritas/célula,
   uma fechadura muito usada degrada a EEPROM em meses — e a EEPROM guarda as
   seeds. Isso explica "controle da EEPROM no escuro" e parte do descompasso.
4. **`FastLED.show()` mata as interrupções** (protocolo WS2812 é bit-bang com IRQ
   desligada). Se um LED anima enquanto o `SoftwareSerial` recebe, **bytes BLE são
   corrompidos**. O antigo evita por disciplina, mas é frágil.
5. **`SLEEP_FOREVER` + wake só por pino do módulo.** Se a config do módulo drifta
   (item 2), o pino de wake nunca sobe e a fechadura fica **inacessível para
   sempre** — só reset físico/troca de bateria resolve. Nenhum watchdog salva
   porque o WDT é desligado no `powerDown`.
6. **`funcReset` = salto para o endereço 0.** Não reseta periféricos nem
   registradores (I²C, timers, módulo BLE ficam em estado sujo). Não é um reset
   de verdade. O correto é reset por watchdog.
7. **`random()` sem `randomSeed()`** → a mesma sequência "aleatória" após cada
   reset (marcado como FIXME no próprio código).
8. **Serial compilado no flash** (`SerialNumber.h` gerado por dispositivo) → cada
   fechadura exige **uma compilação própria**. É a origem do "cadastro lento e
   burocrático" e do "AT manual".
9. **Reset total de EEPROM no 1º boot** (`for i: EEPROM.update(i,0)`), apagando
   inclusive o que a bancada acabou de gravar em alguns fluxos.

> **Por que os patches de hoje não fecharam 100%:** eles tratam 1, 2 e o bypass de
> token, mas os itens 3–9 continuam de pé no firmware antigo. Enquanto a EEPROM se
> desgasta, o `FastLED` corrompe UART e o `SLEEP_FOREVER` pode enforcar a
> fechadura, as inconsistências reaparecem de forma intermitente — que é
> exatamente o sintoma "às vezes funciona, às vezes não". A saída é o firmware
> novo, que remove as causas.

## O que o firmware NOVO faz diferente (decisões de projeto)

Pasta: `firmware/chavi_fi/`. Um único `.ino` + headers, MiniCore 328/328PB.

1. **Protocolo 100% compatível** — mesmo FFE0/FFE1, mesmo handshake, **mesmo LFSR
   (taps 31/21/1/0)**, mesma convenção `resposta = seed+N+salto`, mesmos códigos
   `150993/190720/140197`, mesma resposta `1000/2000+bateria`. Nada muda para os
   ~1000 apps em campo. (`protocolo.h`)
2. **Bypass de token para abrir/fechar** (decisão do cliente): a fechadura ainda
   **responde** os saltos (o app precisa deles para completar o handshake), mas
   **não valida** os tokens no abrir/fechar — aciona direto pelo comando `1`/`2`.
   Qualquer app/seed abre. Elimina de vez a falha por "seed divergente".
   Setup/calibração/reset **mantêm** validação (não são disparados pelo usuário final).
3. **Auto-cura de baud e de config no boot.** Varre baud candidatos, força o
   módulo para 9600 conhecido, e reaplica `TYPE0/MODE2/ROLE0/BAUD/DELI/NOTI/BEFC/
   AFTC/PIO` em **todo** boot. Idempotente. (`ble_modulo.h`)
4. **Sono seguro + wake redundante.** Continua usando `powerDown`, mas:
   (a) **não grava EEPROM ao dormir** (estado dos pinos vai para RAM);
   (b) além do wake por pino do módulo, um **watchdog periódico** acorda o MCU de
   tempos em tempos para reafirmar a config do módulo — assim, mesmo que o pino de
   wake falhe, a fechadura **nunca fica permanentemente inacessível**.
5. **Watchdog de verdade.** WDT armado durante operação (motor/handshake) com
   timeout generoso; `wdt_reset()` nos laços; **reset real** por
   `wdt_enable(WDTO_15MS); while(1);` no lugar do salto-para-zero. Desligado só na
   janela de `powerDown`. Documentado — sem "não sabemos se o watchdog atrapalha".
6. **EEPROM com telemetria e sem desgaste.** Mapa documentado (`eeprom_map.h`),
   **contador de boots** e **contador de aberturas** (escrita esparsa, com
   `EEPROM.update` que só grava se mudou), **byte de versão de layout**, e helpers
   que dizem o quão perto do limite de escrita a célula está. Fim do "no escuro".
7. **Um único `.hex` universal — sem recompilar por dispositivo.** O serial vem da
   **EEPROM** (endereços 769–779, onde o `seedGenerator.py` já grava). No boot o
   firmware lê o serial e monta `AT+NAME<serial>` em runtime. Se a região do
   serial estiver vazia, cai para `CHAVIFI` (modo fábrica) ou nome derivado do MAC.
   → Gravar uma fechadura vira: flashar o **mesmo** `.hex` + um `seed.bin` (rápido).
8. **`FastLED` nunca durante RX BLE.** Feedback visual só antes/depois da troca
   BLE, em janelas onde a UART está ociosa. Documentado como invariante.
9. **`randomSeed()` no boot** a partir de ruído de ADC + `micros()`.
10. **Motor com garantia de parada.** Detecção de batente por INA219 **com timeout
    duro** — o motor sempre para, nunca trava ligado. Lógica de "2 voltas"/delta
    simplificada para não poder enforcar o laço.

## Mapa de EEPROM (compatível com `seedGenerator.py`)

| Endereço | Símbolo | Descrição | Tipo |
|---|---|---|---|
| 1 | `setupSeedOk` | 0x01 = já tem seeds (pula modo setup) | u8 |
| 2 | `generalSetupOk` | setup geral concluído | u8 |
| 3 | `calibrationOk` | sentido de giro calibrado (0/1) | u8 |
| 4 | `verifierCalibration` | calibração validada | u8 |
| 5–8 | `seed01` | seed 1 | u32 LE |
| 15–18 | `seed02` | seed 2 | u32 LE |
| 25–28 | `seed03` | seed 3 | u32 LE |
| 35–38 | `seed04` | seed 4 | u32 LE |
| 45 | `mac` | marcador de MAC | — |
| 100–107 | flags | proximidade, som, luz, 2voltas, botão, auto-fecho, avisos | u8 x8 |
| 150 | `setupProductionOk` | passou pela produção | u8 |
| 768 | `versBLE` | versão do módulo BLE detectada | u8 |
| 769–779 | serial | serial sem "CH" (11 chars), ex. `003FI002465` | string |
| **900** | `fwLayoutVersion` | **NOVO** — versão do layout de EEPROM deste firmware | u8 |
| **901–904** | `bootCount` | **NOVO** — nº de boots (telemetria de saúde) | u32 LE |
| **905–908** | `openCount` | **NOVO** — nº de aberturas (telemetria) | u32 LE |

> Endereços novos (≥900) ficam bem longe da região usada pelo `seedGenerator.py` e
> das flags, para nunca colidir com o provisionamento existente.

## Gravação / provisionamento novo (proposto)

- **Compilar uma vez** o `chavi_fi.ino` → `chavi_fi.hex` **universal** (serve para
  toda fechadura, sem `SerialNumber.h`).
- **Por dispositivo:** `tools/gerar_seed.py CH003FI002846` gera um `seed.bin`
  (imagem de EEPROM de 1024 bytes) com serial + 4 seeds, **igual** ao formato atual.
- **Gravar:** `tools/gravar.sh CH003FI002846` → avrdude USBasp grava fuses + lock +
  o `.hex` universal + o `seed.bin`. Um comando, sem recompilar, sem AT manual (o
  firmware autoconfigura o módulo no 1º boot).

## Pegadinha da calibração (sentido de giro)

O sentido físico de "abrir" = função de (`comando`, `calibrationOk`). Como
`app-imoveis` manda `abrir="2"` e `app-tester` manda `abrir="1"`, o mesmo comando
gira em sentidos opostos entre os dois apps. Isso **não é bug do firmware** — o
passo de calibração por fechadura (código `190720` + `PORTA-ABERTA/FECHADA`) grava
`calibrationOk` de modo que "abrir" no app de produção corresponda ao giro certo. O
firmware novo preserva a tabela do `FI_1_5` exatamente:

| `comando` | `calibrationOk` | sentido | status enviado |
|---|---|---|---|
| 1 | 0 | motor02 | 2000+bat |
| 1 | 1 | motor01 | 1000+bat |
| 2 | 0 | motor01 | 1000+bat |
| 2 | 1 | motor02 | 2000+bat |

## Estado / próximos passos

- [x] Análise cruzada dos 4 projetos + firmware + tooling.
- [x] `CONTEXTO.md` (este arquivo).
- [x] `firmware/chavi_fi/` — firmware novo escrito.
- [x] Compilar com MiniCore: **OK, 66% flash (21.746 B) / 36% RAM (754 B)** —
      contra 91%/57% do antigo. Sem warnings no nosso código.
- [x] Gerador de seed conferido byte a byte contra o `seed.bin` legado (mesmas
      4 seeds + serial). LFSR conferido contra o `generate_token_with_salts.dart`.
- [ ] Bancada: gravar 1 fechadura piloto com `tools/gravar.sh`, validar scan +
      abrir/fechar pelo `app-tester` e pelo `app-imoveis` real.
- [ ] Medir latência de wake→abertura e ajustar `PWRM`/janela de conexão.
- [ ] Rollout gradual (algumas fechaduras) antes de generalizar.

## Sessão de bancada 04/07/2026 — 1º teste real (F07)

Fechadura gravada com o firmware novo. App-imoveis: instalou OK, clicou Calibrar,
"fechadura localizada", travou e deu **F07** (firmware mudo após o desafio). O
**botão físico gira o motor** → MCU vivo, motor OK, `powerDown`+wake por
interrupção OK (botão é INT0). Logo, **o problema é só o caminho BLE**: o módulo
não está subindo o pino de wake (INT1/PD3) quando o celular conecta → o MCU não
acorda → fica mudo → F07.

**Hipótese principal:** os comandos `BEFC/AFTC/PIO` que configuram o pino de wake
só passam a valer após um `AT+RESET` (o FI_1_5 de campo sempre reseta; o novo não
resetava). **Correção aplicada:** `bleResetAplicar()` no boot após configurar.

**Instrumentação p/ diagnóstico sem cabo serial** (`FEATURE_DIAG_FEEDBACK`): a
fechadura bipa 1x ao acordar por BLE, 2x ao receber o desafio, 3x ao responder os
saltos. O padrão de bipes localiza a etapa exata que quebra (ver config.h).

**Dummy total aplicado** (`FEATURE_DUMMY_TOTAL`): calibração e setup também
deixaram de validar token (antes só abrir/fechar eram dummy).

## Documentos do Drive da Chavi analisados (04/07/2026)

- **"Chavi - Protocolo detalhado.pdf"** e **"Chavi - protocolo de comunicação.pdf"**
  → descrevem a arquitetura **FUTURA, baseada em ESP32** (MQTT + JSON + JWT +
  CRC32, `deviceId:"esp32_001"`, GATT service `6F0A0001-...`, characteristics
  `chavi-command`/`chavi-status`, 4 fluxos: MQTT/HTTP/BLE/SDK). **NÃO é** o
  protocolo da FI atual (ATmega328, texto puro + saltos/LFSR sobre FFE1). É o
  norte de uma FI 2.0 / linha EI-CI — guardar para a reescrita futura, não usar no
  firmware atual. Confirmam o conceito `jumps(seed, jumpCount)` = nossos saltos/LFSR.
- **"Como fazer o upload de firmware - CHAVI.docx"** → confirma **USBASP +
  ATmega328P** (FI e EI), gravam o binário "with_bootloader" via **Progisp v1.72**
  (GUI chinês) com driver Zadig. Nosso `gravar.sh` faz o mesmo por avrdude direto
  (sem bootloader) — mais simples e já validado em campo (o teste do Leonardo).
- **Users-Manual-3390377.pdf (ESP-32S)** e **wifi-ble.bin** → material da linha
  ESP32, não da FI. O MCU da FI é ATmega328P (confirmado: o firmware AVR roda nela).

## Correção baud-agnóstica (04/07/2026)

Após o teste dar "1 bipe só" (acordou, mas dados não chegam), a causa foi o
`bleEnsureConfig` mandar `AT+BAUD2` às cegas — que em alguns clones é 38400, não
9600 → derrubava o baud do módulo. **Correção definitiva:** o firmware NUNCA
reprograma o baud do rádio. Ele **descobre** em que baud o módulo está
(`bleSyncBaud`) e passa a falar nesse baud (`g_baudModulo`). Elimina a armadilha
`AT+BAUD0` vs `AT+BAUD2` de vez, para qualquer módulo.

## Sessão 04/07 (cont.) — F01 e a virada para debug real

2º teste: zero beeps + **F01** (erro de conexão) — regressão vs. o F07 anterior.
Diagnóstico: o módulo BLE guarda config em NVM, e o vai-e-vem de `AT+BAUD2`+RESET
entre gravações deixou o módulo num estado bagunçado (provável travado em 38400 +
config inconsistente) → nem conecta.

**Virada de estratégia:** parar de adivinhar. (1) `bleProvisionar()` robusto:
descobre baud → aplica config FI_1_5 → reset → RE-descobre baud (desatola qualquer
estado). (2) **Bipe de BOOT** (no power-on, antes do app): 2 agudos = módulo
respondeu AT / 4 graves = módulo mudo. (3) **Debug serial real** habilitado
(`FEATURE_SERIAL_DEBUG`, hardware UART PD0/PD1, 115200) — o Leonardo tem cabo
USB-TTL. (4) Recomendado **nRF Connect** (app) para testar conectividade sem cabo.

## Sessão 04/07 (noite) — módulo funciona, baud errado, e o bloqueio do ISP

Descobertas grandes desta rodada:
- **O "tudo morto" era falta de energia:** o USBASP NÃO alimenta o MCU nessa placa;
  sem bateria, nada roda (nem botão, nem beep, nem serial). Bateria DENTRO para
  rodar; para gravar, o USBASP alimenta com a bateria FORA (mas a corrente é
  marginal p/ o write — ver abaixo).
- **O módulo BLE FUNCIONA:** ao clicar "calibrar", a fechadura acorda e recebe
  bytes (deu "1 beep longo" = byte cru chegou na UART). Ou seja, anuncia, conecta
  e passa dados. O "módulo mudo" (4 beeps no boot) é FALSO — o meu teste AT falhava.
- **Causa do handshake não fechar = BAUD errado.** Os dados chegam mas como lixo
  (`parseInt`→0, sem o "2 beeps" de desafio lido). O `bleDescobrirBaud` reportava
  mudo e caía no default 9600, mas o módulo está noutro baud.
- **Por que o teste AT falhava:** módulo em `AT+PWRM1` (economia) **dormindo** — o
  1º `AT` só acorda, não responde. **Fix:** `bleResponde()` agora manda AT 4× (o 1º
  acorda, os seguintes respondem). E o **beep de boot agora conta o índice do baud
  detectado** (1=9600 2=38400 3=19200 4=2400 5=57600 6=4800) — dá pra saber o baud
  por ouvido, sem serial. Compilado (70% flash), **falta gravar**.
- **Serial de debug (USB-TTL) não ajudou** nesta placa: o pad "TX" que foi ligado
  dava fluxo contínuo de `0x00` (linha presa em baixo = pad errado / sem contato).
  A placa tem header serial (DTR/RX/TX) mas achar o pad certo é difícil sem foto.

**BLOQUEIO ATUAL = contato físico do ISP.** O USBASP engata ~1 em 400 tentativas
(contato frouxo, provavelmente dupont em pad SMD). Quando engata com bateria fora,
o write falha por energia marginal (`device 0x00 != input`). Precisa de **contato
ISP sólido** (soldar fios, clipe/pogo, ou header 2x3 firme) + energia estável.
Assim que houver 1 gravação boa: contar os beeps de boot (= baud) e clicar
calibrar/abrir (o motor girar é o teste final, sem serial).

## Auto-baud POR DADOS (05/07) — a virada

Descoberto: a detecção de baud por **AT dá falso-positivo** (o módulo "respondeu OK"
a 2400, mas os dados reais vinham noutro baud → lixo → F05/handshake não fecha). O
beep de boot mostrava idx=4 (2400) e a fechadura só dava "1 longo" (byte cru) sem
ler o desafio.

**Solução:** o baud passa a ser achado validando o **DESAFIO do app** (número
decimal em 0..1999999), não por AT:
- `BAUD_CANDIDATOS[] = {9600,38400,19200,2400,57600,4800}`, índice na EEPROM
  (`EE_DATA_BAUD_IDX=909`).
- boot: usa o baud salvo, configura o módulo NESSE baud (`bleConfigurarNoBaud`,
  tolerante), e **bipa o índice** (1=9600...6=4800).
- `atenderApp`: se o desafio não vira número válido em 4s (lixo) → **avança pro
  próximo baud** (salva na EEPROM, aplica na hora). Se o handshake fecha (4 números
  + comando conhecido) → **trava o baud** na EEPROM.
- Converge em poucos cliques de calibrar/abrir. O F05 do framing já foi corrigido
  (2 saltos numa linha "respA respB\n"). Compilado (70%), pronto em `bin/`.

Teste: reflashar → clicar calibrar/abrir várias vezes; o beep de boot muda de
índice a cada tentativa falha até TRAVAR e o motor girar.

## ✅ 05/07 — FUNCIONOU: o app abre a fechadura!

Na placa CH003FI002585, com o firmware novo: clicar **ABRIR** no app → handshake
completo (beeps "2 agudos + 3 agudos") → **o motor GIROU**. Objetivo central
atingido: a fechadura obedece o comando do app.

O que destravou (a cadeia de bugs, em ordem):
1. Energia — bateria SEMPRE dentro (USBASP não alimenta o MCU).
2. Gravação — contato ISP firme + bateria dentro; write precisa de segundos parados.
3. Baud — achado pelos DADOS (desafio do app), não por AT (que dava falso-positivo).
   Travado na EEPROM (`EE_DATA_BAUD_IDX`). Convergiu em ~4 cliques de calibrar.
4. Framing — 2 saltos numa linha só ("respA respB\n") mata o F05.

**Falta (acabamento, não bloqueia o abrir):**
- Calibração: falhou ("não foi possível calibrá-la") — o fluxo multi-passo
  (CALIBRACAO-FI → PORTA-ABERTA/FECHADA) precisa casar melhor com o app. Serve só
  p/ definir o SENTIDO de giro (qual lado é abrir).
- Testar FECHAR.
- Produção: desligar `FEATURE_DIAG_FEEDBACK` e `FEATURE_SERIAL_DEBUG`; opcional
  pular a reconfig do módulo todo boot (baud já travado) p/ acelerar o boot.

## BAUD DO MÓDULO = 2400 (FIXO, definitivo — 05/07)

Lido direto da EEPROM da 1ª fechadura (byte 909 = índice 3 na ordem antiga =
**2400**). A descoberta automática de baud provou-se FRÁGIL: um tropeço no baud
certo fazia ela ciclar pro errado e travar lá (F07 permanente). Removida. Agora:
`#define BAUD_MODULO_FIXO 2400` — sem descoberta, sem ciclo, sem EEPROM de baud.
Simples e determinístico (como os seeds dummy). Se aparecer módulo com outro baud,
muda só esse número. Boot = 1 bipe agudo.

## Sessão 05/07 (retomada) — firmware bypass enxuto + GUI de bancada

Objetivo: fechar o firmware para montar **300 fechaduras** com confiança. Duas
frentes entregues.

### Firmware (`chavi_fi/chavi_fi.ino`) reescrito, bypass total, 2400 fixo
- **Tudo a 2400 baud, FIXO.** Zero descoberta dinâmica (era frágil e travava).
  `#define BAUD_MODULO 2400`. O firmware **NUNCA** manda `AT+BAUD`. No boot ele
  só VERIFICA se o módulo responde `AT` a 2400 (`bleResponde`, manda AT 4× pois
  o 1º só acorda o `PWRM1`) e, se não responder, dá **4 beeps graves** (triagem
  de módulo na bancada). Módulo OK = nenhum beep extra além do "estou vivo".
- **Bypass total:** abrir/fechar/calibrar não validam token. O comando é
  reconhecido em QUALQUER posição pós-desafio (robusto a token perdido/colado —
  32 bits, colisão com 1/2/190720 é desprezível). A fechadura só responde os 2
  saltos para o app completar o handshake.
- **Causa-raiz da calibração que travava ontem (CALIBRACAOERROR):** o app
  (`calibrarpt1`) escreve `[tokenA,tokenB,190720]`, **espera 1000ms** e SÓ ENTÃO
  arma o listener de notificação (`receiveNotificationWithTimeout` cria o próprio
  listener; o stream não tem histórico). O firmware antigo respondia "11" na
  hora → caía no vão dos 1000ms → perdido → erro. **Fix:** as respostas "11" da
  calibração são seguradas (`calibAceitar` 1150ms; `calibGirar` usa o próprio
  giro de 1s + 300ms) e enviadas **em dobro** (o app espera 2 notificações — com
  2 linhas fecha na hora em vez de esperar o timeout de 5s). `PORTA-ABERTA/
  FECHADA` não precisam de resposta (o app só escreve e espera o motor).
- **Motor:** tempo fixo (`MOTOR_MS` 1000), sem INA219 (não trava ligado nunca).
  Tabela de sentido do FI_1_5 preservada (`calibrationOk` decide o lado).
- **Comandos de bancada (texto via FFE1):** `TST-PING`→`PONG`, `TST-BUZ`,
  `TST-LED` (WS2812 R/G/B/branco), `TST-MOT1`/`TST-MOT2` (giro A/B),
  `TST-BAT`→`BAT:x.xx`, `TST-INFO` (serial/calib/seeds/boots/módulo/versão),
  `TST-ALL`. FastLED só roda nesses testes (UART ociosa) — nunca durante RX BLE.
- **Telemetria:** contador de boots na EEPROM 901 (escrita esparsa, só no boot).
- Compila **49% flash (16.148 B) / 36% RAM (740 B)** no MiniCore 328 @ 8MHz int.

### GUI de bancada (`tools/bancada.py` + `tools/bancada.sh`)
Estação gráfica única (Tkinter) para montar as 300, com log verboso. Abra por
**`./tools/bancada.sh`** (cria o venv com o python do SISTEMA — o único com
tkinter no macOS — e instala `bleak`; garante `arduino-cli`/`avrdude` no PATH).

Quatro blocos, todos logando ao vivo (com "Salvar .log"):
1. **Gravação (cabo/USBasp):** Detectar (lê signature), Compilar (.hex
   universal), **GRAVAR** (gera `seed.bin` do serial + fuses `E2/D7/F7` + lock +
   eeprom + flash num avrdude só), Validar (relê flash e EEPROM do chip e
   **compara serial + as 4 seeds** com o esperado).
2. **BLE:** Escanear (marca fechaduras por nome=serial-sem-CH, FFE0 ou CHAVIFI),
   Conectar/Desconectar.
3. **Testes de componentes:** botões Ping/Buzzer/LEDs/Motor A/Motor B/Bateria/
   Info/**TESTAR TUDO** → mandam os `TST-*` e conferem a resposta.
4. **Acionamento real:** ABRIR/FECHAR/CALIBRAR executam o **handshake idêntico
   ao app** (desafio → 2 saltos → tokens LFSR reais → comando). Seeds e LFSR
   conferidos byte a byte contra o `seed.bin` legado da CH003FI002585.

> Para gravar continua valendo: **bateria DENTRO** (o USBasp não alimenta o MCU
> nessa placa) e **contato ISP firme** (header 2x3, não dupont solto).

## Sessão 05/07 (noite) — testes por CABO + wizard + cadastro no backend

Reformulação a pedido do Leonardo: a bancada tinha que ser um **assistente para
leigo** montar 300, e os testes rodarem **por cabo** (determinístico, sem
depender de parear BLE entre dezenas de fechaduras anunciando juntas).

### Firmware: modo bancada pela UART de hardware (PD0/PD1) — `chavi_fi.ino`
- Novo `Stream* io`: as respostas saem pelo canal ativo — **BLE** (app em campo)
  ou **cabo/Serial** (GUI de bancada). Nada mudou no caminho BLE já validado.
- **`bancadaCabo()`** roda **só no 1º boot** (logo após gravar): abre a UART
  hardware a **2400** e espera atividade por ~6s. Se a GUI mandar algo, entra em
  `loopBancadaCabo()` (fica servindo verbos até `FIM-BANCADA` ou 180s ocioso);
  sem cabo, segue o fluxo normal (dorme). No campo, boot é raro → custo ~6s/boot.
- **Verbos por cabo (texto, sem handshake — bancada é confiável):** `PING`→`PONG`,
  `TST-BUZ/TST-LED/TST-BAT/TST-INFO`, `MOT1`/`MOT2` (giro A/B), `ABRIR`/`FECHAR`,
  `CAL-ABERTO`/`CAL-FECHADO` (grava o sentido), `FIM-BANCADA`. Cada comando ecoa
  `ECO:<cmd>` p/ a GUI casar. `bluetooth.stopListening()` no modo cabo evita o
  SoftwareSerial roubar IRQ da UART hardware. 2 bipes agudos = entrou em bancada.
- Compila **56% flash (18.386 B) / 50% RAM (1.029 B)**.
- **Cabos:** grava pelo **header ISP 2x3** (USBasp); testa pelo **header serial
  DTR/RX/TX** (USB-TTL, GND/RX/TX a 2400). Os dois podem ficar plugados juntos.

### Assistente virou APP WEB LOCAL — `tools/bancada.py`
⚠️ **Tkinter foi abandonado:** o Tk 8.5 do sistema (macOS, deprecado) **não
renderiza `bg`/`fg` de Label/Frame em Dark Mode** — a tela ficava preta com só os
botões visíveis. Solução: `bancada.py` agora sobe um **servidor local**
(`http.server`, stdlib) e abre a interface no **navegador** (HTML/CSS/JS
embutido). Renderiza perfeito, bonito, sem depender de Tk. Logs em tempo real via
**SSE** (`/events`). Endpoints: `/api/seeds`, `/api/step`, `/api/test`,
`/api/finalize`, `/api/login/{generate,verify}`, `/api/state`.

Assistente passo-a-passo (no navegador), um leigo consegue:
- **Tela 1 — serial com máscara fixa** `CH [xxx] FI [xxxxxx]` (3 + 6 dígitos, só
  números). Mostra o serial montado + as 4 seeds em tempo real. Botão PRÓXIMO.
- **Tela 2 — passos grandes com ✓/✗** na ordem: (1) **Gravar firmware**
  (avrdude), (2) **Validar** (relê EEPROM e confere serial + seeds), (3)
  **Conectar no cabo** (auto-detecta o USB-TTL, `PING` até `PONG`), (4)
  **Auto-teste** (buzzer, LEDs, motor A/B, bateria, e módulo BLE via `TST-INFO`
  → `MOD:OK`), (5) **Cadastrar no sistema**, e **FINALIZAR** (limpa e já
  pré-preenche o próximo serial, +1). Linha de testes por componente avulso.
- **Cadastro no backend:** `POST v2/api/admin/devices` com
  `{serial_number, name, version:"000", device_type_id:1}` (FI), **só o serial,
  sem vínculo** (nasce no "Estoque Chavi"). Auth por **OTP WhatsApp**: diálogo
  pede o telefone do admin → `POST /otp/generate` → pede o código → `POST
  /loginotp` → guarda o token em `tools/.bancada.json`. 409 = já cadastrada (ok).
  ⚠️ O telefone tem que ser de um usuário **admin** (`role_id=1`) no backend.
- Deps novas no venv: `pyserial`, `requests` (o `bancada.sh` instala). BLE saiu
  do fluxo (tudo por cabo); as funções de seed/LFSR continuam no arquivo.

## Sessão 05/07 (madrugada) — teste por BLE (o cabo não vingou nessa placa)

Tentamos testar por **cabo** (UART hardware PD0/PD1). Diagnóstico com
`tools/diag_cabo.py` (a fechadura passou a **anunciar `CHAVI-FI-BOOT` sozinha** no
boot p/ isolar o problema): mesmo com o anúncio proativo, **nada de texto limpo**
chegou no USB-TTL — só `00`/`FF` soltos (ruído de linha flutuando). Conclusão: os
**pads do UART (PD0/PD1) não são acessíveis/óbvios nessa placa** (já era um
problema conhecido: "achar o pad certo é difícil sem foto"). Com 300 pra montar,
caçar pad em cada uma é inviável.

**Virada: teste por BLE** — que já é PROVADO nessa placa (é como o app abre). Zero
fio, Bluetooth nativo do Mac, e o firmware já aceita os `TST-*` por BLE
(`atenderApp`, mesmo caminho do app). Mudanças:
- **Firmware:** modo cabo de bancada **desligado** (`#define FEATURE_CABO_BANCADA
  0`) — evita que um PD0 solto pegando ruído trave o boot 180s e cegue o BLE. O
  código do cabo fica guardado atrás do `#define` p/ uma placa futura com pads
  bons. Flash caiu p/ **51%** (LTO removeu o morto). Grava igual (USBasp).
- **Bancada (`tools/bancada.py`):** o passo 3 virou **"Conectar (BLE)"** — usa
  `bleak`, escaneia pelo nome do advertising (serial sem "CH", ex.
  `003FI002585`, ou `CHAVIFI`), conecta (acorda o MCU) e dá `TST-PING`→`PONG`. O
  auto-teste e os testes por componente mandam `TST-BUZ/TST-LED/TST-MOT1/
  TST-MOT2/TST-BAT/TST-INFO` por FFE1 e conferem a resposta. `diag_cabo.py`
  continua no repo p/ diagnóstico de cabo, mas não é mais o caminho.
- ⚠️ 1ª vez: o macOS pede **permissão de Bluetooth** pro Terminal (Ajustes ▸
  Privacidade ▸ Bluetooth). A fechadura anuncia ao ACORDAR — se o scan não achar,
  aperte o botão físico / religue a bateria.

## Sessão 05/07 — F05 no calibrar do INSTALL (corrigido no firmware, sem tocar o app)

**Sintoma:** no app-imoveis, ao INSTALAR (unidade → admin → instalar → lê os 2 QR
→ conecta → **Calibrar dá F05**). Fechando e reabrindo o app, na mesma unidade já
instalada, o Calibrar **funciona**.

**Causa raiz (mapeada no app, sem alterá-lo):**
- `calibrarpt1` é **single-shot**: perfil único `nova` (timeout do desafio **3s**),
  `maxTentativas:1` (sem retransmitir), `maxRodadas:1` (sem wake-retry). Já
  `abrir`/`fechar` têm perfil legado 6s + retransmissão + 3 rodadas.
- O app espera **2 notificações** (`expectedNotifications:2`) para os saltos. Nosso
  firmware respondia os 2 saltos em **1 linha só** (`"respA respB\n"`) = **1
  notificação**. No fluxo reaberto isso passava (o app espera o timeout e o
  `parseSaltosDesafio` extrai 2 números da linha). No INSTALL, com a bateria
  recém-inserida e o módulo **recém-configurado (AT+RESET no boot) "groggy"**, a
  1ª transmissão saía **truncada** → 1 salto → dentro dos 3s sem retry → **F05**.
- F05 é *antes* de qualquer conta de seed, então a diferença de fonte das seeds
  (install vem do `installDeviceCall`; reaberto vem do `accessInfo`) **não** é a
  causa (seed errada daria f08/f09/CALIBRACAOERROR).

**Correção (firmware, `atenderApp` step 0):** manda o par de saltos **DUAS vezes**
(`enviaLinha(buf); delay(60); enviaLinha(buf);`). O app recebe as 2 notificações
que espera e completa **na hora** (sem esperar os 3s); e se a 1ª via vier
truncada, a 2ª completa → nunca cai abaixo de 2 saltos → **sem F05**. Como o token
é bypass, repetir/valores divergentes são inócuos (vale p/ abrir/fechar também, que
inclusive ficam mais rápidos). Flash 51%. **Regravar as fechaduras.**

## Sessão 05/07 — feedback de boot (LED/som) + confronto físico na bancada

**1. Sinais de boot (firmware):** ao ligar a bateria o instalador agora VÊ e OUVE
o estado:
- 1 bipe (1800 Hz) + **LED VERMELHO** = bootando/preparando o rádio.
- ao terminar a config: **3 piscadas VERDES + melodia ascendente** (1600→2100→
  2600 Hz) = **PRONTA para conectar**; depois apaga os LEDs (poupa bateria) e dorme.
- **bipe curto (2600 Hz) ao ACORDAR por BLE** — vira diagnóstico: se a fechadura
  **conecta e NÃO bipa**, o pino de wake (PD3) não subiu → é o caso "conecta mas
  não responde". Helpers `ledCor()`/`sinalPronto()`. Flash 52%.

**2. Confronto físico na bancada (web):** o firmware pode responder `OK-MOT1` e o
motor **não girar** (fio solto) — foi o caso da FI 2776 no log. Agora, no
auto-teste e nos testes por componente, depois do OK do firmware a bancada
**PERGUNTA ao operador** ("A fechadura APITOU?", "O motor GIROU?", "Os LEDs
ACENDERAM?") com **SIM/NÃO**. O resultado final cruza *firmware respondeu* **E**
*operador confirmou*; um "NÃO" reprova como **falha de hardware** e vai pro log.
`/api/test` virou síncrono (devolve o resultado real) + `/api/confirm` registra o
veredito. Auto-teste passou a ser orquestrado no navegador.

**Diagnóstico do log 05/07 (2 fechaduras problemáticas):**
- **FI 2418:** encontrada (rssi forte) e conecta, mas **nunca responde ao PING**
  (nem à retentativa). Com o firmware novo: se conectar e **não bipar**, o wake
  não disparou → suspeitar do módulo/solda do pino de wake, ou reflashar. Se
  bipar mas não responder → RX do módulo corrompido (clock/baud daquela unidade).
- **FI 2485:** nome do advertising saiu **corrompido** (`CHAVIFI` / `803FI002485`
  em vez de `003FI002485`) apesar da EEPROM validar o serial certo → glitch na
  transmissão do `AT+NAME` (SoftwareSerial 2400 marginal naquela unidade). O LED
  verde de "pronta" ajuda a separar "config completou" de "config falhou".
  Unidades assim: reflashar; se persistir, trocar o módulo BLE.

## Sessão 05/07 — MOSFET: soltar os periféricos em Hi-Z antes de dormir

**Sintoma:** depois de gravar e ligar a bateria, várias fechaduras não conectam:
umas nem anunciam (2449), outras anunciam mas não acordam (2418). O Leonardo deu
a pista de hardware: **esta placa tem MOSFET** — o **módulo BLE fica sempre
ligado** e o **resto (motor/LEDs/etc.) é chaveado**.

**Causa:** o nosso `dormir()` deixava os pinos dos periféricos (motor PB1/PB2,
LEDs PB3, buzzer PD6) como **OUTPUT** durante o `powerDown`. Nessa placa esses
pinos ficam atrás do MOSFET; como OUTPUT eles **empurram corrente para o rail
chaveado (desligado)**, realimentando/segurando o MOSFET e **desestabilizando a
alimentação do módulo BLE** (que precisa ficar estável e sempre ligado para
anunciar e acordar o MCU pelo PD3). O **FI_1_5 antigo já resolvia isso**:
`goToSleep()` chama `clearPinState()` que põe **todos os pinos em INPUT** antes de
dormir (`include/interrupt.h`). Nós não fazíamos.

**O `pinWakeuC` (PD3) NÃO era o problema** — a gente já o lê como INPUT e o wake
por interrupção (RISING) está correto, igual ao FI_1_5.

**Correção (firmware `dormir()`):** `perifericosHiZ()` põe motor/LEDs/buzzer em
**INPUT (Hi-Z)** antes do `powerDown`; `perifericosOut()` devolve a OUTPUT ao
acordar. Espelha o `clearPinState` do FI_1_5, **sem** o desgaste de EEPROM do
original (que gravava DDR/PORT a cada sono). NÃO mexe em PD4/PD5 (BLE, sempre
alimentado) nem em PD2/PD3 (botão/wake). Também: o **LED vermelho de boot** deixou
de ficar aceso (~60mA) durante o `AT+RESET`/config do módulo — vira um flash de
250ms e apaga antes de configurar o rádio (segurar corrente aí dava brownout e
corrompia os AT, ex. nome `803...`). Flash 52%. **Regravar.**

## Sessão 05/07 — versão do módulo (wake) + brownout do motor

Dois achados de bancada testando 2418/2449:

**A) "Conecta mas não responde" (2418) = wake config errado por versão do módulo.**
O FI_1_5 antigo lê `AT+VERS?` e escolhe o wake: **ver.03 → `AT+STATUS6`**,
**ver.04 → `AT+BEFC000`+`AT+AFTC008`** (`setupBLE01`/`CheckVersBLE`). O nosso
mandava **sempre BEFC/AFTC** → num módulo ver.03 o PD3 nunca sobe → conecta mas o
MCU não acorda. **Fix:** `bleLerVersao()` lê a versão e `configModulo` aplica o
wake certo (3→STATUS6, 4→BEFC/AFTC, 0/desconhecido→manda os dois). O `TST-INFO`
agora mostra `WAKE:vXX` (03/04/00) — dá pra ver a geração de cada unidade.

**B) Brownout do motor derruba o BLE (2449).** No auto-teste a 2449 passou
buzzer/LED/motor-1, mas no **motor-2** mandou `OK-MOT2`, o motor girou e a
**conexão caiu antes do `FIM-MOT2`** — o motor puxa corrente (pior no stall), a
bateria afunda abaixo do brownout e o módulo BLE (que deveria ficar sempre ligado)
cai junto / o MCU reseta. **Fixes:**
- Firmware: pulso de motor **curto no teste** (`MOTOR_TST_MS=450ms`, vs 1000ms do
  abrir/fechar real) → menos energia de stall. `OK-MOT` é enviado ANTES do giro.
- Bancada: motores por **último** no auto-teste, teste do motor fecha no `OK-MOT`
  (não espera `FIM-MOT`, que vem depois do pico), **pausa de 1,5s** antes de cada
  motor (bateria recupera) e **auto-reconexão 1x** se um teste não responder.

> Observação: o brownout do motor é limite de POTÊNCIA (bateria/mecânica). No
> campo o abrir roda 1x e o comando já executou mesmo se o BLE cair depois. Bateria
> fraca ou motor duro pode precisar de bateria melhor. Se o abrir real (1000ms)
> resetar no stall, o caminho é reintroduzir a detecção de batente por INA219.

## Sessão 05/07 — ⚠️ CORREÇÃO: o baud é 9600, NÃO 2400 (causa-raiz do silêncio)

**A conclusão antiga "BAUD DO MÓDULO = 2400 FIXO" estava ERRADA** e era a
bomba-relógio. Descoberto lendo o firmware comprovado desta placa (FI_1_5_400),
que tem um FIX de 04/07 com o comentário explícito:
> "O 2400 fixo era a **CAUSA-RAIZ do silêncio total**: os módulos são padrão
> **9600** (medido em campo: unidade 2287 estava em 9600) e o firmware falava 2400
> → UART módulo↔MCU vira lixo → MCU mudo → F07/F05 em tudo."

Sintomas que isso explica: fechadura grava OK mas **não anuncia / "não encontrada"**,
conecta mas não responde, nome do advertising corrompido (`803...`), `MOD:SEM-AT`
(o módulo não respondia ao `AT` porque estava no baud errado). As 2585/2776 que
"funcionavam a 2400" eram um sub-lote realmente em 2400; a maioria é 9600.

**Correção (firmware):** `BAUD_MODULO = 9600` + `bleSincronizarBaud()` no boot,
que **converge qualquer módulo para 9600** (auto-cura, espelha o
`sincronizarBaudBLE` do FI_1_5_400): tenta 9600; se não responde, varre
`{2400,38400,19200,57600,4800,115200}`, ao achar manda `AT+BAUD2`(=9600)+`AT+RESET`
e volta a 9600. Depois o módulo fica gravado em 9600 e o boot é rápido. Flash 54%.

**O baud é INTERNO (MCU↔módulo). NÃO muda nada no app nem na bancada** — o app fala
BLE GATT (FFE1), não vê o baud da UART. Só o firmware muda. **Regravar tudo.**

> Nota: o "nem beep nem led" reportado é provavelmente o beep de boot ÚNICO (fácil
> de perder) + a fechadura dormindo; o problema REAL era o BLE mudo por baud. Com
> 9600 o módulo passa a ser configurado e a anunciar.

## Sessão 05/07 — config do módulo UMA VEZ (fim da tempestade de reset)

Após ir p/ 9600, a fechadura passou a: MCU vivo (melodia ouvida), módulo ecoando
`AT` LIMPO a 9600 (baud OK!), mas depois **enxurrada de `�` por ~15s e cai o BLE**.
Causa: nosso firmware fazia `bleSincronizarBaud` (varre bauds) **+ `AT+RESET`** a
**cada boot** → o reset derruba a conexão e cospe lixo enquanto o módulo reinicia
(e pode reiniciar por brownout ao conectar → tempestade). O FI_1_5_400 **não faz
isso**: configura o módulo UMA vez (`setupBLE01`) e depois só reafirma o essencial
(`ensureModuleConfig`, SEM reset).

**Correção (espelha o FI_1_5_400):**
- `configModuloCompleto()` (baud→9600 + config + NOME + 1 RESET) roda **só na 1ª
  vez após gravar**, gated por um flag na EEPROM (`EE_MOD_CFG=910`, magic 0xC9). O
  `seed.bin` zera o byte → toda regravação re-provisiona; depois o flag persiste.
- `configModuloLeve()` (TYPE/MODE/ROLE/DELI/NOTI/BEFC/AFTC/PIO60, **sem reset,
  sem nome, sem baud**) roda em todo boot. Rápida e não derruba o BLE.
- Removido `AT+PWRM1` (o campo não usa) — acaba com a grogginess do módulo.
- Flash 53%.

**Ao testar:** grave → **espere ~15s OU religue a bateria UMA vez** (deixa a config
completa da 1ª vez terminar e assentar) → só então **Conectar (BLE)**. O 2º boot
já roda só a config leve (limpa, sem reset) e conecta sem lixo.

### Correção do baud (05/07 cont.) — converge ÀS CEGAS, não por AT
A 2585 continuou dando retorno garbled (`���^`) mesmo a 9600: ela é um módulo
**2400** e a auto-cura por AT falhou (os clones NÃO respondem "OK" a um "AT"
pelado — era o `MOD:SEM-AT`). Trocado `bleSincronizarBaud` (dependia de resposta)
por **`bleConvergir9600()`**: manda `AT+BAUD2`+`AT+RESET` em CADA baud candidato
(`{2400,9600,38400,19200,57600,4800}`), sem checar resposta. No baud real o módulo
obedece → 9600; nos outros é lixo ignorado. Roda só na config completa (1ª vez).
**Espere a MELODIA de pronta antes de conectar** — ela toca só no fim do boot
(depois do converge). Flash 53%.

## Sessão 05/07 — a VIRADA final do baud: auto-baud por DADOS (não forçar)

Forçar 9600 (`AT+BAUD2`) NÃO funcionou: a 2585 continuou garbled (`���^`). O
Leonardo cravou a pergunta certa: *"o 9600 é garantido em todas as pontas? não
tem ponta que não aceita?"* — **exatamente**. Diagnóstico final:
- Os módulos vêm em **bauds MISTOS** (a 2585 é **2400** e funcionava lindamente no
  firmware 2400; outras podem ser 9600).
- O opcode **`AT+BAUD2` é ambíguo entre clones** (9600 num, 38400 noutro) → forçar
  não gruda de forma confiável.
- E os clones **não respondem "OK" a um "AT" pelado** → detecção por AT falha.

**Solução (a que já tinha funcionado): auto-baud por DADOS.** O firmware NÃO muda
o baud do módulo — **descobre** qual o módulo já usa, pelos DADOS do app:
- `BAUD_CANDS={9600,2400,38400,19200}`, índice na EEPROM (`EE_BAUD_IDX=909`).
- Boot: fala no último baud salvo.
- `atenderApp`: se chega byte mas nada CONFIRMA o baud em 2,5s (lixo), **avança
  pro próximo baud** e relê. Confirma só um `TST-`, um comando (1/2/190720) ou
  texto de calibração — NÃO o desafio cru (lixo pode parecer número). Ao fechar
  um handshake/PING, **trava o baud na EEPROM**. **Bipa a cada troca** (tom pelo
  índice) → dá pra ouvir convergindo. Converge numa conexão (a bancada manda 10
  PINGs seguidos). Flash 54%. Removido todo o "forçar AT+BAUD/reset".

**Ao testar:** grave → ligue → Conectar (BLE). Vai **ouvir a fechadura bipar** ao
ciclar os bauds; quando travar no certo, vira **PONG** e não bipa mais. Da próxima
vez já abre direto (baud gravado).

## Sessão 05/07 (final) — BAUD 2400 DEFINITIVO (provado pela esteira), firmware v2.1.0

**A dúvida 2400×9600 morreu com uma PROVA, não com medição:** a esteira de
provisionamento antiga (`Firmware-Antigo/src/at.js`, fluxo oficial de produção)
configura TODO módulo com o lote `AT+SHIELD1 → AT+BAUD0 → AT+PWRM1 → AT+MODE2 →
AT+BEFC020 → AT+AFTC028 → AT+NAME<serial> → AT+RESET`. **`AT+BAUD0` = 2400
nesses clones** → a frota de produção fala 2400 (confirmado pelo Leonardo: "já
tenho outras fechaduras em produção dessa forma"). O vai-e-vem anterior:
- Módulo NOVO de fábrica vem em 9600 (por isso as medições de 9600).
- A linha FI_1_5 SEM mosfet fala 9600 (`AT+BAUD2`) — é OUTRA frota.
- A linha `_400` (esta placa, com MOSFET) = 2400 via esteira.
- **Os `BEFC020/AFTC028` do _400 NÃO eram "mutilação"**: o `at.js` calcula
  esses hex para MOSFET no PIO8 (bit5) + wake no PIO6 (bit3). BEFC020 = mosfet
  ligado antes da conexão; AFTC028 = mosfet ligado + wake alto depois.

**Firmware `chavi_fi.ino` v2.1.0 (reescrito limpo, compilado 55%/43%):**
1. **2400 fixo** + `bleProvisionar()` SÓ no 1º boot após gravar (flag
   `EE_MOD_CFG=910`, magic 0xC9; o seed.bin zera → toda regravação
   re-provisiona): converge o módulo p/ 2400 às cegas (`AT+BAUD0`+`AT+RESET`
   em cada baud candidato — clones não respondem "OK" a `AT` pelado), depois
   aplica o MESMO lote AT da esteira + `AT+NAME` + `AT+RESET`. Boots seguintes:
   `configModuloLeve()` (sem reset, sem nome, sem baud). Auto-baud por dados
   REMOVIDO (o baud agora é determinístico como a esteira).
2. **Feedback claro:** 1 bipe = viva → **melodia do Rocky (Gonna Fly Now) + 3
   piscadas verdes = PRONTA** / 4 graves + vermelho = módulo mudo / bipe curto
   ao acordar por BLE. Piscadas CURTAS (nada aceso contínuo no boot — brownout).
   `TST-ROCKY` toca a melodia sob demanda.
3. **Botão inteligente:** curto (<0,8s) = motor em toggle (bipe agudo=vai,
   grave=volta); segurar ≥3s = bipes de contagem subindo; **10s = reset total**
   (melodia descendente, apaga `EE_MOD_CFG` p/ re-provisionar o rádio e
   `wdt_enable(WDTO_15MS)` = reset REAL, como tirar a bateria). Watchdog
   desarmado em `.init3` no boot (senão loop infinito de reset pós-WDT).
4. Versão do módulo (`AT+VERS?`) lida SÓ no provisionamento e gravada na
   EEPROM 768 (`WAKE:vXX` no TST-INFO sem custo em todo boot); ver.03 ganha
   `AT+STATUS8` (como o _400 de produção, não STATUS6 do FI_1_5 sem mosfet).
5. Headers órfãos removidos (`config.h/protocolo.h/ble_modulo.h/eeprom_map.h`
   não eram incluídos pelo .ino e divergiam — ex. falavam 9600/16MHz).

**Bancada:** `bancada.sh` agora instala `bleak` (estava faltando — o passo
"Conectar (BLE)" quebraria num venv novo); mensagens atualizadas (sem
auto-baud; "espere a melodia"); smoke test do servidor OK (página, /api/seeds,
/api/state); seeds bancada×gerar_seed conferidas.

**Para testar na bancada:** regravar → 1 bipe → aguardar a MELODIA (1º boot
provisiona o rádio, alguns segundos a mais) → Conectar (BLE) → auto-teste →
abrir/fechar pelo app. Botão: toque curto gira; 10s reseta.

### Rodada 2 (05/07 tarde) — caso CH003FI002424 e os fixes que fecharam

A 2424 (nova) gravou OK, provisionou OK (anunciava `003FI002424`), mas: bipe
fraco no boot, sem melodia audível, **conecta e nunca responde** (sem PONG).
Dump da EEPROM: flag 910=0xC9 (provisionamento completou) e **versBLE 768=0**
(módulo clone que não responde consultas AT — família "ver.12", cujo **pino de
wake PIO6→PD3 não sobe ao conectar** → MCU dormia p/ sempre). Fixes no v2.1:
1. **Wake por DADOS** — o PCINT que o SoftwareSerial arma no pino RX também
   acorda o MCU do powerDown; o `loop()` agora chama `atenderApp()` em QUALQUER
   wake (o firmware antigo ignorava esse wake e voltava a dormir — era o
   "conecta mas não responde" sem solução). Com isso, mesmo módulo com wake
   quebrado funciona: os writes do app acordam o MCU (1º write pode se perder;
   app/bancada retransmitem).
2. Triagem do boot usa `bleVivo()` (AT **e** AT+VERS?; qualquer byte = vivo) —
   reduz o "4 beeps" falso-negativo dos clones.
3. Flag `EE_MOD_CFG` gravada ANTES do reset final do provisionamento (brownout
   no meio não deixa mais a fechadura re-provisionando em todo boot).
4. 2 bipes curtos = "provisionando o rádio, aguarde".
5. **Debug serial** (`FEATURE_SERIAL_DEBUG 1`, UART hardware TX=PD1, 9600):
   `[boot]/[prov]/[wake]/[app]` no terminal. Cabo USB-TTL: PRETO=GND,
   BRANCO(RX do cabo)→pad TX, VERDE(TX do cabo)→pad RX, vermelho NÃO liga.
   `tools/.venv-bancada/bin/pyserial-miniterm <porta> 9600`.

**✅ VALIDADO na CH003FI003066 (12:12):** melodia + PONG na 1ª tentativa +
auto-teste completo (buzzer/LEDs/BAT 4.09/motor A e B confirmados). Compila
61% flash / 51% RAM.

**Gravação — erro conhecido:** `eeprom verification mismatch: device 0x00 !=
input 0x01 at addr 0x0001` = contato ISP/energia marginal durante o write;
repetir a gravação resolve (a 3066 passou na 3ª tentativa).

## Inconsistências ainda em aberto (a confirmar em bancada)

1. **Tabela de baud do clone** — confirmar qual opcode (`AT+BAUD0` vs `AT+BAUD2`)
   dá 9600 no lote específico. O auto-sync do novo firmware contorna, mas queremos
   saber o valor de fábrica para o script de bancada.
2. **Fronteira de campo do módulo "Soft BLE 5.2"** — esse lote consome o `\n` da
   UART e manda os 2 saltos colados; o app já insere `\n` sintético por fronteira
   de notificação. O firmware novo deve **enviar cada campo em um write separado**
   (não colar) para o app fatiar por notificação. Validar no lote `FI_1_5_400`.
3. **App Tauri grava só flash** (sem fuses/EEPROM) — o fluxo novo `gravar.sh` cobre
   isso; decidir se o Tauri será atualizado ou aposentado em favor do script.
4. **Sentido de giro entre app-imoveis e app-tester** é invertido — garantir que a
   calibração de bancada use o mesmo app que o cliente usa em campo.
