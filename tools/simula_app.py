#!/usr/bin/env python3
"""
simula_app.py — SIMULADOR DO APP (diagnóstico do ciclo hibernação↔acionamento).

Reproduz, pelo BLE do Mac, exatamente o que o app-imoveis faz no protocolo
DIRETO — e MEDE cada etapa com timestamp, que é o que os relatos de campo não
conseguem dar:

  1. conecta (a conexão RELIGA a placa cortada, via AFTC do módulo)
  2. dispara TST-PING a cada 300ms e cronometra o TEMPO ATÉ O 1º PONG
     -> esta é a LATÊNCIA DO RELIGAMENTO (boot frio), o número que decide se a
        janela de sonda do app (2,5s) é suficiente
  3. manda o verbo (ABRIR/FECHAR) e cronometra as 2 confirmações "1004.xx"
     (a 1ª na chegada do comando, a 2ª quando o motor PARA)
  4. desconecta e espera a janela ociosa do firmware (padrão 30s) para a
     fechadura CORTAR a energia sozinha
  5. repete N ciclos e imprime a estatística

Uso (com o venv da bancada):
    tools/.venv-bancada/bin/python tools/simula_app.py 003FI002910 [ciclos] [espera_s]

⚠️ Feche o app do celular / desligue o Bluetooth do telefone antes: dois centrais
   disputando a mesma fechadura falseiam a medição.
"""
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner

FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
PROBE_INTERVALO = 0.3      # o app sonda em rajadas; aqui é fino p/ medir
PROBE_TETO = 20.0          # até 20s procurando o PONG (o app desiste em ~2,5s)
CONF_TETO = 20.0           # espera pelas confirmações do verbo


def agora():
    return time.monotonic()


class Sessao:
    def __init__(self, client):
        self.client = client
        self.buf = ""
        self.linhas = []

    def on_notify(self, _sender, data: bytearray):
        txt = data.decode("utf-8", errors="ignore")
        self.buf += txt
        for parte in txt.replace("\r", "\n").split("\n"):
            if parte.strip():
                self.linhas.append((agora(), parte.strip()))

    async def escrever(self, texto):
        await self.client.write_gatt_char(FFE1, texto.encode(), response=False)

    def conta_confirmacoes(self):
        import re
        return len(re.findall(r"[12]0\d{2}", self.buf))


async def achar(alvo, timeout=10.0):
    devs = await BleakScanner.discover(timeout=timeout)
    for d in devs:
        if (d.name or "").upper() == alvo.upper():
            return d.address
    return None


async def um_ciclo(addr, alvo, verbo, n):
    print(f"\n=== CICLO {n} ===")
    t_conn = agora()
    async with BleakClient(addr) as client:
        s = Sessao(client)
        await client.start_notify(FFE1, s.on_notify)
        print(f"  conectado em {agora() - t_conn:.2f}s (isto RELIGA a placa)")

        # ---- FASE 1: quanto tempo até o MCU responder? (religamento/boot) ----
        # "bytes de sacrifício" iguais aos do app (acordam o MCU/módulo)
        await s.escrever("\n")
        await asyncio.sleep(0.15)
        await s.escrever("\n")

        t0 = agora()
        pong_em = None
        enviados = 0
        while agora() - t0 < PROBE_TETO:
            await s.escrever("TST-PING")
            enviados += 1
            fim_janela = agora() + PROBE_INTERVALO
            while agora() < fim_janela:
                if "PONG" in s.buf:
                    pong_em = agora() - t0
                    break
                await asyncio.sleep(0.03)
            if pong_em is not None:
                break

        if pong_em is None:
            print(f"  ✗ SEM PONG em {PROBE_TETO:.0f}s ({enviados} sondas) "
                  f"-> aqui o app teria dado F07")
            await client.stop_notify(FFE1)
            return {"pong": None, "conf1": None, "conf2": None, "sondas": enviados}
        print(f"  ✓ PONG em {pong_em:.2f}s ({enviados} sonda(s))"
              + ("   ⚠️ ACIMA da janela de 2,5s do app!" if pong_em > 2.5 else ""))

        # ---- FASE 2: verbo + confirmações ----
        s.buf = ""
        t1 = agora()
        await s.escrever(verbo)
        conf1 = conf2 = None
        while agora() - t1 < CONF_TETO:
            n_conf = s.conta_confirmacoes()
            if conf1 is None and n_conf >= 1:
                conf1 = agora() - t1
                print(f"  ✓ 1ª confirmação (chegada do comando) em {conf1:.2f}s")
            if n_conf >= 2:
                conf2 = agora() - t1
                print(f"  ✓ 2ª confirmação (motor PAROU) em {conf2:.2f}s")
                break
            await asyncio.sleep(0.05)
        if conf1 is None:
            print(f"  ✗ verbo {verbo} SEM confirmação -> f04/f07 no app")
        elif conf2 is None:
            print("  ⚠️ só 1 confirmação (o giro não reportou o fim; melodia/"
                  "reboot no meio?)")
        cru = " | ".join(t for _, t in s.linhas[-8:])
        print(f"  linhas: {cru}")
        await client.stop_notify(FFE1)
    return {"pong": pong_em, "conf1": conf1, "conf2": conf2, "sondas": enviados}


async def main():
    if len(sys.argv) < 2:
        print(__doc__)
        raise SystemExit(1)
    alvo = sys.argv[1].strip().upper()
    ciclos = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    espera = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0

    print(f"Procurando {alvo}...")
    addr = await achar(alvo)
    if not addr:
        print("NÃO ENCONTRADA no ar."); raise SystemExit(1)
    print(f"Encontrada: {addr}")

    resultados = []
    for i in range(1, ciclos + 1):
        verbo = "ABRIR" if i % 2 else "FECHAR"
        try:
            r = await um_ciclo(addr, alvo, verbo, i)
        except Exception as e:
            print(f"  ✗ EXCEÇÃO no ciclo {i}: {e}")
            r = {"pong": None, "conf1": None, "conf2": None, "sondas": 0}
        resultados.append(r)
        if i < ciclos:
            print(f"  … desconectado; esperando {espera:.0f}s para a fechadura "
                  "CORTAR sozinha (janela ociosa do firmware)")
            await asyncio.sleep(espera)
            # re-scan: cortada, ela continua anunciando (o módulo fica ligado)
            novo = await achar(alvo, timeout=8.0)
            if novo:
                addr = novo

    print("\n===== RESUMO =====")
    ok = [r for r in resultados if r["pong"] is not None]
    print(f"ciclos: {len(resultados)} · com PONG: {len(ok)} · "
          f"sem PONG (F07): {len(resultados) - len(ok)}")
    if ok:
        ps = [r["pong"] for r in ok]
        print(f"latência do religamento (até o 1º PONG): "
              f"min {min(ps):.2f}s · máx {max(ps):.2f}s · "
              f"média {sum(ps)/len(ps):.2f}s")
        acima = [p for p in ps if p > 2.5]
        if acima:
            print(f"⚠️ {len(acima)}/{len(ps)} ciclos passaram da janela de 2,5s "
                  "do app -> é AQUI que nasce o F07 em campo")
    c2 = [r["conf2"] for r in resultados if r["conf2"] is not None]
    if c2:
        print(f"giro completo (2ª confirmação): min {min(c2):.2f}s · "
              f"máx {max(c2):.2f}s")
    faltou2 = [r for r in resultados if r["conf1"] and not r["conf2"]]
    if faltou2:
        print(f"⚠️ {len(faltou2)} ciclo(s) sem a 2ª confirmação (fim do giro) — "
              "compatível com reboot/brownout no meio do motor")


asyncio.run(main())
