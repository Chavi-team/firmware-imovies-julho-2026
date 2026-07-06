# Gera o pacote "Chavi-Fi-Imoveis-Setup" para Windows (rode ISTO num Windows).
# Requisitos no PC de build: Python 3 (python.org, com "Add to PATH"), Git-Bash
# OU 7-Zip para descompactar o avrdude, e conexão à internet (baixa avrdude).
# Resultado: packaging\dist\Chavi-Fi-Imoveis-Setup-win.zip
$ErrorActionPreference = "Stop"
$HERE = Split-Path -Parent $MyInvocation.MyCommand.Path
$FW   = Split-Path -Parent $HERE
Set-Location $FW

Write-Host "== 1/4 firmware .hex ==" -ForegroundColor Cyan
# O .hex vem VERSIONADO no repo (packaging\firmware\chavi_fi.ino.hex) — não
# precisa de arduino-cli no Windows. Copia para bin\ se ainda não existir.
if (-not (Test-Path "bin\chavi_fi.ino.hex")) {
  if (Test-Path "packaging\firmware\chavi_fi.ino.hex") {
    New-Item -ItemType Directory -Force -Path "bin" | Out-Null
    Copy-Item "packaging\firmware\chavi_fi.ino.hex" "bin\chavi_fi.ino.hex"
    Write-Host "   usando o .hex versionado (packaging\firmware\)" -ForegroundColor Green
  } elseif (Get-Command arduino-cli -ErrorAction SilentlyContinue) {
    arduino-cli compile --profile chavi_fi --build-path bin chavi_fi
  } else {
    Write-Warning "Sem .hex versionado e sem arduino-cli. Repositório incompleto."
    exit 1
  }
}

Write-Host "== 2/4 avrdude standalone ==" -ForegroundColor Cyan
$avr = "$HERE\avrdude\win"
if (-not (Test-Path "$avr\avrdude.exe")) {
  New-Item -ItemType Directory -Force -Path $avr | Out-Null
  $zip = "$env:TEMP\avrdude_win.zip"
  Invoke-WebRequest "https://github.com/avrdudes/avrdude/releases/download/v8.0/avrdude_v8.0_Windows_64bit.zip" -OutFile $zip
  Expand-Archive -Force $zip "$env:TEMP\avrdude_win"
  Copy-Item (Get-ChildItem "$env:TEMP\avrdude_win" -Recurse -Filter avrdude.exe | Select -First 1).FullName "$avr\avrdude.exe"
  Copy-Item (Get-ChildItem "$env:TEMP\avrdude_win" -Recurse -Filter avrdude.conf | Select -First 1).FullName "$avr\avrdude.conf"
  Get-ChildItem "$env:TEMP\avrdude_win" -Recurse -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $avr }
}

Write-Host "== 3/4 venv + PyInstaller ==" -ForegroundColor Cyan
$BV = "$HERE\.venv-build"
if (-not (Test-Path $BV)) { python -m venv $BV }
& "$BV\Scripts\pip.exe" install --quiet --upgrade pip pyinstaller bleak pyserial requests pywebview

Write-Host "== 4/4 empacotando ==" -ForegroundColor Cyan
Remove-Item -Recurse -Force "$HERE\build","$HERE\dist" -ErrorAction SilentlyContinue
& "$BV\Scripts\pyinstaller.exe" --clean --distpath "$HERE\dist" --workpath "$HERE\build" "$HERE\chavi_fi_setup.spec"
Copy-Item "$HERE\LEIA-ME.txt" "$HERE\dist\Chavi-Fi-Imoveis-Setup\" -ErrorAction SilentlyContinue
Compress-Archive -Force -Path "$HERE\dist\Chavi-Fi-Imoveis-Setup" -DestinationPath "$HERE\dist\Chavi-Fi-Imoveis-Setup-win.zip"
Write-Host ">> PRONTO: packaging\dist\Chavi-Fi-Imoveis-Setup-win.zip" -ForegroundColor Green
Write-Host ">> No PC de destino: descompactar e rodar Chavi-Fi-Imoveis-Setup.exe (instale o driver USBasp com Zadig se preciso)."
