// dishSniffer — controllo locale della lavastoviglie Samsung DW60A8050 (scheda Midea, protocollo UART «E1») dal bus
// tra la scheda madre e il modulo Wi-Fi Samsung CWAP210M. M5Stack Atom Lite (ESP32), firmware 2.4 (18/11/2026).
// Local control of a Samsung DW60A8050 dishwasher (Midea board, «E1» UART protocol) by tapping the bus between the main
// board and the Samsung CWAP210M Wi-Fi module. See README.md and docs/PROTOCOL.md.
//
// L'Atom sta IN PARALLELO sul connettore CON1 (il modulo Samsung resta collegato e SmartThings continua a funzionare),
// fili diretti, senza resistenze:
//   giallo  (TX scheda→modulo)  → G22   ascolto: stato, eventi della porta, contatori, rapporto di fine ciclo
//   nero    (TX modulo→scheda)  → G19   ascolto dei comandi del modulo; dalla 2.1 anche trasmissione (open-drain)
//   marrone (sempre alto)       → G23   solo livello e numero di cambi
//   rosso   (GND)               → GND
//   bianco  (+5 V)              → 5V    l'Atom si alimenta dal bus
// Trasmissione (2.1+): G19 passa in open-drain con la corrente minima solo durante l'invio e solo dopo 1 s di bus zitto;
// la linea nera ha una resistenza di pull-up verso 5 V (verificato con il comando «sonda»), quindi l'Atom può solo
// tirarla giù, mai spingerla contro il modulo. ATTENZIONE: i pin dell'ESP32 non sono dichiarati tolleranti ai 5 V;
// qui funziona così, ma lo fai a tuo rischio (una resistenza in serie da 1–10 kΩ è più prudente).
// NON mettere resistenze verso massa sulle linee: il partitore 1k+2k le caricava e la scheda smetteva di rispondere.
//
// Baud e inversione sono regolabili da Home Assistant senza riflashare.

#include <M5Atom.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

#define FW_VERSION     "2.4 (18/11/2026)"
#ifndef AIO_SERVER
#define AIO_SERVER     "homeassistant.local"   // indirizzo del broker MQTT: meglio definirlo in secrets.h
#endif
#define AIO_SERVERPORT 1883
#define NAME           "dishSniffer/lavastoviglie"
#define OTA_HOSTNAME   "dishsniffer"

#define PIN_SM     22         // giallo: scheda → modulo
#define PIN_MS     19         // nero: modulo → scheda (e trasmissione dell'Atom)
#define PIN_GIALLO 23         // marrone: linea fissa (il nome «giallo» è storico)
#define TX_FINTO_2 26         // TX non usati delle due seriali (Grove): lasciare scollegati
#define TX_FINTO_1 32
#define GAP_MS     6          // silenzio sul bus che separa due trame
#define BUF_MAX    512

// topic
#define cap_topic    "dish/capture"      // «<millis> SM|MS <gap_ms> <hex>» oppure «<millis> GIALLO 0|1»
#define last_topic   "dish/last"         // ultima trama, con la direzione davanti
#define giallo_topic "dish/giallo"       // ON/OFF
#define stats_topic  "dish/stats"
#define stato_topic  "dish/stato"        // stato decodificato (JSON, retained)
#define log_topic    "dish/log"
#define energia_topic "dish/energia"     // energia del lavaggio in corso (contatore 1A0B, retained)
#define ciclo_topic  "dish/ciclo"        // rapporto di fine ciclo (retained)          // una riga per ogni byte che cambia, comandi, eventi
#define avail_topic  "dish/status"
#define tx_cmd       "dish/tx/cmd"           // 2.1: «sonda» | «stato» (mai retained)
#define tx_esito     "dish/tx/esito"         // 2.1: esito JSON (retained)
#define cap_cmd      "dish/capture/cmd"      // ON/OFF: pubblica le trame (default OFF, per non intasare MQTT)
#define cap_state    "dish/capture/state"
#define baud_cmd     "dish/cfg/baud"         // baud (retained), default 9600
#define baud_state   "dish/cfg/baud/state"
#define inv_cmd      "dish/cfg/invert"       // 0/1 (retained): inverte il segnale UART
#define inv_state    "dish/cfg/invert/state"

WiFiClient net;
PubSubClient client(net);

uint32_t baudRate = 9600;
bool     invert = false;
bool     capturing = true;          // 1.8: registrazione sempre attiva (HA la salva su file: pyscript/trama_lavastoviglie.py)

struct Verso {
  HardwareSerial *ser;
  const char *tag;
  uint8_t buf[BUF_MAX];
  uint16_t idx;
  unsigned long lastByte, frames;
};
Verso versi[2] = { {&Serial2, "G22", {0}, 0, 0, 0}, {&Serial1, "G19", {0}, 0, 0, 0} };

unsigned long lastStats = 0, bytesTot = 0;
int giallo = -1;                 // ultimo livello pubblicato
unsigned long gialloCambi = 0, gialloUltimaPub = 0;
int gialloLetto = 0;

// Diagnostica elettrica dei due pin dati, indipendente dalla seriale: quante commutazioni (fronti) e per quanta parte del
// tempo il pin è alto. Linea viva ma muta = 100 % alto e 0 fronti; bloccata in basso = ~0 % alto; dati a un'altra
// velocità = fronti > 0 ma nessuna trama.
volatile uint32_t fronti[2] = {0, 0};
void IRAM_ATTR isrSM() { fronti[0]++; }
void IRAM_ATTR isrMS() { fronti[1]++; }
uint32_t campioni = 0, alti[2] = {0, 0};

