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
"$BV/bin/pip" install --quiet --upgrade pip pyinstaller bleak pyserial requests \
  pywebview pyobjc-framework-WebKit pyobjc-framework-Cocoa

echo "== 4/4 empacotando =="
rm -rf "$HERE/build" "$HERE/dist"
"$BV/bin/pyinstaller" --clean --distpath "$HERE/dist" --workpath "$HERE/build" \
  "$HERE/chavi_fi_setup.spec"

# empacota o .app (janela nativa) + instruções. Remove o atributo de quarentena
# para reduzir o atrito do Gatekeeper no destino.
cd "$HERE/dist"
xattr -cr "Chavi-Fi-Imoveis-Setup.app" 2>/dev/null || true
cp "$HERE/LEIA-ME.txt" .
rm -f Chavi-Fi-Imoveis-Setup-mac.zip
zip -rqy Chavi-Fi-Imoveis-Setup-mac.zip Chavi-Fi-Imoveis-Setup.app LEIA-ME.txt
echo ">> PRONTO: packaging/dist/Chavi-Fi-Imoveis-Setup-mac.zip"
echo ">> No Mac de destino: descompactar e rodar o app Chavi-Fi-Imoveis-Setup"
echo "   (1ª vez: botão direito ▸ Abrir, por causa do Gatekeeper)."
