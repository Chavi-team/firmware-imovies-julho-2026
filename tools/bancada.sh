#!/usr/bin/env bash
#
# bancada.sh — Abre o Assistente de Bancada das fechaduras Chavi FI.
#
# A interface roda no NAVEGADOR (app web local) — nada de Tkinter. Este script:
#   - cria um venv em tools/.venv-bancada
#   - instala pyserial + requests
#   - garante arduino-cli e avrdude no PATH (Homebrew)
#   - sobe o servidor local e abre o navegador (tools/bancada.py)
#
# Uso:  ./tools/bancada.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="$HERE/.venv-bancada"
SYS_PY="${SYS_PY:-/usr/bin/python3}"

# arduino-cli / avrdude do Homebrew no PATH
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

if [[ ! -d "$VENV" ]]; then
    echo ">> Criando venv de bancada (primeira vez)..."
    "$SYS_PY" -m venv "$VENV"
fi

# dependências python (BLE + cabo + backend)
if ! "$VENV/bin/python" -c "import serial, requests, bleak" 2>/dev/null; then
    echo ">> Instalando dependências no venv (pyserial, requests, bleak)..."
    "$VENV/bin/pip" install --quiet --disable-pip-version-check pyserial requests bleak
fi

for t in arduino-cli avrdude; do
    command -v "$t" >/dev/null || echo "!! aviso: '$t' não está no PATH (gravação indisponível)"
done

echo ">> Subindo o assistente (abre no navegador)..."
exec "$VENV/bin/python" "$HERE/bancada.py"
