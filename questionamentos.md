1. O MOSFET está sendo usado?

Sim. Ele é a "chave de energia" da placa — o interruptor que liga/desliga a alimentação do MCU e dos periféricos. Hoje, no modo normal (IDLE), ele fica sempre LIGADO (o firmware segura o pino 8 do módulo em nível alto). Ou seja: ele está no circuito e funcionando, mas configurado pra manter a energia ligada, não pra cortar. Ele só cortaria a energia se você ligasse a hibernação (que está desligada por padrão agora).

2. Quanto tempo dura a bateria?

Quem "gasta" a bateria em repouso é quase 100% o módulo BLE, porque ele fica ligado o tempo todo. O MCU dorme e gasta quase nada (microampères). Pelo manual, o módulo consome:

┌─────────────────────────────────┬──────────┬─────────────────────────────┐
│ Configuração │ Consumo │ Autonomia\*(bateria 2000mAh) │
├─────────────────────────────────┼──────────┼─────────────────────────────┤
│ Atual (PWRM0 — sempre acordado) │ ~1,5 mA │ ~55 dias │
├─────────────────────────────────┼──────────┼─────────────────────────────┤
│ Modo economia (PWRM1) │ ~0,65 mA │ ~4 meses │
└─────────────────────────────────┴──────────┴─────────────────────────────┘

\*Estimativa — eu não tenho a capacidade real da bateria de vocês (mAh); troque na conta capacidade ÷ consumo. O ponto importante: hoje estamos no modo que dura menos, mas é o mais confiável. Aquele "modo economia" que conversamos (que triplica a duração) ainda está pra fazer.

3. Na prática: o BLE fica ligado sempre e o MCU não?

Exatamente isso. O módulo BLE fica sempre ligado — anunciando e escutando, pronto pra receber o app a qualquer momento. O MCU dorme (consumo mínimo) e só acorda quando tem trabalho. É a divisão certa: o rádio é a "recepcionista" que nunca sai; o cérebro só liga quando alguém bate na porta.

4. Como ficou o clock de 8MHz?

Não usa mais 8MHz. Os 8MHz eram o problema antigo (era um oscilador interno impreciso, que não conseguia falar direito com o módulo). Agora roda a 16MHz.

5. Está usando o cristal?

Sim. Roda no cristal externo de 16MHz que a placa já tem (o componente X1 + os dois capacitores de 22pF). É ele que dá o "relógio" preciso — e é justamente o que faz a comunicação com o módulo ser confiável.

6. O BAUD está em 2400?

Não, está em 9600. Os 2400 eram do firmware antigo. Hoje o link entre o MCU e o módulo é a 9600 (que é o pamais estável).

7. Quando o app conecta, ele acorda a placa?

Sim. Quando o app conecta por Bluetooth, o módulo avisa o MCU (manda um sinal no pino de wake) e, além disso, os próprios dados que o app envia acordam o MCU do sono profundo. Em uma fração de segundo o MCU liga, atende e responde. Foi por isso que, mesmo com o MCU dormindo, a fechadura abriu nos testes.

Resumindo o "estado de saúde" atual: cristal 16MHz ✓, baud 9600 ✓, brown-out protegendo as seeds ✓, MCU dormindo + módulo sempre pronto ✓, MOSFET funcionando (em modo "sempre ligado"). O único ganho que ainda está na mesa é a economia de bateria — hoje ela dura ~55 dias (estimativa) porque o módulo fica no modo mais gastão; o "modo economia" (PWRM1 como selo final no fim do teste) levaria pra ~4 meses sem perder confiabilidade, porque a bancada sabe acordar o módulo pelo ar quando precisar.

Quer que eu implemente esse modo economia agora? É a última peça pra fechar os 100%.

O teste respondeu a pergunta de forma definitiva: o modo economia 2400-slow não funciona neste hardware — e a rede de segurança fez o trabalho dela perfeitamente.

O que aconteceu

- Ligou a economia, rodou os 20 ciclos de validação do abrir: ciclos 1-2 passaram, 3-20 falharam (18/20 falhas).
- A bancada reprovou automaticamente e reverteu pro modo NORMAL (9600) sozinha, como projetado.
- A fechadura (que é a da sua porta, 002FI001767) voltou 100% ao normal logo depois: PONG, buzzer, LEDs, os dois motores (84mA / 78mA). Nada foi perdido.

Por que falhou (o que o log mostra)

Nos ciclos que falharam, a fechadura conectava por Bluetooth (o app a acharia), mas o TST-PING não voltava nada — o módulo dormindo não repassava o comando pro MCU ao acordar. E durante o provisionamento apareceu lixo de framing (F8, 80 80 80 F8...), que é a assinatura clássica de baud desalinhado.

