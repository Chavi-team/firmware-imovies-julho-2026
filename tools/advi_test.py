#!/usr/bin/env python3
"""
advi_test.py — Experimento de ECONOMIA DE RÁDIO: intervalo de advertising (AT+ADVI).

O módulo BLE (PWRM0) é o maior consumidor restante da fechadura (~1,5mA) e o
grosso disso é o duty do advertising. Aumentar o intervalo economiza — MAS
aumenta a latência de descoberta no app (iOS recomenda intervalos ≤ ~1285ms;
valores altos demais deixam a fechadura "sumida" no scan). Produção usa ADVI2.

Este script muda o ADVI PELO AR (BLE, baud-agnóstico, funciona em MODE2
conectado) para MEDIR o trade-off na bancada, SEM regravar firmware:

    ./tools/.venv-bancada/bin/python tools/advi_test.py 003FI003066          # consulta
    ./tools/.venv-bancada/bin/python tools/advi_test.py 003FI003066 --set 3  # aplica
    ./tools/.venv-bancada/bin/python tools/advi_test.py 003FI003066 --set 2  # REVERTE (produção)

Protocolo de medição (anotar os 3 números por valor de ADVI):
  1. corrente em série com a bateria, fechadura em repouso (multímetro em mA);
  2. tempo até a fechadura aparecer no scan do app (média de 5 tentativas);
  3. tempo do "abrir" fim-a-fim no app.
Valores sugeridos: 2 (produção, ~211ms) → 3 → 4. NÃO usar 9 (segundos de
intervalo — estoura o teto do iOS; foi o valor da branch novo-sistema, vetado).

Obs.: o ADVI fica salvo na NVM do módulo e SOBREVIVE a reboot (o firmware só
reaplica ADVI no provisionamento completo, não a cada boot). Ao terminar o
experimento, reverter com --set 2 OU re-provisionar (regravar) a fechadura.
No BLE-1010 o ADVI só vale após reset — o script já manda AT+RESET ao aplicar
(derruba a conexão; é esperado).
"""
import argparse
import asyncio
import sys

CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
VALIDOS = "0123456789"          # índices aceitos (0=menor intervalo … 9=maior)


async def achar(serial, timeout):
    from bleak import BleakScanner
    alvo = serial.strip().upper()
    achado = None

    def cb(dev, ad):
        nonlocal achado
        nome = (dev.name or ad.local_name or "").strip().upper()
        if nome == alvo:
            achado = dev

    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    for _ in range(int(timeout * 10)):
        if achado:
            break
        await asyncio.sleep(0.1)
    await scanner.stop()
    return achado


async def main():
    ap = argparse.ArgumentParser(description="Experimento AT+ADVI pelo ar (só serial EXATO)")
    ap.add_argument("serial", help="nome BLE exato da fechadura (ex.: 003FI003066)")
    ap.add_argument("--set", dest="advi", metavar="N",
                    help=f"aplica AT+ADVI<N> + AT+RESET (N em [{VALIDOS}]); sem --set = só consulta")
    ap.add_argument("--timeout", type=float, default=10.0, help="timeout do scan (s)")
    a = ap.parse_args()

    if a.advi is not None and (len(a.advi) != 1 or a.advi not in VALIDOS):
        sys.exit(f"valor inválido: --set {a.advi} (use um dígito de {VALIDOS[0]} a {VALIDOS[-1]})")
    if a.advi == "9":
        sys.exit("ADVI9 vetado: intervalo de segundos estoura o teto do iOS (descoberta "
                 "lentíssima/instável). Teste 3 ou 4.")

    from bleak import BleakClient

    print(f"escaneando por '{a.serial}' (só serial EXATO; vizinhas são ignoradas)…")
    dev = await achar(a.serial, a.timeout)
    if not dev:
        sys.exit("não encontrada. A fechadura precisa estar ANUNCIANDO — desconecte o "
                 "app/bancada dela e tente de novo.")
    print(f"achou: {dev.address}")

    notas = []
    async with BleakClient(dev) as cli:
        await cli.start_notify(CHR_FFE1, lambda _h, d: notas.append(bytes(d)))

        async def cmd(texto, espera=1.2):
            notas.clear()
            await cli.write_gatt_char(CHR_FFE1, (texto + "\r").encode(), response=False)
            await asyncio.sleep(espera)
            resp = b"".join(notas).decode(errors="replace").strip()
            print(f"  {texto:<12} -> {resp or '(sem resposta)'}")
            return resp

        await cmd("AT+ADVI?")
        if a.advi is not None:
            await cmd(f"AT+ADVI{a.advi}")
            await cmd("AT+ADVI?")
            print("  AT+RESET     -> (módulo reinicia; a desconexão agora é esperada)")
            try:
                await cli.write_gatt_char(CHR_FFE1, b"AT+RESET\r", response=False)
                await asyncio.sleep(0.3)
            except Exception:
                pass  # desconexão no meio do reset é o comportamento normal

    if a.advi is not None:
        print(f"\nADVI{a.advi} aplicado. Meça: corrente em repouso (mA em série), tempo de "
              f"descoberta no app (5x) e tempo do abrir. Reverter: --set 2 (produção).")


if __name__ == "__main__":
    asyncio.run(main())
