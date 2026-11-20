# Protocol: Samsung DW60A8050 main board ↔ Wi-Fi module

Decoded on a Samsung **DW60A8050BB** (Wi-Fi module **CWAP210M**, board made by Midea) by listening to the CON1 bus
and correlating frames with button presses, SmartThings commands and full wash cycles (November 2026).
Anything marked **?** is a guess.

## Physical layer

| CON1 wire | Function |
|---|---|
| red | GND |
| white | +5 V (the ESP32 can be powered from it) |
| yellow | TX board → module |
| black | TX module → board. **Open-drain with a pull-up to 5 V**: a second device can pull it low in parallel |
| brown | static high, no data |

UART **9600 8N1**, 5 V levels, idle high.

## Frame

```
AA  len  E1  sync  00 00  msgid  00 00  type  body…  checksum
```

- `len` = total length − 1.
- `checksum = (~sum(bytes[1 .. n-2]) + 1) & 0xFF` (two's complement of the sum, without `AA`).
- `type`: `02` set (command), `03` query / reply, `04` spontaneous notification, `61` time, `63` network status.

### Background traffic
- Every 15 s the board asks for the network status: `AA 0B E1 EA 00 00 01 00 00 63 00 C6`. The module replies ~90 ms
  later (`63 01 …` in station mode, `63 03 44 01 04 A8 C0 …` = 192.168.4.1 in AP / pairing mode).
- The module sends the time: `61 ss mm hh dow DD MM YY`.

## Status (body of 24 bytes, `body[0] = 01`, types 02/03/04 from the board)

Query (sent by the module, harmless, the board answers within ~60 ms):
`AA 12 E1 00 00 00 00 00 00 03 01 00 00 00 00 00 00 00 09`

| Byte | Meaning |
|---|---|
| 1 | state: `01` idle, `02` running (also during a delayed start, with phase `06`), `03` paused |
| 2 | progress % |
| 3–4 | minutes elapsed (big endian) |
| 5–6 | minutes to the end (big endian) |
| 7, 8 | ? (change at the end of the cycle) |
| 9 | **armed for remote control** (Smart Control): `01` / `00`. It disarms when the door opens or the panel is touched |
| 10–11 | delayed-start minutes |
| 16 | phase: `00` none, `01` pre-wash, `02` wash, `03` rinse, `04` drying, `05` end, `06` waiting (delayed start), `07` draining / cancel |
| 17 | `04` / `00` display / standby ? |
| 19 | program: `01` Auto 45-65°, `02` Intensive 65°, `04` Eco 50°, `05` Delicate 40°, `08` Pre-wash, `09` Express 60°, `0D` Extra silent, `10` Self clean |
| 20 | baskets: `01` both, `04` lower only |
| 21 | bit 0 = Speed Booster, bit 1 = **Sanitize** (seen while toggling them on the panel: `01` → `03` → `02`) |
| 22 | bit 1 = **auto door release disabled** (`04` = auto-open in drying ON, `06` = OFF; set from SmartThings, see commands) |
| 23 | changed by command `0A XX` ? |

### Door / panel event
Type 04, body `03 00 XX 00 00`: `05` door open, `04` closed, `00` washing (locked) / panel off, `01` panel on (also when
arming Smart Control). **Auto door release**: with the option on, the door opens by itself in the drying phase about
10 minutes before the end, and the board sends only `01`, never `05` (seen 18/11 02:02 and 19/11 16:51, both while
running in phase `04`). Firmware 2.5 reports that case as `aperta (automatica)`.

## Commands (module → board, type 02, body of 6 bytes)

The board accepts them **only when armed** (byte 9 = `01`).

| Body | Action | Verified |
|---|---|---|
| `02 01 00 00 00 00` | start | yes |
| `02 02 …` | pause (state → `03`) | yes |
| `02 03 …` | resume | yes |
| `02 05 …` | cancel (drains ~1 min, then idle; stays armed) | yes |
| `02 04 …` | "prepare" (sent by the module before changing baskets) | seen |
| `03 XX …` | set program `XX` (other codes are stored but are not real programs: the duration does not change) | yes |
| `05 00` / `05 02` | baskets both / lower only (`05 01`, upper only, is ignored) | yes |
| `04 XX …` | ⚠️ **starts a wash** (`04 01` started an Auto cycle). Do not use | yes |
| `0A XX …` | changes status byte 23 | ? |
| `01 02 00 00 00 00` / `01 02 01 00 00 00` | auto door release in drying **on / off** (status byte 22 bit 1), sent by the module when the option is toggled in SmartThings | yes (19/11, from the Atom; accepted **also when not armed**) |

Example, start: `AA 10 E1 00 00 00 00 00 00 02 02 01 00 00 00 00 0A`.

Speed Booster and sanitize set from SmartThings produce no traffic before the start: no command found yet.

## Counters (types 03/04, body `02` + 4-byte entries `tag(2) value(2)`)

| Tag | Meaning |
|---|---|
| `1A0B` | energy, in **10 Wh** units. **Cumulative, it does not reset at every wash** (it was 0 right after a reset of the Wi-Fi module): 0 → 150 over an Intensive 65°, then 194 → 349 over an Auto + Sanitize = 1.55 kWh, exactly the `93` of that wash's report. The energy of one wash is the increase while it runs |
| `1A07`–`1A0A` | always 0 so far |

## End-of-cycle report

Four type-03 messages with body `0A seq 46 …` (seq 1–4). Drop the first 3 bytes of each part and concatenate: `02`,
number of items (`0x13` = 19), then items `tag, length, value`.

| Tag | Example | Meaning |
|---|---|---|
| 93 | 1500 | energy, Wh |
| 87 | 650 | 65.0 °C, wash temperature |
| 88 | 580 / 700 | final rinse temperature: 58.0 °C in an Intensive 65°, **70.0 °C with Sanitize** |
| 86 | 230 | 23.0 °C, inlet water ? |
| 80 | 242 → 243 → 244 | **completed washes, total** (a wash cancelled early is not counted) |
| 83 | 1, 1, then 0 | **rinse aid low** (probably): 1 in the two washes before refilling it, 0 in the first wash after. The app's "rinse aid low" warning disappeared at the same time |
| 71, 72 | 167 / 18 (Intensive), 130 / 22 (Auto + Sanitize), 138 / 18 (Eco) | ? water (dl)? |
| 81, 82 | always 1 | ? (salt?) |
| 70, 73, 84, 85, 89–92, 94 | | same in every wash so far |

Careful when concatenating: the last byte of each part is its checksum, not data (it looks like a value for `84`).

Eco 50° (20/11): 0.746 kWh, wash 50.0 °C, rinse 45.0 °C, inlet 24.0 °C; with auto door release the door opened
**50 minutes** before the end (10 minutes in Auto): the drying phase is longer.

A full annotated wash (Italian) is in [analisi-20261118.md](analisi-20261118.md).

## Related work
- Midea dishwasher panel bus (a different bus, `0x55` frames): github.com/0xGiddi/esphome-midea-dishwasher
- Midea UART sniffing: github.com/reneklootwijk/midea-uartsniffer
