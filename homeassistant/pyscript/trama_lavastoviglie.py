"""Registrazione su file del bus della lavastoviglie (17/11/2026), come trama_climatizzatore.py per il clima.

Lo sniffer (M5 Atom, firmware dishSniffer 1.8+, sempre e solo in ascolto tra la scheda e il modulo Wi-Fi Samsung) manda:
  dish/capture  «<millis> G22|G19 <gap_ms> <byte esadecimali>»   (G22 = scheda→modulo, filo giallo; G19 = modulo→scheda, nero)
  dish/log      una riga per ogni byte dello stato che cambia, per i comandi del modulo e per gli eventi della porta
Qui tutto finisce in /config/lavastoviglie/catture/AAAAMMGG.txt (un file al giorno), con l'ora di Home Assistant davanti
a ogni riga: serve a decodificare il protocollo (fasi del lavaggio, sale, brillantante, partenza ritardata…).
I file più vecchi di 30 giorni vengono cancellati.
"""
import os
import time
from homeassistant.util import dt as dt_util

CARTELLA = "/config/lavastoviglie/catture"
GIORNI_DA_TENERE = 30


@pyscript_compile
def _scrivi(percorso, testo):
    os.makedirs(os.path.dirname(percorso), exist_ok=True)
    with open(percorso, "a") as f:
        f.write(testo)


@pyscript_compile
def _pulisci(cartella, giorni):
    if not os.path.isdir(cartella):
        return
    limite = time.time() - giorni * 86400
    for n in os.listdir(cartella):
        p = os.path.join(cartella, n)
        if n.endswith(".txt") and os.path.getmtime(p) < limite:
            os.remove(p)


def _file():
    return f"{CARTELLA}/{dt_util.now().strftime('%Y%m%d')}.txt"


@mqtt_trigger("dish/capture")
def trama_lavastoviglie(payload=None):
    if payload:
        task.executor(_scrivi, _file(), f"{dt_util.now().strftime('%H:%M:%S')} {payload.strip()}\n")


@mqtt_trigger("dish/log")
def registro_lavastoviglie(payload=None):
    if payload:
        task.executor(_scrivi, _file(), f"{dt_util.now().strftime('%H:%M:%S')} LOG {payload.strip()}\n")


@time_trigger("startup", "cron(17 4 * * *)")
def pulizia_catture_lavastoviglie():
    task.executor(_pulisci, CARTELLA, GIORNI_DA_TENERE)
