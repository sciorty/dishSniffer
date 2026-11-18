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
| 21 | bit 0 = Speed Booster |
| 23 | changed by command `0A XX` ? |

### Door / panel event
Type 04, body `03 00 XX 00 00`: `05` door open, `04` closed, `00` washing (locked), `01` seen at power-off and when
arming Smart Control. At the end of a full cycle the door opens by itself.

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

Example, start: `AA 10 E1 00 00 00 00 00 00 02 02 01 00 00 00 00 0A`.

Speed Booster and sanitize set from SmartThings produce no traffic before the start: no command found yet.

## Counters (types 03/04, body `02` + 4-byte entries `tag(2) value(2)`)

| Tag | Meaning |
|---|---|
| `1A0B` | energy of the current wash, in **10 Wh** units (e.g. 19 → 81 → 120 → 150 = 1.50 kWh at the end of an Intensive 65°). Resets every wash |
| `1A07`–`1A0A` | always 0 so far |

## End-of-cycle report

Four type-03 messages with body `0A seq 46 …` (seq 1–4). Drop the first 3 bytes of each part and concatenate: `02`,
number of items (`0x13` = 19), then items `tag, length, value`.

| Tag | Example | Meaning |
|---|---|---|
| 93 | 1500 | energy, Wh |
| 87 | 650 | 65.0 °C, wash temperature |
| 88 | 580 | 58.0 °C, rinse temperature ? |
| 86 | 230 | 23.0 °C, inlet water ? |
| 70–73, 80–85, 89–92, 94 | | unknown (compare different programs) |

A full annotated wash (Italian) is in [analisi-20261118.md](analisi-20261118.md).

## Related work
- Midea dishwasher panel bus (a different bus, `0x55` frames): github.com/0xGiddi/esphome-midea-dishwasher
- Midea UART sniffing: github.com/reneklootwijk/midea-uartsniffer
