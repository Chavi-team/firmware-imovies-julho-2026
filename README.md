# Firmware Chavi FI (novo)

Firmware das fechaduras BLE da Chavi (setor imobiliário), reescrito para
**funcionar sempre**: quando o app manda abrir, a porta abre.

> Leia o **`CONTEXTO.md`** antes de mexer — é o documento-mestre (protocolo,
> causas-raiz do firmware antigo, decisões de projeto, mapa de EEPROM).

## Estrutura

```
firmware/
├── CONTEXTO.md            # documento-mestre (ler primeiro)
├── README.md              # este arquivo
├── chavi_fi/              # o firmware (MiniCore 328/328PB, 8MHz interno)
│   ├── chavi_fi.ino       # TUDO num arquivo só: boot, protocolo, motor, botão, sono
│   └── sketch.yaml        # perfil arduino-cli (MiniCore + libs)
├── bin/                   # artefatos de build (.hex universal, seed_*.bin)
└── tools/
    ├── gerar_seed.py      # gera seed.bin (EEPROM) de 1 fechadura
    ├── gravar.sh          # grava 1 fechadura por linha de comando
    ├── bancada.py         # assistente de bancada (web local: gravar+validar+testar)
    └── bancada.sh         # launcher do assistente (cuida do venv/PATH) ← use este
```

## Decisões centrais (não regredir)

- **BAUD do módulo BLE = 2400, FIXO.** É como a frota de produção foi
  provisionada (a esteira antiga `Firmware-Antigo/src/at.js` manda `AT+BAUD0`
  = 2400 nesses clones "Soft AT 5.2"). 2400 também é o baud mais robusto para
  o SoftwareSerial a 8MHz interno (9600 perde byte). Módulo novo de fábrica
  (9600) é **convergido para 2400 no 1º boot** após a gravação
  (`bleProvisionar`: manda `AT+BAUD0`+`AT+RESET` às cegas em cada baud
  candidato — os clones não respondem "OK" a um `AT` pelado, então detecção
  por resposta não é confiável). Boots seguintes = só config leve, **sem
  reset** (reset a cada boot derrubava a conexão e cuspia lixo).
- **Bypass total de token** (decisão do cliente: confiabilidade > segurança).
  A fechadura responde os 2 saltos (o app precisa deles p/ fechar o handshake)
  mas não valida nada; o comando `1`/`2` aciona direto.
- **`BEFC020`/`AFTC028`** — valores da esteira de produção para ESTA placa
  (MOSFET no PIO8 do módulo + wake no PIO6). Módulo ver.03 ganha `AT+STATUS8`.
- **Protocolo 100% compatível** com os ~1000 apps em campo: FFE0/FFE1, saltos
  em dobro (mata o F05), respostas "11" da calibração seguradas ~1,2s e em
  dobro (timing que o `calibrarpt1` do app exige).

## Feedback sonoro/visual (o que a fechadura está dizendo)

| Sinal | Significado |
|---|---|
| 1 bipe curto ao ligar a bateria | "estou vivo" (se não tocar = energia/hardware) |
| **Melodia (fanfarra do Rocky) + 3 piscadas VERDES** | boot 100% OK — **pronta para conectar** |
| 4 bipes GRAVES + 2 piscadas VERMELHAS | módulo BLE mudo no auto-teste AT (triagem; pode ser falso-negativo em clone — a conexão BLE real é a prova) |
| 1 bipe curto agudo ao conectar o celular | acordou por BLE (conectou e NÃO bipou = pino de wake não subiu) |
| 2 bipes durante calibração | recebeu o passo da calibração |
| bipes subindo (1/s a partir de 3s segurando o botão) | contagem para o reset |
| melodia DESCENDENTE + 3 piscadas vermelhas | reset total disparado |

## Botão físico

- **Toque curto (<0,8s):** aciona o motor em **toggle** (alterna o sentido a
  cada toque) — destranca/tranca sem celular.
- **Segurar 10s:** **reset total** — re-provisiona o rádio no próximo boot e
  reinicia o MCU (efeito de tirar/recolocar a bateria). NÃO apaga
  serial/seeds/calibração.
- Soltar entre 0,8s e 10s: cancela (1 bipe grave).

## Assistente de bancada (para montar em lote)

```bash
cd firmware
./tools/bancada.sh          # sobe o servidor local e abre no navegador
```

Wizard passo-a-passo (app web local, sem Tkinter), feito para um leigo:

1. **Gravar firmware** (cabo USBasp / avrdude — .hex universal + seed.bin)
2. **Validar gravação** (relê o chip e confere serial + as 4 seeds)
3. **Conectar (BLE)** (scan pelo nome = serial sem "CH"; PING→PONG)
4. **Auto-teste** (buzzer, LEDs, motor A/B, bateria) — cruza a resposta do
   firmware com a confirmação FÍSICA do operador ("o motor girou?")
5. **Cadastrar no sistema** (`POST admin/devices`, só o serial; login por OTP
   de um telefone **admin** `role_id=1`)
6. **Finalizar** (pré-preenche o próximo serial)

Os testes rodam **por BLE** (mesmo caminho do app — provado; nesta placa os
pads do UART não são acessíveis). Comandos aceitos pela FFE1: `TST-PING`,
`TST-BUZ`, `TST-LED`, `TST-MOT1/2`, `TST-BAT`, `TST-INFO`, `TST-ROCKY`,
`TST-ALL`.

> **Gravar exige:** bateria DENTRO da placa (o USBasp não alimenta o MCU) e
> contato ISP firme (header 2x3). Depois de gravar, **espere a MELODIA** (o 1º
> boot faz o provisionamento completo do rádio, alguns segundos) antes de
> conectar. 1ª vez no macOS: dê permissão de Bluetooth ao Terminal.

## Como gravar uma fechadura (linha de comando)

```bash
cd firmware
./tools/gravar.sh CH003FI002465          # ATmega328PB (FI_1_5) — default
./tools/gravar.sh CH003FI002465 m328p    # ATmega328 (FI_1_0)
```

Compila o `.hex` **universal** uma única vez, gera o `seed.bin` desta fechadura
e grava fuses + lock + flash + EEPROM num comando só. **Sem passo AT manual** e
**sem recompilar por dispositivo** — o serial vai na EEPROM e vira o nome BLE
em runtime; o módulo se autoconfigura no 1º boot.

## Validação de compatibilidade (já conferida)

- `tools/gerar_seed.py` produz as **mesmas 4 seeds e o mesmo serial** do
  `seed.bin` legado (conferido byte a byte; bancada e gerador batem entre si).
- O LFSR/handshake reproduz o do app (`generate_token_with_salts.dart`).
- Handshake real validado em bancada: app-imoveis ABRIU a CH003FI002585.
