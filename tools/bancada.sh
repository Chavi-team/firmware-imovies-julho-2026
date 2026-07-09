#!/usr/bin/env bash
#
# bancada.sh — Abre o Assistente de Bancada em modo DEV e libera o console via log externo.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="$HERE/.venv-bancada"
SYS_PY="${SYS_PY:-/usr/bin/python3}"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

if [[ ! -d "$VENV" ]]; then
    echo ">> Criando venv de bancada..."
    "$SYS_PY" -m venv "$VENV"
fi

if ! "$VENV/bin/python" -c "import serial, requests, bleak" 2>/dev/null; then
    echo ">> Instalando dependências no venv..."
    "$VENV/bin/pip" install --quiet --disable-pip-version-check pyserial requests bleak
fi

# Variáveis de ambiente para Modo Dev
export FLASK_ENV=development
export FLASK_DEBUG=1
export PYTHONUNBUFFERED=1

echo ">> Forçando abertura da interface no Google Chrome..."
open -a "Google Chrome" "http://localhost:5000" 2>/dev/null || open "http://localhost:5000"

echo ">> 🚀 Subindo o servidor em MODO DEV..."
echo ">> 💡 DICA: Se o console travar, abra o arquivo 'bancada_dev.log' no VS Code para copiar livremente!"

# ⭐ MÁGICA: Executa o python, joga tudo em tempo real para o arquivo bancada_dev.log e mantem a tela viva
"$VENV/bin/python" "$HERE/bancada.py" 2>&1 | tee "$HERE/bancada_dev.log"