#!/usr/bin/env bash
# Baixa o avrdude STANDALONE (self-contained) das releases oficiais para
# empacotar dentro do Chavi-Fi-Imoveis-Setup. Roda no Mac (para o pacote Mac)
# e no Windows-Git-Bash/WSL (para o pacote Windows), OU baixa os dois de uma vez.
#
# Uso:  ./packaging/baixar_avrdude.sh [mac|win|todos]   (default: a plataforma atual)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VER="8.0"   # avrdudes/avrdude release
BASE="https://github.com/avrdudes/avrdude/releases/download/v${VER}"

alvo="${1:-auto}"
[[ "$alvo" == "auto" ]] && { [[ "$(uname)" == "Darwin" ]] && alvo="mac" || alvo="win"; }

baixa_mac() {
  local d="$HERE/avrdude/mac"; mkdir -p "$d"
  echo ">> avrdude macOS..."
  # release traz um .tar.gz com o binário universal + avrdude.conf
  curl -fL "${BASE}/avrdude_v${VER}_macOS_64bit.tar.gz" -o /tmp/avr_mac.tgz
  rm -rf /tmp/avrx && mkdir -p /tmp/avrx && tar -xzf /tmp/avr_mac.tgz -C /tmp/avrx
  cp "$(find /tmp/avrx -name avrdude -type f | head -1)" "$d/avrdude"
  cp "$(find /tmp/avrx -name avrdude.conf | head -1)" "$d/avrdude.conf"
  chmod +x "$d/avrdude"
  echo ">> ok: $d"
}
baixa_win() {
  local d="$HERE/avrdude/win"; mkdir -p "$d"
  echo ">> avrdude Windows..."
  curl -fL "${BASE}/avrdude_v${VER}_Windows_64bit.zip" -o /tmp/avr_win.zip
  rm -rf /tmp/avr_win && mkdir -p /tmp/avr_win && (cd /tmp/avr_win && unzip -oq /tmp/avr_win.zip)
  cp "$(find /tmp/avr_win -name avrdude.exe | head -1)" "$d/avrdude.exe"
  cp "$(find /tmp/avr_win -name avrdude.conf | head -1)" "$d/avrdude.conf"
  # libusb dll, se vier separada
  find /tmp/avr_win -iname '*.dll' -exec cp {} "$d/" \; 2>/dev/null || true
  echo ">> ok: $d"
}

case "$alvo" in
  mac) baixa_mac ;;
  win) baixa_win ;;
  todos) baixa_mac; baixa_win ;;
  *) echo "uso: $0 [mac|win|todos]"; exit 1 ;;
esac
