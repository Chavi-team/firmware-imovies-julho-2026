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
# firmware pré-compilado (o pacote NÃO compila — grava o .hex embutido)
hexf = os.path.join(BIN, "chavi_fi.ino.hex")
if os.path.exists(hexf):
    datas.append((hexf, "."))

# avrdude standalone + avrdude.conf, se preparado em packaging/avrdude/<plat>/
plat = "win" if sys.platform.startswith("win") else ("mac" if sys.platform == "darwin" else "linux")
avr_dir = os.path.join(PKG, "avrdude", plat)
if os.path.isdir(avr_dir):
    datas.append((avr_dir, "avrdude"))

block_cipher = None

a = Analysis(
    [os.path.join(TOOLS, "bancada.py")],
    pathex=[TOOLS],
    binaries=[],
    datas=datas,
    hiddenimports=["bleak", "serial", "serial.tools.list_ports", "requests"],
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
    upx=True, console=True,
    disable_windowed_traceback=False, argv_emulation=False,
    target_arch=None, codesign_identity=None, entitlements_file=None,
)
coll = COLLECT(
    exe, a.binaries, a.zipfiles, a.datas,
    strip=False, upx=True, upx_exclude=[],
    name="Chavi-Fi-Imoveis-Setup",
)
