# Install guide for an AI assistant (dishSniffer)

**Human**: give this file to Claude (or another AI assistant) together with the link to this repository and say
"help me install this". Then answer its questions. You do the physical work; it tells you what to do and checks
each step with you.

---

**Assistant**: you are helping a person install dishSniffer, an ESP32 (M5Stack Atom Lite) that reads and controls a
Samsung dishwasher over the bus between its main board and its Wi-Fi module, integrated in Home Assistant via MQTT.
Read `README.md` and `docs/PROTOCOL.md` of the repository first. Go **one step at a time**: give one instruction, wait
for the result, then continue. Never ask the person to do two risky things at once. Answer in their language.

## Rules

- **Safety first.** The dishwasher is a mains appliance: it must be **unplugged** whenever the panel is open and wires
  are being connected. Say it every time it applies.
- **Never suggest a resistor to ground** on the data lines (no voltage dividers): on this bus it broke the
  communication. A series resistor (1–10 kΩ) on each data line is acceptable and more cautious than a direct wire.
- **Listen-only first.** Do not let the person send commands (`dish/tx/cmd`) until reading works and `sonda` has
  confirmed that the black line is open-drain on their unit.
- **Never send `04 XX` or other unknown command codes**: `04 01` starts a wash. Only the commands listed in the README.
- Never ask the person to paste passwords into the chat; they go in `secrets.h`, which is not committed.
- If something does not match what the README says (different module, different wire colours, no `AA … E1` frames),
  stop and say so: their model may be different. Do not improvise wiring.

## Step 1: check the model

Ask for:
1. The dishwasher model (label on the side of the door) and whether it has SmartThings Wi-Fi.
2. A photo of the Wi-Fi module once the panel is open: it should be a **CWAP210M** with a 5-wire cable in **CON1**
   (white, red, yellow, black, brown).

Tested only on the DW60A8050BB. Other Samsung dishwashers with the CWAP210M may work; tell the person it is an
experiment and that listen-only is harmless if done as described.

## Step 2: what they need

- M5Stack Atom Lite (or another ESP32: then adapt the pin numbers), 5 jumper wires, a USB-C cable.
- Home Assistant with the **Mosquitto broker** add-on and the **MQTT** integration. Help them create a Home Assistant
  user for the Atom (Settings → People → Users).
- A computer with **arduino-cli** (or the Arduino IDE), ESP32 core **2.0.17**, libraries **M5Atom**, **FastLED 3.6.0**,
  **PubSubClient**. Give the exact commands for their operating system.

## Step 3: firmware (on the desk, nothing connected to the dishwasher)

1. `cp dishSniffer/secrets.example.h dishSniffer/secrets.h` and fill in Wi-Fi, MQTT user/password, broker IP
   (the Home Assistant IP). They edit the file themselves.
2. Compile and upload via USB (commands in the README). Ask them to read the serial output or to check that the
   device **"Lavastoviglie (sniffer bus Wi-Fi)"** appears in Home Assistant (Settings → Devices → MQTT).
3. Do not continue until the device is online in Home Assistant.

## Step 4: wiring, listen-only

With the dishwasher **unplugged**, panel open:

1. Identify the wires with a multimeter **before** connecting the Atom (dishwasher plugged in for the measurement
   only, panel kept away from water, probes only): red = GND, white = +5 V, yellow and black around 5 V idle. If the
   colours or voltages differ, stop.
2. Unplug again. Connect **GND first**, then yellow → **G22**, black → **G19**, optionally brown → G23. Tap the wires:
   the original module stays plugged in.
3. For the first test, keep the Atom powered from USB and **do not connect white (+5 V)**. Later, white → 5V lets the
   dishwasher power the Atom (then never connect USB and white at the same time).
4. Plug the dishwasher in. In Home Assistant, check:
   - `sensor.lavastoviglie_sniffer_bus_wi_fi_trame_ricevute` keeps increasing (a frame every 15 s at least);
   - `sensor.…_stato_bus` shows *Ferma*, `sensor.…_programma_bus` shows the program on the panel;
   - SmartThings, if they use it, still works.
5. If nothing arrives: check GND, then swap yellow and black (harmless), then check the baud rate (9600).

## Step 5: check what they see

Ask them to change the program on the panel, open and close the door, arm Smart Control (open the door, press
Smart Control, close within a few seconds). Each action must show up in `sensor.…_registro_bus` and in the decoded
sensors. If unknown messages appear, collect them from `dish/log`: they are useful for the project.

## Step 6: transmitting (only if they want commands)

1. Send `sonda` on `dish/tx/cmd` (MQTT, not retained; e.g. Settings → MQTT → Publish a packet). Read
   `sensor.…_ultimo_comando`. Only if it says the black line goes low ("il nero scende") continue.
2. Send `stato`: the board must answer ("riuscita"). This query is harmless.
3. With the dishwasher armed and **empty or loaded as they wish**, press **Avvia** and then, if they only want to test,
   **Annulla** (it drains for about a minute and stops). Confirm each step before the next.

## Step 7: Home Assistant extras

Offer the files in `homeassistant/`: the example dashboard, the goodnight script and the scheduled-start automation
(explain the helpers they need: `input_number.lavastoviglie_ore_ritardo_avvio`,
`input_datetime.lavastoviglie_avvio_previsto`), and the pyscript files if they have pyscript installed.

## Step 8: wrap up

Make sure the panel is closed properly and nothing is pinched or near water. Remind them that OTA has no password
(keep the Atom on a trusted network) and how to update the firmware over the air.
