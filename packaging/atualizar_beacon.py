#!/usr/bin/env python3
"""Atualiza o BEACON PÚBLICO de versão da bancada (Chavi-team/chavi-bancada-latest,
arquivo latest.json) — a fonte que a bancada lê p/ detectar atualização, já que o
repo do firmware é PRIVADO (API de releases dá 404 sem token).

Roda no GitHub Actions após publicar a release. Precisa do secret BEACON_PAT (PAT
com escrita no repo do beacon). Se o secret não estiver setado, apenas AVISA e sai
0 (não quebra o build). Sem dependências além da stdlib."""
import base64
import json
import os
import re
import sys
import urllib.request

BEACON_REPO = "Chavi-team/chavi-bancada-latest"
BEACON_FILE = "latest.json"
BRANCH = "main"
FW_REPO = "Chavi-team/firmware-imovies-julho-2026"
BANCADA_PY = os.path.join(os.path.dirname(__file__), "..", "tools", "bancada.py")


def _const(nome, default=""):
    """Lê uma constante string de tools/bancada.py (ex.: VERSION_DATE = "...")."""
    try:
        txt = open(BANCADA_PY, encoding="utf-8").read()
    except Exception:
        return default
    m = re.search(rf'{nome}\s*=\s*"([^"]*)"', txt)
    return m.group(1) if m else default


def _api(method, url, token, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "chavi-beacon-updater")
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


def main():
    token = os.environ.get("BEACON_PAT", "").strip()
    # versão vem da tag (bancada-vX.Y.Z); fallback = BANCADA_VERSION do fonte
    ref = os.environ.get("GITHUB_REF_NAME", "")
    m = re.search(r"(\d+\.\d+\.\d+)", ref)
    version = m.group(1) if m else _const("BANCADA_VERSION")
    if not version:
        print("beacon: sem versão (nem tag nem BANCADA_VERSION) — nada a fazer.")
        return 0

    latest = {
        "version": version,
        "date": _const("VERSION_DATE"),
        "firmware": _const("FIRMWARE_VERSION"),
        "notes": _const("VERSION_NOTES"),
        "url": f"https://github.com/{FW_REPO}/releases/tag/{ref or ('bancada-v' + version)}",
    }
    conteudo = json.dumps(latest, ensure_ascii=False, indent=2) + "\n"
    print("beacon: latest.json =\n" + conteudo)

    if not token:
        print("⚠️ BEACON_PAT não configurado — pulei a atualização do beacon "
              "(o build da release segue normal). Configure o secret p/ automatizar.")
        return 0

    base = f"https://api.github.com/repos/{BEACON_REPO}/contents/{BEACON_FILE}"
    sha = None
    try:
        atual = _api("GET", f"{base}?ref={BRANCH}", token)
        sha = atual.get("sha")
    except Exception as e:
        print(f"beacon: latest.json ainda não existe? ({e}) — vou criar.")
    payload = {
        "message": f"beacon: bancada v{version}",
        "content": base64.b64encode(conteudo.encode()).decode(),
        "branch": BRANCH,
    }
    if sha:
        payload["sha"] = sha
    _api("PUT", base, token, payload)
    print(f"✓ beacon atualizado para v{version}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
