#!/usr/bin/env bash
# Gera o pacote "Chavi-Fi-Imoveis-Setup" para macOS (rode ISTO num Mac).
# Resultado: packaging/dist/Chavi-Fi-Imoveis-Setup-mac.zip — o usuário
# descompacta e roda o app, SEM instalar Python/avrdude.
set -euo pipefail
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(dirname "$HERE")"                       # .../firmware
cd "$FW"

echo "== 1/4 firmware .hex (compila se preciso) =="
if [[ ! -f bin/chavi_fi.ino.hex || chavi_fi/chavi_fi.ino -nt bin/chavi_fi.ino.hex ]]; then
  arduino-cli compile --profile chavi_fi --build-path bin chavi_fi
fi

echo "== 2/4 avrdude standalone =="
[[ -x packaging/avrdude/mac/avrdude ]] || bash packaging/baixar_avrdude.sh mac

echo "== 3/4 venv de build + PyInstaller =="
BV="$HERE/.venv-build"
[[ -d "$BV" ]] || /usr/bin/python3 -m venv "$BV"
"$BV/bin/pip" install --quiet --upgrade pip pyinstaller bleak pyserial requests

echo "== 4/4 empacotando =="
rm -rf "$HERE/build" "$HERE/dist"
"$BV/bin/pyinstaller" --clean --distpath "$HERE/dist" --workpath "$HERE/build" \
  "$HERE/chavi_fi_setup.spec"

# instruções ao lado do app + zip
cp "$HERE/LEIA-ME.txt" "$HERE/dist/Chavi-Fi-Imoveis-Setup/" 2>/dev/null || true
cd "$HERE/dist"
zip -rqy Chavi-Fi-Imoveis-Setup-mac.zip Chavi-Fi-Imoveis-Setup
echo ">> PRONTO: packaging/dist/Chavi-Fi-Imoveis-Setup-mac.zip"
echo ">> Distribua o .zip; no Mac de destino: descompactar e rodar o app"
echo "   (1ª vez: botão direito ▸ Abrir, por causa do Gatekeeper)."
