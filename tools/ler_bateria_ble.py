#!/usr/bin/env python3
"""Lê a tensão da bateria de uma fechadura FI direto pelo BLE, sem app e sem banco.

    .venv-bancada/bin/python ler_bateria_ble.py FI002910
    .venv-bancada/bin/python ler_bateria_ble.py FI002910 --json

Faz scan, conecta na característica FFE1, acorda o MCU com TST-PING e pergunta
TST-BAT (tensão da bateria pelo divisor no A1) e TST-INFO (contadores de boot,
corte de trilho e versão de firmware).

Por que não usar o VCC do TST-INFO como tensão da bateria: a placa v2.7 tem um
StepUp MT3608 que fixa o trilho do MCU em ~5V, então VCC fica ~4,9V mesmo com a
célula em 3,5V. Só o TST-BAT mede a bateria de verdade.

Este script é uma versão enxuta e headless da camada BLE do bancada.py — o
bancada é interativo e serve à esteira de gravação; aqui o objetivo é uma leitura
só, script-friendly, para monitoramento.
"""

import argparse
import asyncio
import json
import re
import sys

from bleak import BleakClient, BleakScanner

CHR_FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"


async def procurar(padrao: str, timeout: float):
    """Devolve (endereço, nome) do device cujo nome casa com o padrão, RSSI mais forte."""
    devs = await BleakScanner.discover(timeout=timeout, return_adv=True)
    candidatos = []
    vistos = []
    for _addr, (dev, adv) in devs.items():
        nome = adv.local_name or dev.name or ""
        if nome:
            vistos.append((adv.rssi, nome, dev.address))
        if re.search(padrao, nome, re.IGNORECASE):
            candidatos.append((adv.rssi, dev.address, nome))
    candidatos.sort()
    return (candidatos[-1][1], candidatos[-1][2], candidatos[-1][0]) if candidatos else (None, None, None), vistos


class Sessao:
    """Conexão FFE1 com buffer de notificações."""

    def __init__(self, client: BleakClient):
        self.client = client
        self.buf = ""

    def _cb(self, _c, data: bytearray):
        self.buf += data.decode("utf-8", errors="replace")

    async def iniciar(self):
        await self.client.start_notify(CHR_FFE1, self._cb)
        await asyncio.sleep(1.5)  # o MCU boota quando a conexão religa o trilho

    async def cmd(self, texto: str, alvos, timeout=8.0, tentativas=3):
        """Envia um comando e espera uma das respostas-alvo.

        O firmware repete algumas respostas (o app espera 2 notificações), então a
        segunda cópia da resposta anterior cai na janela do comando seguinte e o
        engole. Daí a pausa de drenagem antes de escrever e o retry: sem os dois,
        TST-BAT falha silenciosamente logo depois de um TST-PING.
        """
        for tentativa in range(tentativas):
            await asyncio.sleep(1.1)  # deixa passar a repetição da resposta anterior
            self.buf = ""
            await self.client.write_gatt_char(CHR_FFE1, texto.encode(), response=False)
            alvo_t = asyncio.get_event_loop().time() + timeout
            while asyncio.get_event_loop().time() < alvo_t:
                await asyncio.sleep(0.05)
                if any(a in self.buf for a in alvos):
                    return True, self.buf
        return False, self.buf


# Campos que o TST-INFO emite, na ordem do firmware (enviaInfo).
CAMPOS_INFO = ["SER", "CAL", "SEEDS", "MOD", "MODF", "INA", "PLACA", "MOSFET", "WAKE",
               "UPTIME", "RST", "MCUSR", "BOOTS", "BODS", "CUTS", "VCC", "VCCMIN",
               "ATOK", "CUT8", "VER"]


def extrair_info(texto: str) -> dict:
    """Puxa os campos CHAVE:VALOR do TST-INFO (ex.: BOOTS:45 CUTS:0 VER:2.27.0).

    As linhas chegam grudadas quando o módulo emenda as notificações, e cortar
    pela "próxima chave em maiúsculas" não serve: em RST:B o valor é uma letra
    maiúscula e o campo seguinte vira "BMCUSR". Por isso o corte usa a lista
    fechada de chaves que o firmware emite.
    """
    chaves = "|".join(CAMPOS_INFO)
    achados = re.findall(rf"({chaves}):(.*?)(?=(?:{chaves}):|FIM-INFO|$)", texto, re.DOTALL)
    return {k: v.strip() for k, v in achados}


async def principal(args) -> int:
    (addr, nome, rssi), vistos = await procurar(args.alvo, args.scan_timeout)
    if not addr:
        print(f"fechadura '{args.alvo}' não encontrada no ar.", file=sys.stderr)
        if vistos:
            print("devices vistos no scan:", file=sys.stderr)
            for r, n, a in sorted(vistos, reverse=True)[:15]:
                print(f"  {n}  {a}  rssi={r}", file=sys.stderr)
        return 2

    resultado = {"serial": nome, "endereco": addr, "rssi": rssi}
    async with BleakClient(addr) as client:
        s = Sessao(client)
        await s.iniciar()

        ok, _ = await s.cmd("TST-PING", ["PONG"], timeout=args.timeout)
        resultado["ping"] = ok
        if not ok:
            print("conectou mas não respondeu ao TST-PING.", file=sys.stderr)
            return 3

        ok, buf = await s.cmd("TST-BAT", ["BAT:"], timeout=args.timeout)
        if ok:
            m = re.search(r"BAT:\s*([\d.]+)", buf)
            if m:
                resultado["bateria_v"] = float(m.group(1))
        else:
            resultado["erro_bat"] = buf.strip()

        ok, buf = await s.cmd("TST-INFO", ["FIM-INFO"], timeout=args.timeout + 4)
        if ok:
            resultado["info"] = extrair_info(buf)

    if args.json:
        print(json.dumps(resultado, ensure_ascii=False))
    else:
        v = resultado.get("bateria_v")
        print(f"{resultado['serial']}  rssi={rssi}")
        print(f"  bateria: {v:.2f} V" if v is not None else "  bateria: (não lida)")
        info = resultado.get("info", {})
        if info:
            campos = ["VER", "BOOTS", "BODS", "CUTS", "UPTIME", "RST", "MOD", "VCC", "VCCMIN"]
            print("  " + "  ".join(f"{c}:{info[c]}" for c in campos if c in info))
    return 0


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("alvo", nargs="?", default="FI002910",
                   help="trecho do nome anunciado da fechadura (regex; default FI002910)")
    p.add_argument("--json", action="store_true", help="saída em JSON")
    p.add_argument("--timeout", type=float, default=8.0, help="timeout por comando (s)")
    p.add_argument("--scan-timeout", type=float, default=8.0, help="janela de scan (s)")
    sys.exit(asyncio.run(principal(p.parse_args())))