void startUART()
{
  Serial2.end();
  Serial1.end();
  // begin(baud, config, rxPin, txPin, invert): leggiamo soltanto; i TX vanno su pin non collegati
  Serial2.begin(baudRate, SERIAL_8N1, PIN_SM, TX_FINTO_2, invert);
  Serial1.begin(baudRate, SERIAL_8N1, PIN_MS, TX_FINTO_1, invert);       // 1.5: di nuovo G19 (nero, trasmissione della scheda)
  for (Verso &v : versi) v.idx = 0;
}

void hexString(const uint8_t *b, uint16_t n, char *out)
{
  const char *h = "0123456789ABCDEF";
  for (uint16_t i = 0; i < n; i++) { out[i * 3] = h[b[i] >> 4]; out[i * 3 + 1] = h[b[i] & 0xF]; out[i * 3 + 2] = ' '; }
  out[n ? n * 3 - 1 : 0] = 0;
}

// ---------------------------------------------------------------- decodifica Midea E1 (lavastoviglie)
// Trama: AA len E1 sync 00 00 msgid 00 00 tipo corpo… checksum, checksum = (~somma(byte 1..n-2))+1.
// Stato (corpo di 24 byte, corpo[0]=01, nei tipi 02/03/04 della scheda): c1 stato 01 ferma / 02 in funzione;
// c5-c6 minuti alla fine; c9 armata da remoto; c16 fase; c19 programma; c20 opzione. Porta: tipo 04, corpo 03 00 XX.
// 2.1: prove di trasmissione (vedi «prove di trasmissione sul NERO» più sotto)
int  sondaEsito = -1;                 // -1 non provata, 0 non scende, 1 scende
char txRichiesta = 0;                 // 's' sonda, 'q' domanda di stato, 'c' comando (2.2)
uint8_t cmdCorpo[2] = {0, 0}; char cmdNome[24] = ""; uint8_t cmdFrame[17];
unsigned long txChiesta = 0, txInviata = 0;
bool txAttesa = false, txEco = false, txRisposta = false;
const uint8_t DOMANDA_STATO[] = {0xAA, 0x12, 0xE1, 0, 0, 0, 0, 0, 0, 0x03, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x09};
uint8_t st[24]; bool haveSt = false, stCambiato = false;
unsigned long ultimoStato = 0;
int porta = -1;                       // 0 chiusa, 1 aperta, 2 in lavaggio (bloccata)
struct Altro { uint8_t dir, tipo, sotto; uint32_t firma; };
Altro altri[12]; uint8_t nAltri = 0;

void logLine(const char *s) { if (client.connected()) client.publish(log_topic, s); }

const char *nomeProgramma(uint8_t p)
{
  switch (p) { case 0x01: return "Auto 45-65°"; case 0x02: return "Intensivo 65°"; case 0x04: return "Eco 50°";
               case 0x05: return "Delicato 40°"; case 0x08: return "Prelavaggio"; case 0x09: return "Express 60°";
               case 0x0D: return "Extra silenzioso"; case 0x10: return "Pulizia vasca"; default: return NULL; }
}
const char *nomeFase(uint8_t f)
{
  // 18/11/2026: fasi dal lavaggio Intensivo 65° della notte 17-18/11 (prelavaggio 20 min, lavaggio 82, risciacquo 55, asciugatura 38)
  switch (f) { case 0x00: return "Nessuna"; case 0x01: return "Prelavaggio"; case 0x02: return "Lavaggio"; case 0x03: return "Risciacquo";
               case 0x04: return "Asciugatura"; case 0x05: return "Fine"; case 0x06: return "In attesa (partenza ritardata)";
               case 0x07: return "Scarico"; default: return NULL; }
}
// cestelli (stato byte 20; comando 05 XX): 01 entrambi, 04 solo inferiore (comando 05 00 / 05 02)
const char *nomeCestelli(uint8_t z) { switch (z) { case 0x01: return "Entrambi"; case 0x04: return "Solo inferiore"; case 0x02: return "Solo superiore?"; default: return NULL; } }
// 2.4 (18/11/2026): 03 = in pausa (comando 02 02; 02 03 riprende). ATTENZIONE: il comando 04 XX fa PARTIRE un lavaggio
// (provato 04 01: Auto 45-65° avviato, annullato subito); 0A XX cambia il byte 23 dello stato (opzione da capire).
const char *nomeStato(uint8_t x) { switch (x) { case 0x01: return "Ferma"; case 0x02: return "In funzione"; case 0x03: return "In pausa"; default: return NULL; } }
const char *nomePorta() { switch (porta) { case 0: return "chiusa"; case 1: return "aperta"; case 2: return "bloccata (lavaggio)"; default: return "?"; } }

void pubblicaStato()
{
  if (!haveSt || !client.connected()) return;
  char raw[24 * 3 + 1]; hexString(st, 24, raw);
  char prog[28], fase[20], stato[20], cest[20];
  const char *n;
  if ((n = nomeProgramma(st[19]))) snprintf(prog, sizeof(prog), "%s", n); else snprintf(prog, sizeof(prog), "codice %02X", st[19]);
  if ((n = nomeFase(st[16])))      snprintf(fase, sizeof(fase), "%s", n); else snprintf(fase, sizeof(fase), "fase %02X", st[16]);
  if (st[1] == 0x02 && st[16] == 0x06) snprintf(stato, sizeof(stato), "Partenza ritardata");   // byte 1 = 02 anche in attesa
  else if ((n = nomeStato(st[1]))) snprintf(stato, sizeof(stato), "%s", n); else snprintf(stato, sizeof(stato), "stato %02X", st[1]);
  if ((n = nomeCestelli(st[20])))  snprintf(cest, sizeof(cest), "%s", n); else snprintf(cest, sizeof(cest), "codice %02X", st[20]);
  char j[600];
  snprintf(j, sizeof(j), "{\"stato\":%u,\"stato_nome\":\"%s\",\"programma\":%u,\"programma_nome\":\"%s\",\"minuti\":%u,"
           "\"fase\":%u,\"fase_nome\":\"%s\",\"armata\":%u,\"opzione\":\"%02X\",\"cestelli\":\"%s\",\"ritardo\":%u,\"booster\":%u,"
           "\"avanzamento\":%u,\"trascorsi\":%u,\"porta\":\"%s\",\"raw\":\"%s\"}",
           st[1], stato, st[19], prog, (unsigned)(st[5] * 256 + st[6]), st[16], fase, st[9] ? 1 : 0, st[20], cest,
           (unsigned)(st[10] * 256 + st[11]), st[21] & 0x01, st[2], (unsigned)(st[3] * 256 + st[4]), nomePorta(), raw);   // 2.0: byte 2 avanzamento %, 3-4 minuti trascorsi   // 1.9: byte 10-11 ritardo (min), byte 21 Speed Booster
  client.publish(stato_topic, j, true);
  ultimoStato = millis(); stCambiato = false;
}

