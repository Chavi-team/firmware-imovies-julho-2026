#!/usr/bin/env python3
"""AT pelo ar numa FI — diagnóstico e resgate, sem cabo e sem a bancada.

    python3 tools/at_ar.py 001FI001000 --ler
    python3 tools/at_ar.py 001FI001000 --achar-gate
    python3 tools/at_ar.py 001FI001000 --mascaras 020 028
    python3 tools/at_ar.py 001FI001000 --cmd AT+PIO61 --cmd AT+PIO81

Para que serve: o módulo BLE é alimentado DIRETO da bateria, fora da chave do
MOSFET. Então, mesmo com a placa sem energia (MCU mudo, botão morto), o módulo
continua no ar e aceita AT — em `AT+MODE2` o lado remoto controla PIO5..11. É
por esse caminho que se descobre qual pino segura o trilho e se recupera uma
placa que ficou sem energia.

Regras aprendidas em campo e respeitadas aqui:
  · AT pelo ar vai SEM terminador ('\\r' faz o módulo ignorar);
  · máscaras (BEFC/AFTC) só passam a valer depois de AT+RESET;
  · AT+RENEW é proibido: apaga a NVM e mata placa com MOSFET.

⚠️ `--achar-gate` só ERGUE pinos (PIOx1) — nunca corta. É seguro rodar numa
placa que está sem energia: o pior caso é nada acontecer.
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
# Universo de gates da FI 1.0: {7,8,9} no `upload` de produção e {4,5,6,7} na
# esteira at.js. PIO6 entra por último — ele é a linha de wake (PD3) e mexer
# nele tem efeito colateral no MCU.
CANDIDATOS = (8, 7, 9, 5, 4, 6)


class Radio:
    def __init__(self, client):
        self.client = client
        self.buf = ""

    def _rx(self, _c, data: bytearray):
        self.buf += data.decode(errors="replace")

    async def cmd(self, texto, espera=("OK",), timeout=4.0, quieto=False):
        self.buf = ""
        await self.client.write_gatt_char(CHR_FFE1, texto.encode(), response=False)
        t0 = asyncio.get_event_loop().time()
        while asyncio.get_event_loop().time() - t0 < timeout:
            await asyncio.sleep(0.05)
            if any(e in self.buf for e in espera):
                break
        resp = self.buf.strip().replace("\r", " ").replace("\n", " ")
        if not quieto:
            print(f"  → {texto:<18} ⟵ {resp or '(sem resposta)'}")
        return resp

    async def vivo(self, timeout=4.0):
        """O MCU responde? (é ele quem responde PONG; o módulo não)"""
        r = await self.cmd("TST-PING", ("PONG",), timeout=timeout, quieto=True)
        return "PONG" in r


async def achar(alvo, timeout=12.0):
    print(f"procurando '{alvo}' (até {timeout:.0f}s)...")
    d = await BleakScanner.find_device_by_filter(
        lambda dev, _adv: (dev.name or "").upper().endswith(alvo.upper()), timeout=timeout)
    if not d:
        print("✗ não encontrada. Chegue perto e feche o app no celular.")
        return None
    print(f"✓ {d.name}  {d.address}")
    return d


async def principal(a):
    dev = await achar(a.alvo)
    if not dev:
        return 1
    async with BleakClient(dev) as c:
        r = Radio(c)
        await c.start_notify(CHR_FFE1, r._rx)
        await asyncio.sleep(0.5)

        vivo = await r.vivo()
        print(f"MCU: {'VIVO (PONG)' if vivo else 'MUDO — placa provavelmente sem energia'}")

        if a.ler:
            print("\nconfiguração do módulo:")
            for q in ("AT+VERS?", "AT+BEFC?", "AT+AFTC?", "AT+PWRM?", "AT+BAUD?"):
                await r.cmd(q, ("OK", "Soft", "ver"))
            for p in CANDIDATOS:
                await r.cmd(f"AT+PIO{p}?", ("OK+Get",))

        if a.achar_gate:
            print("\nprocurando o pino que ALIMENTA a placa (só ergue, nunca corta):")
            if vivo:
                print("  o MCU já está vivo — nada a religar. Use --ler para ver o estado.")
            for p in CANDIDATOS:
                await r.cmd(f"AT+PIO{p}1", ("OK+Set",))
                await asyncio.sleep(6.0)          # boot frio do MCU leva ~5-8s
                if await r.vivo(timeout=6.0):
                    print(f"\n✅ O MCU VOLTOU com PIO{p} ALTO — este é o gate do MOSFET.")
                    print(f"   Grave na placa com: TST-GATE{p} (ou o passo da bancada).")
                    break
            else:
                print("\n✗ Nenhum pino trouxe o MCU de volta. Restam: bateria fraca/ausente, "
                      "gate no PIO4 fora do alcance remoto, ou problema elétrico na placa.")

        for cmd in (a.cmd or []):
            await r.cmd(cmd, ("OK", "ERR"))

        if a.mascaras:
            befc, aftc = a.mascaras
            print(f"\naplicando BEFC{befc} / AFTC{aftc} (valem após o RESET):")
            await r.cmd(f"AT+BEFC{befc}", ("OK+Set",))
            await r.cmd(f"AT+AFTC{aftc}", ("OK+Set",))
            await r.cmd("AT+RESET", ("OK+RESET",), timeout=3.0)
            print("  módulo reiniciado — reconecte para conferir.")
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("alvo", help="final do nome anunciado (ex.: 001FI001000)")
    ap.add_argument("--ler", action="store_true", help="lê versão, máscaras, PWRM e o estado dos PIOs")
    ap.add_argument("--achar-gate", action="store_true", help="ergue um pino por vez até o MCU voltar")
    ap.add_argument("--cmd", action="append", help="comando AT cru (pode repetir)")
    ap.add_argument("--mascaras", nargs=2, metavar=("BEFC", "AFTC"), help="grava as duas máscaras + RESET")
    args = ap.parse_args()
    if not (args.ler or args.achar_gate or args.cmd or args.mascaras):
        args.ler = True
    sys.exit(asyncio.run(principal(args)))
