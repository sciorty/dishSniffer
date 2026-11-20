"""Archivio dei rapporti di fine lavaggio della lavastoviglie (20/11/2026).

Ogni volta che l'Atom riceve il rapporto di fine ciclo (sensor.…_ultimo_lavaggio_dati_grezzi: «70=… 71=… … 94=…»)
aggiunge una riga a /config/lavastoviglie/rapporti.txt con data, ora, programma e tutte le voci. Sempre, anche con la
registrazione delle trame spenta: una riga per lavaggio, pochi byte. Serve a capire le voci ancora sconosciute quando
la lavastoviglie segnala qualcosa (es. sale da rabboccare): si confronta il rapporto con quelli prima.
Voci già capite: 80 lavaggi completati, 83 brillantante da rabboccare (1), 86/87/88 temperature (decimi di °C), 93 Wh.
"""
import os
from homeassistant.util import dt as dt_util

GREZZI = "sensor.lavastoviglie_sniffer_bus_wi_fi_ultimo_lavaggio_dati_grezzi"
PROGRAMMA = "sensor.lavastoviglie_sniffer_bus_wi_fi_programma_bus"
FILE = "/config/lavastoviglie/rapporti.txt"


@pyscript_compile
def _aggiungi(percorso, riga):
    os.makedirs(os.path.dirname(percorso), exist_ok=True)
    with open(percorso, "a") as f:
        f.write(riga + "\n")


@state_trigger(GREZZI)
def lavastoviglie_archivia_rapporto(value=None, old_value=None):
    if not value or value in ("unknown", "unavailable") or value == old_value:
        return
    quando = dt_util.now().strftime("%d/%m/%Y %H:%M")
    programma = state.get(PROGRAMMA) if state.exist(PROGRAMMA) else "?"
    task.executor(_aggiungi, FILE, f"{quando} | {programma} | {value}")