void nuovoStato(const uint8_t *b)
{
  if (haveSt)
    for (int i = 0; i < 24; i++)
      if (b[i] != st[i])
      {
        stCambiato = true;
        if (i == 5 || i == 6) continue;          // i minuti scendono da soli durante il lavaggio: niente registro
        char l[64]; snprintf(l, sizeof(l), "stato byte %d: %02X -> %02X", i, st[i], b[i]); logLine(l);
      }
  else stCambiato = true;
  memcpy(st, b, 24); haveSt = true;
}

// registra una volta sola ogni messaggio «diverso dal solito» (per tipo e sotto-tipo)
void altroMessaggio(uint8_t dir, const uint8_t *f, uint8_t n)
{
  uint8_t tipo = f[9], sotto = n > 11 ? f[10] : 0;
  uint32_t firma = 2166136261UL; for (uint8_t i = 10; i < n - 1; i++) firma = (firma ^ f[i]) * 16777619UL;
  for (uint8_t k = 0; k < nAltri; k++)
    if (altri[k].dir == dir && altri[k].tipo == tipo && altri[k].sotto == sotto)
    { if (altri[k].firma == firma) return; altri[k].firma = firma; goto scrivi; }
  if (nAltri < 12) altri[nAltri++] = {dir, tipo, sotto, firma};
scrivi:
  char hex[60]; uint8_t m = n - 11 > 18 ? 18 : n - 11; hexString(f + 10, m, hex);
  char l[120]; snprintf(l, sizeof(l), "%s tipo %02X: %s", dir ? "modulo" : "scheda", tipo, hex); logLine(l);
}

// 2.0 (18/11/2026): contatore 1A0B = energia del lavaggio in corso in decine di Wh (19, 81, 120, 150 a ogni fase:
// 1,50 kWh a fine Intensivo 65°, confermato da SmartThings +1,33 kWh a scatti di 0,33).
long energiaWh = -1;
void contatori(const uint8_t *c, uint8_t clen)
{
  for (uint8_t k = 1; k + 3 < clen; k += 4)
    if (c[k] == 0x1A && c[k + 1] == 0x0B)
    {
      long wh = (long)(c[k + 2] * 256 + c[k + 3]) * 10;
      if (wh != energiaWh && client.connected())
      {
        energiaWh = wh;
        char j[64]; snprintf(j, sizeof(j), "{\"kwh\":%.2f}", wh / 1000.0);
        client.publish(energia_topic, j, true);
      }
    }
}

// Rapporto di fine ciclo: 4 messaggi «0A seq 46 …» (seq 1-4); i dati, messi in fila, sono: tipo, numero di voci, poi
// le voci «etichetta, lunghezza, valore». Visto: 93 = energia in Wh; 87/88/86 = temperature in decimi di °C (65,0 / 58,0 / 23,0
// in un Intensivo 65°: da confermare con altri programmi); 70-73, 80-85, 89-92, 94 da capire.
uint8_t rapporto[160]; uint8_t lunRapporto = 0;
void finisciRapporto()
{
  if (lunRapporto < 2 || !client.connected()) return;
  uint8_t voci = rapporto[1]; uint16_t i = 2;
  long energia = -1, t86 = -1, t87 = -1, t88 = -1;
  static char campi[400]; int m = 0; campi[0] = 0;
  for (uint8_t v = 0; v < voci && i + 1 < lunRapporto; v++)
  {
    uint8_t tag = rapporto[i], len = rapporto[i + 1];
    if (i + 2 + len > lunRapporto) break;
    unsigned long val = 0; for (uint8_t k = 0; k < len && k < 4; k++) val = (val << 8) | rapporto[i + 2 + k];
    if (tag == 0x93) energia = val; else if (tag == 0x86) t86 = val; else if (tag == 0x87) t87 = val; else if (tag == 0x88) t88 = val;
    m += snprintf(campi + m, sizeof(campi) - m, "%s%02X=%lu", m ? " " : "", tag, val);
    i += 2 + len;
  }
  char j[560];
  snprintf(j, sizeof(j), "{\"energia_kwh\":%.3f,\"t86\":%.1f,\"t87\":%.1f,\"t88\":%.1f,\"campi\":\"%s\"}",
           energia >= 0 ? energia / 1000.0 : -1.0, t86 >= 0 ? t86 / 10.0 : -1.0, t87 >= 0 ? t87 / 10.0 : -1.0, t88 >= 0 ? t88 / 10.0 : -1.0, campi);
  client.publish(ciclo_topic, j, true);
  logLine("rapporto di fine ciclo ricevuto");
}

void parteRapporto(const uint8_t *c, uint8_t clen)
{
  if (clen < 3) return;
  uint8_t seq = c[1];
  if (seq == 1) lunRapporto = 0;
  for (uint8_t k = 3; k < clen && lunRapporto < sizeof(rapporto); k++) rapporto[lunRapporto++] = c[k];
  if (seq == 4) finisciRapporto();
}

