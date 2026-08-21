# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec do "Chavi-Fi-Imoveis-Setup" — bancada de gravação portátil.
#
# Empacota o tools/bancada.py + o firmware .hex pré-compilado + o avrdude
# standalone numa pasta única que roda SEM instalar Python/deps.
# Build: pyinstaller packaging/chavi_fi_setup.spec  (rode dentro de firmware/)
import os
import sys

ROOT = os.path.abspath(os.path.join(os.getcwd()))          # .../firmware
TOOLS = os.path.join(ROOT, "tools")
BIN = os.path.join(ROOT, "bin")
PKG = os.path.join(ROOT, "packaging")

datas = []
# firmware pré-compilado (o pacote NÃO compila — grava o .hex embutido).
# Procura no bin/ (build local) e, senão, no packaging/firmware/ VERSIONADO no
# git — assim o build Windows funciona sem arduino-cli (o .hex viaja no repo).
hexf = os.path.join(BIN, "chavi_fi.ino.hex")
if not os.path.exists(hexf):
    hexf = os.path.join(PKG, "firmware", "chavi_fi.ino.hex")
if os.path.exists(hexf):
    datas.append((hexf, "."))
else:
    raise SystemExit("chavi_fi.ino.hex não encontrado (bin/ nem packaging/firmware/)")

# ⭐ 21/08/2026 — TRAVA: o .hex tem de ser o do FW_VERSION atual.
# O bin/ é gitignorado, então no CI só existe o packaging/firmware/ versionado.
# Ele ficou parado na v2.21.0 enquanto o fonte foi até a 2.27.0, e por ~3
# semanas TODA bancada publicada gravou 2.21.0 sem ninguém perceber (a versão
# no cabeçalho é a DA BANCADA, não a do firmware). Agora o build quebra aqui.
import re as _re
_ino = os.path.join(ROOT, "chavi_fi", "chavi_fi.ino")
if os.path.exists(_ino):
    with open(_ino, encoding="utf-8", errors="replace") as _f:
        _m = _re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', _f.read())
    if _m:
        _esperada = _m.group(1)
        _dados = bytearray()
        with open(hexf, encoding="ascii", errors="replace") as _f:
            for _l in _f:
                _l = _l.strip()
                if _l.startswith(":") and int(_l[7:9], 16) == 0:
                    _dados += bytes.fromhex(_l[9:9 + int(_l[1:3], 16) * 2])
        if _esperada not in _re.findall(r"\b\d+\.\d+\.\d+\b", _dados.decode("latin1")):
            raise SystemExit(
                f"ABORTADO: {hexf} nao contem FW_VERSION {_esperada} do fonte.\n"
                "Recompile e rode: cp bin/chavi_fi.ino.hex packaging/firmware/chavi_fi.ino.hex"
            )

# avrdude standalone + avrdude.conf, se preparado em packaging/avrdude/<plat>/
plat = "win" if sys.platform.startswith("win") else ("mac" if sys.platform == "darwin" else "linux")
avr_dir = os.path.join(PKG, "avrdude", plat)
if os.path.isdir(avr_dir):
    datas.append((avr_dir, "avrdude"))

block_cipher = None

# Coleta TUDO do bleak e do backend CoreBluetooth (pyobjc): o connect no macOS
# carrega submódulos/dylibs que o autodetect do PyInstaller perde -> o scan
# funciona mas o connect quebra ("Error -3 while decompressing"/backend faltando).
from PyInstaller.utils.hooks import collect_all
_hidden, _datas, _bins = [], list(datas), []
for _pkg in ("bleak", "CoreBluetooth", "libdispatch", "objc",
             "Foundation", "AppKit", "WebKit"):
    try:
        b, d, h = collect_all(_pkg)
        _bins += b; _datas += d; _hidden += h
    except Exception:
        pass

a = Analysis(
    [os.path.join(TOOLS, "bancada.py")],
    pathex=[TOOLS],
    binaries=_bins,
    datas=_datas,
    hiddenimports=_hidden + ["bleak", "serial", "serial.tools.list_ports", "requests",
                   "webview", "webview.platforms.cocoa", "webview.platforms.winforms",
                   "objc", "Foundation", "WebKit", "AppKit", "CoreBluetooth", "libdispatch"],
    hookspath=[],
    runtime_hooks=[],
    excludes=["tkinter", "matplotlib", "numpy", "PIL"],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)
pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="Chavi-Fi-Imoveis-Setup",
    debug=False, bootloader_ignore_signals=False, strip=False,
    upx=True, console=False,   # janela nativa (webview) — sem terminal preto
    disable_windowed_traceback=False, argv_emulation=False,
    target_arch=None, codesign_identity=None, entitlements_file=None,
    icon=os.path.join(PKG, "icon.ico"),   # ícone do .exe (Windows); inócuo no Mac
)
coll = COLLECT(
    exe, a.binaries, a.zipfiles, a.datas,
    strip=False, upx=True, upx_exclude=[],
    name="Chavi-Fi-Imoveis-Setup",
)

# No macOS, empacota num BUNDLE .app (ícone no Dock, sem terminal preto).
if sys.platform == "darwin":
    app = BUNDLE(
        coll,
        name="Chavi-Fi-Imoveis-Setup.app",
        icon=os.path.join(PKG, "icon.icns"),
        bundle_identifier="com.chavi.fi.setup",
        info_plist={
            "NSHighResolutionCapable": True,
            "LSBackgroundOnly": False,
            "NSBluetoothAlwaysUsageDescription":
                "Testar as fechaduras por Bluetooth na bancada.",
            "CFBundleName": "Chavi FI Setup",
        },
    )