A causa é exatamente o risco nº 1 que eu tinha te alertado: em 2400-slow o módulo usa um clock interno de 32kHz que gera um 2400 impreciso, e ele não casa de forma estável com o SoftwareSerial do MCU (16MHz). O manual promete "acorda ao receber dado por BLE" — e o rádio até acorda —, mas o repasse do dado pro MCU não acontece de forma confiável. Retry de software não conserta framing corrompido.

Veredito honesto

A economia por software não é viável nesta placa. O 2400-slow era o único caminho software (o 9600+PWRM1 exigiria um fio de wake por hardware que a placa não tem). Para uma fechadura, 18/20 falhas é catastrófico, e você foi claro: o abrir é inegociável. Então: ficamos no modo NORMAL 9600+PWRM0 — os ~55 dias de bateria são o preço da confiabilidade, e confiabilidade ganha.

O que fica

- develop intocado (nunca teve economia). Você não precisa fazer nada — é onde está o firmware bom (v2.10.1).
- A branch feature/modo-economia-2400 fica preservada como um experimento documentado que falhou (registrei o resultado no CONTEXTO e no commit), pra ninguém tentar isso de novo às cegas. Não deve ser mergeada.
- A única forma de ter os 4 meses de bateria seria revisão de placa (ligar o pino de wake do módulo ao MCU) — item futuro de hardware, não de firmware.

Minha recomendação: fechar essa frente e ficar com o que está sólido. Você chegou nos 100% de confiabilidade que importava — a economia era o "nice to have" que o hardware atual não comporta com segurança. Quer que eu delete a branch (o histórico do experimento fica no reflog por um tempo) ou prefere deixá-la lá como registro?

Questionamento Mosfet, Soft:

Usamos os módulos de vocês numa fechadura: um ATmega328 conversa com o módulo Soft por UART 9600 (temos os dois na frota — "Soft AT 5.2 ver.XX" e "Soft AT ver.XX" 1010), em MODE2 / ROLE0. O app do usuário conecta por BLE e manda o comando de abrir, que o módulo repassa pela UART ao MCU. Um PIO do módulo aciona um MOSFET que controla a energia do MCU.

O que funciona perfeitamente hoje: com AT+PWRM0 (módulo sempre acordado) e o MOSFET mantido ligado (via BEFC/AFTC), o módulo fica sempre anunciando e o MCU dorme. O app conecta e o comando chega no ato, inclusive depois de uma noite inteira em standby. Zero falha.

Onde travamos — tentando economizar bateria (fazer o módulo dormir e/ou cortar o MCU pelo MOSFET):

1. AT+PWRM1 (auto-sleep) a 9600: o módulo continua anunciando e aceita a conexão BLE, mas não repassa mais o dado da UART pro MCU de forma confiável. Pelo manual, com PWRM1 o módulo só aceita dados na UART com GND no pino wake (pino 24). Pergunta: existe alguma forma de, com PWRM1, o módulo acordar e repassar os dados pro MCU só pela conexão BLE (sem pulso de hardware no pino 24)?
2. AT+BAUD0 (2400-slow): o manual diz que, nesse modo, "conectado, o módulo acorda ao receber dados por Bluetooth". Testamos, mas a UART ficou instável — apareceu lixo de framing (bytes tipo F8, 80 80 80 F8), aparentemente porque o 2400 gerado pelo clock interno de 32kHz do módulo não casa com o nosso MCU a 16MHz (18 de 20 aberturas falharam). Pergunta: o 2400-slow é confiável pra repasse de dados MCU↔módulo? Qual a precisão de baud esperada nesse modo e como garantir framing estável?
3. Gate do MOSFET por PIO (BEFC/AFTC vs STATUS): queremos que um PIO fique ALTO ao conectar (liga o MCU) e possa ser cortado no repouso. O manual diz que AT+STATUS conflita com BEFC/AFTC e exige RESET. Pergunta: qual é a forma correta e recomendada de usar um PIO pra chavear um MOSFET de energia conforme o estado da conexão BLE — STATUS, ou BEFC/AFTC, e como evitar o conflito?
4. Pino wake (pino 24): existe um circuito recomendado para o MCU acordar o módulo (linha MCU→pino 24)? É obrigatório pra ter baixo consumo com repasse de dados garantido?

Pergunta central: qual é a arquitetura recomendada por vocês para atingir baixo consumo em standby (módulo dormindo e/ou MCU cortado pelo MOSFET) garantindo que o comando do app sempre chega ao MCU ao conectar por BLE? Hoje só conseguimos confiabilidade total mantendo o módulo sempre acordado (PWRM0); queremos economia sem abrir mão dessa garantia.