void trattaTrama(uint8_t dir, const uint8_t *f, uint8_t n)   // dir 0 = scheda→modulo (G22), 1 = modulo→scheda (G19)
{
  uint8_t tipo = f[9]; const uint8_t *c = f + 10; uint8_t clen = n - 11;
  if (dir == 0)
  {
    if ((tipo == 0x02 || tipo == 0x03 || tipo == 0x04) && clen >= 24 && c[0] == 0x01) { nuovoStato(c); return; }
    if (tipo == 0x04 && clen >= 3 && c[0] == 0x03)
    {
      int p = c[2] == 0x05 ? 1 : c[2] == 0x04 ? 0 : c[2] == 0x00 ? 2 : porta;
      char l[48]; snprintf(l, sizeof(l), "evento porta/pannello: %02X", c[2]); logLine(l);
      if (p != porta) { porta = p; stCambiato = true; }
      return;
    }
    if (tipo == 0x63) return;                    // domanda «com'è la rete?» ogni 15 s: rumore
    if ((tipo == 0x03 || tipo == 0x04) && clen >= 5 && c[0] == 0x02) contatori(c, clen);
    if ((tipo == 0x03 || tipo == 0x04) && clen >= 3 && c[0] == 0x0A) { parteRapporto(c, clen); return; }
    altroMessaggio(0, f, n);
  }
  else
  {
    if (tipo == 0x02 && clen >= 2)
    {
      char cosa[48];
      const char *n;
      if (c[0] == 0x02) snprintf(cosa, sizeof(cosa), "%s", c[1] == 0x01 ? "AVVIA" : c[1] == 0x05 ? "ANNULLA" : c[1] == 0x04 ? "prepara" : c[1] == 0x02 ? "PAUSA" : c[1] == 0x03 ? "RIPRENDI" : "lavoro ?");
      else if (c[0] == 0x03) { n = nomeProgramma(c[1]); snprintf(cosa, sizeof(cosa), "programma %s", n ? n : "?"); }
      else if (c[0] == 0x05) snprintf(cosa, sizeof(cosa), "cestelli %s", c[1] == 0x00 ? "entrambi" : c[1] == 0x02 ? "solo inferiore" : c[1] == 0x01 ? "solo superiore?" : "?");
      else snprintf(cosa, sizeof(cosa), "?");
      char hex[60]; uint8_t m = clen > 18 ? 18 : clen; hexString(c, m, hex);
      char l[140]; snprintf(l, sizeof(l), "comando dal modulo (%s): %s", cosa, hex); logLine(l);
      return;
    }
    if (tipo == 0x03 || tipo == 0x63 || tipo == 0x61 || tipo == 0xA0) return;   // domande e messaggi di rete del modulo
    altroMessaggio(1, f, n);
  }
}

void decodifica(uint8_t dir, const uint8_t *b, uint16_t n)
{
  for (uint16_t i = 0; i + 11 <= n; )
  {
    if (b[i] != 0xAA) { i++; continue; }
    uint16_t len = b[i + 1] + 1;
    if (len < 11 || i + len > n) { i++; continue; }
    uint8_t s = 0; for (uint16_t k = i + 1; k < i + len - 1; k++) s += b[k];
    if ((uint8_t)(~s + 1) != b[i + len - 1]) { i++; continue; }
    if (txAttesa)
    {
      if (dir == 1 && len == sizeof(DOMANDA_STATO) && !memcmp(b + i, DOMANDA_STATO, len)) txEco = true;
      else if (dir == 1 && len == 17 && !memcmp(b + i, cmdFrame, 17)) txEco = true;
      else if (dir == 0 && txEco && (b[i + 9] == 0x03 || b[i + 9] == 0x02)) txRisposta = true;
    }
    trattaTrama(dir, b + i, len);
    i += len;
  }
}

