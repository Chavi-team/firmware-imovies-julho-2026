#!/usr/bin/env python3
"""Confere que o .hex versionado é o do FW_VERSION atual do fonte.

Por que existe (incidente de 21/08/2026): o instalador NÃO compila — ele grava
o `.hex` pré-compilado que viaja no repositório. O `bin/` é gitignorado, então
no CI só existe `packaging/firmware/chavi_fi.ino.hex`. Esse arquivo foi
commitado uma vez na v2.21.0 e nunca mais atualizado, enquanto o fonte avançou
até a 2.27.0.

Resultado: TODAS as releases da bancada de 02/08 a 21/08 gravaram 2.21.0 — uma
versão anterior ao fix do LED preso (v2.22.0), que sozinho drena ~0,144 V/dia.
E nada acusava: a bancada mostra a versão DELA no cabeçalho (que por
coincidência também é 2.27.0), não a do firmware que grava.

Este script quebra o build quando as duas versões divergem, para que o erro
apareça em quem empacota e não no cliente, semanas depois.
"""

import os
import re
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTE = os.path.join(RAIZ, "chavi_fi", "chavi_fi.ino")
HEX = os.path.join(RAIZ, "packaging", "firmware", "chavi_fi.ino.hex")


def versao_do_fonte(caminho: str) -> str:
    with open(caminho, encoding="utf-8", errors="replace") as f:
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
    if not m:
        raise SystemExit(f"FW_VERSION não encontrado em {caminho}")
    return m.group(1)


def versoes_do_hex(caminho: str) -> set:
    """Extrai as strings de versão gravadas no binário (formato Intel HEX)."""
    dados = bytearray()
    with open(caminho, encoding="ascii", errors="replace") as f:
        for linha in f:
            linha = linha.strip()
            if not linha.startswith(":"):
                continue
            n = int(linha[1:3], 16)
            tipo = int(linha[7:9], 16)
            if tipo == 0:  # 00 = dados
                dados += bytes.fromhex(linha[9:9 + n * 2])
    return set(re.findall(r"\b\d+\.\d+\.\d+\b", dados.decode("latin1")))


def main() -> int:
    if not os.path.exists(HEX):
        print(f"ERRO: {HEX} não existe — o instalador não teria firmware.")
        return 1

    esperada = versao_do_fonte(FONTE)
    achadas = versoes_do_hex(HEX)

    if esperada in achadas:
        print(f"OK: packaging/firmware/chavi_fi.ino.hex contém {esperada}.")
        return 0

    print("ERRO: o .hex empacotado NÃO é o do fonte atual.")
    print(f"  fonte (chavi_fi.ino FW_VERSION): {esperada}")
    print(f"  versões encontradas no .hex:     {sorted(achadas) or '(nenhuma)'}")
    print()
    print("Recompile e promova o binário antes de empacotar:")
    print("  bash packaging/build_mac.sh   # ou compile como preferir")
    print("  cp bin/chavi_fi.ino.hex packaging/firmware/chavi_fi.ino.hex")
    print("  git add packaging/firmware/chavi_fi.ino.hex")
    return 1


if __name__ == "__main__":
    sys.exit(main())
