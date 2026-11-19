"""Lavastoviglie: consumo di energia per la plancia Energia, e se sta lavando davvero. Tutto dall'Atom sul bus.

18/11/2026: niente più SmartThings. Il consumo arriva dalla scheda della lavastoviglie (contatore 1A0B = energia del lavaggio
in corso, in decine di Wh, decodificato dall'Atom: sensor.lavastoviglie_sniffer_bus_wi_fi_energia_lavaggio_in_corso, 1,50 kWh
a fine Intensivo 65° del 17-18/11, confermato da SmartThings). 19/11/2026: CORRETTO, il contatore della scheda NON riparte
da zero a ogni lavaggio: è un totale (riparte da 0 solo quando il modulo Wi-Fi viene resettato). Qui si sommano solo gli
aumenti avvenuti mentre lava, in un totale che non torna mai indietro (pyscript.lavastoviglie_energia_totale,
salvato anche dopo un riavvio), pubblicato come sensor.lavastoviglie_energia (total_increasing) per la plancia Energia.
TOTALE_INIZIALE: da dove parte il totale (es. l'ultimo valore che avevi da SmartThings, per avere uno storico continuo).

sensor.lavastoviglie_energia_lavaggio (19/11/2026): l'energia del lavaggio in corso = contatore della scheda adesso − contatore
quando il bus è passato a «In funzione» (pyscript.lavastoviglie_energia_inizio, salvato). A fine lavaggio resta l'ultimo valore.

binary_sensor.lavastoviglie_in_funzione: «on» mentre il bus dice «In funzione»; se l'Atom non risponde resta l'ultimo stato.
Storia: dal 13/11/2026 leggeva i consumi mensili da SmartThings (samsungce.consumedEnergy); dal 17/11 «in funzione» dal bus.
"""
SENSORE = "sensor.lavastoviglie_energia"
IN_FUNZIONE = "binary_sensor.lavastoviglie_in_funzione"
BUS_STATO = "sensor.lavastoviglie_sniffer_bus_wi_fi_stato_bus"
BUS_MINUTI = "sensor.lavastoviglie_sniffer_bus_wi_fi_minuti_alla_fine_bus"
BUS_ENERGIA = "sensor.lavastoviglie_sniffer_bus_wi_fi_energia_lavaggio_in_corso"
TOTALE = "pyscript.lavastoviglie_energia_totale"
ULTIMO_BUS = "pyscript.lavastoviglie_energia_ultimo_bus"

TOTALE_INIZIALE = "0"   # kWh di partenza (es. ultimo valore SmartThings)
state.persist(TOTALE, default_value=TOTALE_INIZIALE)
state.persist(ULTIMO_BUS, default_value="0")
LAVAGGIO = "sensor.lavastoviglie_energia_lavaggio"
INIZIO = "pyscript.lavastoviglie_energia_inizio"
state.persist(INIZIO, default_value="0")


def _numero(entita, ripiego=0.0):
    try:
        return float(state.get(entita))
    except (ValueError, TypeError, NameError):
        return ripiego


def _pubblica():
    state.set(SENSORE, round(_numero(TOTALE, 0.0), 3), {
        "friendly_name": "Lavastoviglie energia",
        "device_class": "energy",
        "state_class": "total_increasing",
        "unit_of_measurement": "kWh",
        "icon": "mdi:dishwasher",
        "fonte": "bus (Atom)",
    })
    bus = _numero(BUS_ENERGIA, -1.0)
    inizio = _numero(INIZIO, 0.0)
    if bus >= 0:
        lavaggio = bus - inizio if bus >= inizio else bus     # contatore azzerato (reset del modulo): conta da zero
        state.set(LAVAGGIO, round(max(lavaggio, 0.0), 2), {
            "friendly_name": "Lavastoviglie energia del lavaggio",
            "device_class": "energy",
            "unit_of_measurement": "kWh",
            "icon": "mdi:lightning-bolt",
            "contatore_scheda_kwh": bus,
            "contatore_a_inizio_lavaggio_kwh": inizio,
        })


@state_trigger(f"{BUS_STATO} == 'In funzione'")
def lavastoviglie_energia_inizio_lavaggio(var_name=None, value=None, old_value=None):
    """All'avvio di un lavaggio si segna il contatore della scheda: il lavaggio in corso conta da lì."""
    if old_value in ("In funzione", "unknown", "unavailable", None, ""):
        return   # l'Atom si è solo ricollegato a lavaggio già partito
    bus = _numero(BUS_ENERGIA, -1.0)
    if bus >= 0:
        state.set(INIZIO, bus)
    _pubblica()


@time_trigger("startup")
def lavastoviglie_energia_avvio():
    _pubblica()


@service
@state_trigger(BUS_ENERGIA)
def lavastoviglie_energia_aggiorna(value=None):
    """Somma al totale l'energia consumata dal lavaggio in corso (solo mentre lava)."""
    nuovo = _numero(BUS_ENERGIA, -1.0) if value is None else float(value) if str(value).replace('.', '', 1).isdigit() else -1.0
    if nuovo < 0:
        return
    ultimo = _numero(ULTIMO_BUS, 0.0)
    if state.get(BUS_STATO) == "In funzione":
        aumento = nuovo - ultimo if nuovo >= ultimo else nuovo     # sceso = nuovo lavaggio: conta da zero
        if aumento > 0:
            state.set(TOTALE, round(_numero(TOTALE, 0.0) + aumento, 3))
    state.set(ULTIMO_BUS, nuovo)
    _pubblica()


@service
@time_trigger("startup", "cron(*/2 * * * *)")
@state_trigger(BUS_STATO, BUS_MINUTI)
def lavastoviglie_in_funzione_aggiorna():
    """Sta lavando? Dal bus (Atom): stato «In funzione». Se l'Atom non risponde resta l'ultimo stato noto."""
    stato_bus = state.get(BUS_STATO) if state.exist(BUS_STATO) else "unknown"
    if stato_bus in ("unknown", "unavailable", None, ""):
        return
    lavando = stato_bus == "In funzione"
    rimanenti = int(_numero(BUS_MINUTI, 0)) if lavando else 0
    state.set(IN_FUNZIONE, "on" if lavando else "off", {
        "friendly_name": "Lavastoviglie in funzione",
        "device_class": "running",
        "icon": "mdi:dishwasher",
        "minuti_rimanenti": rimanenti,
        "fonte": "bus (Atom)",
    })
