#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
baixar_avrdude.py — Baixa o avrdude STANDALONE (self-contained) da release
oficial para a plataforma ATUAL e coloca em packaging/avrdude/<plat>/ no
formato que o chavi_fi_setup.spec espera (avrdude[.exe] + avrdude.conf).

Cross-platform (roda em runner Windows e macOS do GitHub Actions).
Uso:  python packaging/baixar_avrdude.py
"""
import io
import os
import sys
import tarfile
import zipfile
import urllib.request

VER = "8.0"
BASE = f"https://github.com/avrdudes/avrdude/releases/download/v{VER}"
HERE = os.path.dirname(os.path.abspath(__file__))


def baixar(url):
    print(f">> baixando {url}")
    with urllib.request.urlopen(url) as r:
        return r.read()


def main():
    if sys.platform == "darwin":
        plat, exe = "mac", "avrdude"
        data = baixar(f"{BASE}/avrdude_v{VER}_macOS_64bit.tar.gz")
        tf = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
        membros = {os.path.basename(m.name): m for m in tf.getmembers() if m.isfile()}
        binm, confm = membros.get("avrdude"), membros.get("avrdude.conf")
    elif sys.platform.startswith("win"):
        plat, exe = "win", "avrdude.exe"
        data = baixar(f"{BASE}/avrdude_v{VER}_Windows_64bit.zip")
        zf = zipfile.ZipFile(io.BytesIO(data))
        membros = {os.path.basename(n): n for n in zf.namelist()}
        binm, confm = membros.get("avrdude.exe"), membros.get("avrdude.conf")
    else:
        print("plataforma não suportada p/ este build"); sys.exit(1)

    dest = os.path.join(HERE, "avrdude", plat)
    os.makedirs(dest, exist_ok=True)

    if sys.platform == "darwin":
        with open(os.path.join(dest, exe), "wb") as f:
            f.write(tf.extractfile(binm).read())
        with open(os.path.join(dest, "avrdude.conf"), "wb") as f:
            f.write(tf.extractfile(confm).read())
        os.chmod(os.path.join(dest, exe), 0o755)
    else:
        with open(os.path.join(dest, exe), "wb") as f:
            f.write(zf.read(binm))
        with open(os.path.join(dest, "avrdude.conf"), "wb") as f:
            f.write(zf.read(confm))
        # DLLs que venham juntas (libusb etc.)
        for n in membros.values():
            if n.lower().endswith(".dll"):
                with open(os.path.join(dest, os.path.basename(n)), "wb") as f:
                    f.write(zf.read(n))

    print(f">> ok: {dest}")


if __name__ == "__main__":
    main()