void publishDiscovery()
{
  const char *dev = "\"dev\":{\"ids\":[\"dishsniffer\"],\"name\":\"Lavastoviglie (sniffer bus Wi-Fi)\",\"mf\":\"DIY\","
                    "\"mdl\":\"M5Atom Lite\",\"sw\":\"" FW_VERSION "\"},\"avty_t\":\"" avail_topic "\"";
  char msg[700];
  snprintf(msg, sizeof(msg), "{\"name\":\"Registra trama\",\"uniq_id\":\"dish_capture\",\"cmd_t\":\"" cap_cmd "\",\"stat_t\":\"" cap_state
           "\",\"ic\":\"mdi:record-rec\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/switch/dish_capture/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Baud\",\"uniq_id\":\"dish_baud\",\"cmd_t\":\"" baud_cmd "\",\"stat_t\":\"" baud_state
           "\",\"min\":1200,\"max\":115200,\"step\":100,\"mode\":\"box\",\"ret\":true,\"ic\":\"mdi:speedometer\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/number/dish_baud/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Segnale invertito\",\"uniq_id\":\"dish_invert\",\"cmd_t\":\"" inv_cmd "\",\"stat_t\":\"" inv_state
           "\",\"pl_on\":\"1\",\"pl_off\":\"0\",\"ic\":\"mdi:invert-colors\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/switch/dish_invert/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Ultima trama\",\"uniq_id\":\"dish_last\",\"stat_t\":\"" last_topic
           "\",\"val_tpl\":\"{{ value[:255] }}\",\"ic\":\"mdi:code-brackets\",\"ent_cat\":\"diagnostic\",%s}", dev);
  client.publish("homeassistant/sensor/dish_last/config", msg, true);

  struct S { const char *id, *nome, *campo, *extra; };
  const S s[] = {
    {"frames",    "Trame ricevute",       "frames",       ",\"stat_cla\":\"total_increasing\""},
    {"frames_sm", "Trame scheda→modulo",  "frames_sm",    ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:arrow-right-bold\""},
    {"frames_ms", "Trame modulo→scheda",  "frames_ms",    ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:arrow-left-bold\""},
    {"giallo_cambi", "Cambi filo marrone", "giallo_cambi", ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:sine-wave\""},
    {"rssi",      "Segnale Wi-Fi",        "rssi",         ",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\""},
    {"ip",        "Indirizzo IP",         "ip",           ",\"ic\":\"mdi:ip-network\""},
    {"fronti_sm", "Commutazioni G22",     "fronti_sm",    ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:pulse\""},
    {"fronti_ms", "Commutazioni G19",     "fronti_ms",    ",\"stat_cla\":\"total_increasing\",\"ic\":\"mdi:pulse\""},
    {"alto_sm",   "G22 alto (%)",         "alto_sm_pct",  ",\"unit_of_meas\":\"%\",\"ic\":\"mdi:gauge\""},
    {"alto_ms",   "G19 alto (%)",         "alto_ms_pct",  ",\"unit_of_meas\":\"%\",\"ic\":\"mdi:gauge\""},
  };
  char topic[96];
  for (const S &x : s)
  {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/dish_%s/config", x.id);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"dish_%s\",\"stat_t\":\"" stats_topic
             "\",\"val_tpl\":\"{{ value_json.%s }}\"%s,\"ent_cat\":\"diagnostic\",%s}", x.nome, x.id, x.campo, x.extra, dev);
    client.publish(topic, msg, true);
  }
  snprintf(msg, sizeof(msg), "{\"name\":\"Filo marrone (G23)\",\"uniq_id\":\"dish_giallo\",\"stat_t\":\"" giallo_topic
           "\",\"ic\":\"mdi:cable-data\",\"ent_cat\":\"diagnostic\",%s}", dev);
  client.publish("homeassistant/binary_sensor/dish_giallo/config", msg, true);

  // stato decodificato (dish/stato)
  struct E { const char *comp, *id, *nome, *tpl, *extra; };
  const E e[] = {
    {"sensor", "bus_stato", "Stato (bus)", "{{ value_json.stato_nome }}", ",\"ic\":\"mdi:dishwasher\""},
    {"sensor", "bus_programma", "Programma (bus)", "{{ value_json.programma_nome }}", ",\"ic\":\"mdi:format-list-bulleted\""},
    {"sensor", "bus_minuti", "Minuti alla fine (bus)", "{{ value_json.minuti }}", ",\"unit_of_meas\":\"min\",\"ic\":\"mdi:timer-sand\""},
    {"sensor", "bus_fase", "Fase (bus)", "{{ value_json.fase_nome }}", ",\"ic\":\"mdi:water-sync\""},
    {"binary_sensor", "bus_armata", "Armata da remoto (bus)", "{{ 'ON' if value_json.armata == 1 else 'OFF' }}", ",\"ic\":\"mdi:remote\""},
    {"binary_sensor", "bus_porta", "Porta (bus)", "{{ 'ON' if value_json.porta == 'aperta' else 'OFF' }}", ",\"dev_cla\":\"door\""},
    {"sensor", "bus_opzione", "Opzione (bus)", "{{ value_json.opzione }}", ",\"ic\":\"mdi:tune\",\"ent_cat\":\"diagnostic\""},
    {"sensor", "bus_cestelli", "Cestelli (bus)", "{{ value_json.cestelli }}", ",\"ic\":\"mdi:tray-full\""},
    {"sensor", "bus_ritardo", "Ritardo partenza (bus)", "{{ value_json.ritardo }}", ",\"unit_of_meas\":\"min\",\"ic\":\"mdi:timer-play-outline\""},
    {"sensor", "bus_avanzamento", "Avanzamento (bus)", "{{ value_json.avanzamento }}", ",\"unit_of_meas\":\"%\",\"ic\":\"mdi:progress-clock\""},
    {"sensor", "bus_trascorsi", "Minuti trascorsi (bus)", "{{ value_json.trascorsi }}", ",\"unit_of_meas\":\"min\",\"ic\":\"mdi:timer-outline\""},
    {"binary_sensor", "bus_booster", "Speed Booster (bus)", "{{ 'ON' if value_json.booster == 1 else 'OFF' }}", ",\"ic\":\"mdi:rocket-launch-outline\""},
    {"sensor", "bus_raw", "Stato grezzo (bus)", "{{ value_json.raw }}", ",\"ic\":\"mdi:code-brackets\",\"ent_cat\":\"diagnostic\""},
  };
  for (const E &x : e)
  {
    snprintf(topic, sizeof(topic), "homeassistant/%s/dish_%s/config", x.comp, x.id);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"dish_%s\",\"stat_t\":\"" stato_topic
             "\",\"val_tpl\":\"%s\"%s,%s}", x.nome, x.id, x.tpl, x.extra, dev);
    client.publish(topic, msg, true);
  }
  // 2.0: energia del lavaggio in corso (total_increasing: a ogni lavaggio riparte da 0 e HA lo conta come nuovo giro)
  snprintf(msg, sizeof(msg), "{\"name\":\"Energia lavaggio in corso\",\"uniq_id\":\"dish_energia\",\"stat_t\":\"" energia_topic
           "\",\"val_tpl\":\"{{ value_json.kwh }}\",\"unit_of_meas\":\"kWh\",\"dev_cla\":\"energy\",\"stat_cla\":\"total_increasing\",%s}", dev);
  client.publish("homeassistant/sensor/dish_energia/config", msg, true);
  struct C { const char *id, *nome, *tpl, *extra; };
  const C cc[] = {
    {"ciclo_energia", "Ultimo lavaggio: energia", "{{ value_json.energia_kwh }}", ",\"unit_of_meas\":\"kWh\",\"dev_cla\":\"energy\""},
    {"ciclo_t87", "Ultimo lavaggio: temperatura lavaggio", "{{ value_json.t87 }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\""},
    {"ciclo_t88", "Ultimo lavaggio: temperatura risciacquo", "{{ value_json.t88 }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\""},
    {"ciclo_t86", "Ultimo lavaggio: temperatura iniziale", "{{ value_json.t86 }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\""},
    {"ciclo_campi", "Ultimo lavaggio: dati grezzi", "{{ value_json.campi[:250] }}", ",\"ic\":\"mdi:code-brackets\",\"ent_cat\":\"diagnostic\""},
  };
  for (const C &x : cc)
  {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/dish_%s/config", x.id);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"dish_%s\",\"stat_t\":\"" ciclo_topic
             "\",\"val_tpl\":\"%s\"%s,%s}", x.nome, x.id, x.tpl, x.extra, dev);
    client.publish(topic, msg, true);
  }
  snprintf(msg, sizeof(msg), "{\"name\":\"Registro bus\",\"uniq_id\":\"dish_log\",\"stat_t\":\"" log_topic
           "\",\"val_tpl\":\"{{ value[:250] }}\",\"ic\":\"mdi:text-box-search-outline\",\"ent_cat\":\"diagnostic\",%s}", dev);
  client.publish("homeassistant/sensor/dish_log/config", msg, true);

  // 2.3 (18/11/2026): comandi locali dall'Atom (in parallelo sul nero, open-drain). La scheda li accetta solo da armata.
  const char *bt[][3] = {{"avvia", "Avvia", "mdi:play"}, {"annulla", "Annulla", "mdi:stop"},
                         {"pausa", "Pausa", "mdi:pause"}, {"riprendi", "Riprendi", "mdi:play-pause"}};
  for (auto &x : bt)
  {
    snprintf(topic, sizeof(topic), "homeassistant/button/dish_%s/config", x[0]);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"dish_btn_%s\",\"cmd_t\":\"" tx_cmd "\",\"pl_prs\":\"%s\",\"ic\":\"%s\",%s}",
             x[1], x[0], x[0], x[2], dev);
    client.publish(topic, msg, true);
  }
  static char big[1500];
  snprintf(big, sizeof(big), R"({"name":"Programma","uniq_id":"dish_sel_programma","cmd_t":")" tx_cmd R"(","stat_t":")" stato_topic
           R"(","val_tpl":"{{ value_json.programma_nome }}","options":["Auto 45-65°","Intensivo 65°","Eco 50°","Delicato 40°","Prelavaggio","Express 60°","Extra silenzioso","Pulizia vasca"],)"
           R"("cmd_tpl":"programma {{ {'Auto 45-65°':'01','Intensivo 65°':'02','Eco 50°':'04','Delicato 40°':'05','Prelavaggio':'08','Express 60°':'09','Extra silenzioso':'0D','Pulizia vasca':'10'}[value] }}",)"
           R"("ic":"mdi:format-list-bulleted",%s})", dev);
  client.publish("homeassistant/select/dish_programma/config", big, true);
  snprintf(big, sizeof(big), R"({"name":"Cestelli","uniq_id":"dish_sel_cestelli","cmd_t":")" tx_cmd R"(","stat_t":")" stato_topic
           R"(","val_tpl":"{{ value_json.cestelli }}","options":["Entrambi","Solo inferiore"],)"
           R"("cmd_tpl":"cestelli {{ '02' if value == 'Solo inferiore' else '00' }}","ic":"mdi:tray-full",%s})", dev);
  client.publish("homeassistant/select/dish_cestelli/config", big, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Ultimo comando\",\"uniq_id\":\"dish_esito\",\"stat_t\":\"" tx_esito
           "\",\"val_tpl\":\"{{ (value_json.prova ~ ': ' ~ value_json.esito)[:250] }}\",\"json_attr_t\":\"" tx_esito "\",\"ic\":\"mdi:console-line\",%s}", dev);
  client.publish("homeassistant/sensor/dish_esito/config", msg, true);
}

void publishCfgState()
{
  client.publish(cap_state, capturing ? "ON" : "OFF", true);
  client.publish(baud_state, String(baudRate).c_str(), true);
  client.publish(inv_state, invert ? "1" : "0", true);
}

// ---------------------------------------------------------------- 2.1 (18/11/2026): prove di trasmissione sul NERO (G19)
// L'Atom resta in parallelo, senza resistenze. Trasmette SOLO in open-drain (tira giù, non spinge mai in alto) e con la
// corrente d'uscita minima. «sonda»: impulso basso di 20 µs (più corto di mezzo bit a 9600, la scheda non lo legge come
// dato) e rilettura del pin: se scende, la linea è tirata su da una resistenza (open-drain) e si può trasmettere; se resta
// alta, il modulo la tiene su attivamente e non si trasmette. «stato»: manda la stessa domanda di stato del modulo
// (innocua) quando il bus è zitto da 1 s, e controlla l'eco su G19 e la risposta della scheda su G22.
portMUX_TYPE muxTx = portMUX_INITIALIZER_UNLOCKED;

void neroOD(bool uart, gpio_drive_cap_t cap)
{
  gpio_set_level(GPIO_NUM_19, 1);
  esp_rom_gpio_connect_out_signal(PIN_MS, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_drive_capability(GPIO_NUM_19, cap);
  if (uart) esp_rom_gpio_connect_out_signal(PIN_MS, U1TXD_OUT_IDX, invert, false);
}
void neroSoloAscolto()
{
  gpio_set_level(GPIO_NUM_19, 1);
  esp_rom_gpio_connect_out_signal(PIN_MS, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT);
}

void esito(const char *prova, const char *risultato, const char *dettaglio)
{
  char j[300];
  snprintf(j, sizeof(j), "{\"prova\":\"%s\",\"esito\":\"%s\",\"dettaglio\":\"%s\",\"sonda\":%d,\"uptime_s\":%lu}",
           prova, risultato, dettaglio, sondaEsito, millis() / 1000UL);
  if (client.connected()) client.publish(tx_esito, j, true);
  char l[200]; snprintf(l, sizeof(l), "TX %s: %s (%s)", prova, risultato, dettaglio); logLine(l);
}

void sonda()
{
  int prima = gpio_get_level(GPIO_NUM_19), basso = 0, dopo = 1, cap = 0;
  const gpio_drive_cap_t caps[2] = {GPIO_DRIVE_CAP_0, GPIO_DRIVE_CAP_1};
  for (cap = 0; cap < 2 && basso < 3; cap++)
  {
    basso = 0;
    neroOD(false, caps[cap]);
    for (int k = 0; k < 5; k++)
    {
      portENTER_CRITICAL(&muxTx);
      gpio_set_level(GPIO_NUM_19, 0);
      delayMicroseconds(10);
      int l = gpio_get_level(GPIO_NUM_19);
      delayMicroseconds(10);
      gpio_set_level(GPIO_NUM_19, 1);
      portEXIT_CRITICAL(&muxTx);
      delayMicroseconds(20);
      dopo = gpio_get_level(GPIO_NUM_19);
      if (!l) basso++;
      delay(20);
    }
    neroSoloAscolto();
  }
  sondaEsito = basso >= 3 ? 1 : 0;
  char d[120];
  snprintf(d, sizeof(d), "prima %d, scesa %d volte su 5 con corrente %s, dopo %d", prima, basso, cap == 1 ? "minima" : "bassa", dopo);
  esito("sonda", sondaEsito ? "il nero scende: open-drain, si puo trasmettere" : "il nero NON scende: lo tiene su il modulo, niente trasmissione", d);
}

void domandaStato()
{
  if (sondaEsito != 1) { esito("stato", "non inviata", "la sonda non e riuscita"); return; }
  neroOD(true, GPIO_DRIVE_CAP_0);
  Serial1.write(DOMANDA_STATO, sizeof(DOMANDA_STATO));
  Serial1.flush();
  neroSoloAscolto();
  txInviata = millis(); txAttesa = true; txEco = false; txRisposta = false;
}

// 2.2: comandi (tipo 02 SET, corpo 6 byte) decodificati il 17/11: 02 01 avvia, 02 05 annulla, 02 02/03 pausa/riprendi (da
// provare), 03 XX programma, 05 ZZ cestelli. La scheda li accetta solo da armata (Smart Control).
void inviaComando()
{
  if (sondaEsito != 1) { sonda(); if (sondaEsito != 1) { esito(cmdNome, "non inviato", "il nero non scende"); return; } }
  uint8_t f[17] = {0xAA, 0x10, 0xE1, 0, 0, 0, 0, 0, 0, 0x02, cmdCorpo[0], cmdCorpo[1], 0, 0, 0, 0, 0};
  uint8_t sum = 0; for (int k = 1; k < 16; k++) sum += f[k];
  f[16] = (uint8_t)(~sum + 1);
  memcpy(cmdFrame, f, 17);
  neroOD(true, GPIO_DRIVE_CAP_0);
  Serial1.write(f, 17);
  Serial1.flush();
  neroSoloAscolto();
  txInviata = millis(); txAttesa = true; txEco = false; txRisposta = false;
}

void gestisciTx()
{
  if (txAttesa)
  {
    const char *pr = cmdNome[0] ? cmdNome : "stato";
    if (txEco && txRisposta) { txAttesa = false; esito(pr, "riuscita: la scheda ha risposto", "eco su G19 e risposta su G22"); cmdNome[0] = 0; }
    else if (millis() - txInviata > 1500)
    {
      txAttesa = false; cmdNome[0] = 0;
      esito(pr, "nessuna risposta", txEco ? "eco ok, la scheda non ha risposto" : "niente eco: i byte non sono passati sulla linea");
    }
    return;
  }
  if (!txRichiesta) return;
  unsigned long q = millis() - max(versi[0].lastByte, versi[1].lastByte);
  if (versi[0].idx == 0 && versi[1].idx == 0 && q >= 1000)
  {
    char r = txRichiesta; txRichiesta = 0;
    if (r == 's') sonda(); else if (r == 'c') inviaComando(); else { if (sondaEsito != 1) sonda(); if (sondaEsito == 1) domandaStato(); else esito("stato", "non inviata", "il nero non scende"); }
  }
  else if (millis() - txChiesta > 60000UL) { char r = txRichiesta; txRichiesta = 0; esito(r == 's' ? "sonda" : "stato", "annullata", "il bus non e mai stato zitto per 1 s"); }
}

void callback(char *topic, byte *payload, unsigned int len)
{
  String t = topic, p;
  for (unsigned int i = 0; i < len; i++) p += (char)payload[i];
  p.trim();
  if (t == cap_cmd) { capturing = (p == "ON" || p == "1"); client.publish(cap_state, capturing ? "ON" : "OFF", true); }
  else if (t == baud_cmd) { uint32_t b = p.toInt(); if (b >= 1200 && b <= 115200) { baudRate = b; startUART(); client.publish(baud_state, String(baudRate).c_str(), true); } }
  else if (t == tx_cmd)
  {
    auto cmd = [&](const char *n, uint8_t a, uint8_t b) { cmdCorpo[0] = a; cmdCorpo[1] = b; snprintf(cmdNome, sizeof(cmdNome), "%s", n); txRichiesta = 'c'; };
    if (p == "sonda") txRichiesta = 's'; else if (p == "stato") { cmdNome[0] = 0; txRichiesta = 'q'; }
    else if (p == "avvia") cmd("avvia", 0x02, 0x01); else if (p == "annulla") cmd("annulla", 0x02, 0x05);
    else if (p == "pausa") cmd("pausa", 0x02, 0x02); else if (p == "riprendi") cmd("riprendi", 0x02, 0x03);
    else if (p.startsWith("programma ")) cmd("programma", 0x03, (uint8_t)strtol(p.c_str() + 10, NULL, 16));
    else if (p.startsWith("cestelli ")) cmd("cestelli", 0x05, (uint8_t)strtol(p.c_str() + 9, NULL, 16));
    else if (p.startsWith("grezzo ") && p.length() >= 12)          // 2.3: «grezzo XX YY» per le prove (corpo XX YY 00 00 00 00)
      cmd("grezzo", (uint8_t)strtol(p.substring(7, 9).c_str(), NULL, 16), (uint8_t)strtol(p.substring(10, 12).c_str(), NULL, 16));
    txChiesta = millis();
  }
  else if (t == inv_cmd) { invert = (p == "1" || p == "ON"); startUART(); client.publish(inv_state, invert ? "1" : "0", true); }
}

bool reconnect()
{
  if (!client.connect(NAME, AIO_USERNAME, AIO_KEY, avail_topic, 0, true, "offline")) return false;
  client.subscribe(cap_cmd);
  client.subscribe(baud_cmd);
  client.subscribe(inv_cmd);
  client.subscribe(tx_cmd);
  client.publish(avail_topic, "online", true);
  publishDiscovery();
  publishCfgState();
  giallo = -1;                  // ripubblica il livello del giallo
  return true;
}

void flushFrame(Verso &v)
{
  if (!v.idx) return;
  v.frames++; bytesTot += v.idx;
  decodifica(&v == &versi[0] ? 0 : 1, v.buf, v.idx);
  if (capturing && client.connected())
  {
    static char hex[BUF_MAX * 3 + 1];
    hexString(v.buf, v.idx, hex);
    static char line[BUF_MAX * 3 + 48];
    snprintf(line, sizeof(line), "%lu %s %lu %s", millis(), v.tag, millis() - v.lastByte, hex);
    client.publish(cap_topic, line);
    snprintf(line, sizeof(line), "%s %s", v.tag, hex);
    client.publish(last_topic, line);
  }
  v.idx = 0;
  M5.dis.drawpix(0, v.tag[2] == '2' ? 0x0000ff : 0x00ffff); // blu scheda→modulo, azzurro modulo→scheda
}

void leggiGiallo()
{
  int l = digitalRead(PIN_GIALLO);
  if (l != gialloLetto) { gialloLetto = l; gialloCambi++; }
  // al massimo un aggiornamento al secondo: se il giallo fosse un'altra linea dati, i cambi si vedono nel contatore
  if (client.connected() && l != giallo && millis() - gialloUltimaPub >= 1000UL)
  {
    giallo = l; gialloUltimaPub = millis();
    client.publish(giallo_topic, l ? "ON" : "OFF", true);
    if (capturing) { char line[40]; snprintf(line, sizeof(line), "%lu GIALLO %d", millis(), l); client.publish(cap_topic, line); }
  }
}

void publishStats()
{
  uint32_t a0 = campioni ? (alti[0] * 100UL / campioni) : 0, a1 = campioni ? (alti[1] * 100UL / campioni) : 0;
  campioni = 0; alti[0] = alti[1] = 0;
  char j[420];
  snprintf(j, sizeof(j), "{\"fronti_sm\":%lu,\"fronti_ms\":%lu,\"alto_sm_pct\":%lu,\"alto_ms_pct\":%lu,"
           "\"frames\":%lu,\"frames_sm\":%lu,\"frames_ms\":%lu,\"bytes\":%lu,\"giallo\":%d,\"giallo_cambi\":%lu,"
           "\"baud\":%lu,\"invert\":%d,\"capturing\":%d,\"rssi\":%d,\"ip\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,\"fw\":\"%s\"}",
           (unsigned long)fronti[0], (unsigned long)fronti[1], a0, a1,
           versi[0].frames + versi[1].frames, versi[0].frames, versi[1].frames, bytesTot, gialloLetto, gialloCambi,
           baudRate, invert, capturing, WiFi.RSSI(), WiFi.localIP().toString().c_str(), millis() / 1000UL, ESP.getFreeHeap(), FW_VERSION);
  client.publish(stats_topic, j);
}

void setup()
{
  M5.begin(true, false, true);
  M5.dis.drawpix(0, 0xff8000);
  // 1.4: G23 è l'ingresso della seconda seriale (giallo), niente pull-down
  WiFi.mode(WIFI_STA);
  WiFi.begin(NET_SSID, NET_PSW);
  while (WiFi.status() != WL_CONNECTED) delay(400);
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.begin();
  client.setServer(AIO_SERVER, AIO_SERVERPORT);
  client.setCallback(callback);
  client.setBufferSize(2048);
  startUART();
  attachInterrupt(digitalPinToInterrupt(PIN_SM), isrSM, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_MS), isrMS, CHANGE);
}

void loop()
{
  ArduinoOTA.handle();
  if (!client.connected()) { M5.dis.drawpix(0, 0xff00ff); reconnect(); delay(500); }
  client.loop();

  for (Verso &v : versi)
  {
    while (v.ser->available())
    {
      uint8_t c = v.ser->read();
      if (v.idx < BUF_MAX) v.buf[v.idx++] = c;
      v.lastByte = millis();
      if (v.idx >= BUF_MAX) flushFrame(v);   // trama più lunga del buffer: la chiudiamo qui
    }
    if (v.idx && millis() - v.lastByte >= GAP_MS) flushFrame(v);
  }
  gestisciTx();
  if (haveSt && (stCambiato || millis() - ultimoStato >= 60000UL)) pubblicaStato();
  leggiGiallo();
  campioni++; alti[0] += digitalRead(PIN_SM); alti[1] += digitalRead(PIN_MS);

  if (millis() - lastStats >= 10000UL) { lastStats = millis(); if (client.connected()) { publishStats(); M5.dis.drawpix(0, 0x00ff00); } }
  M5.update();
}
