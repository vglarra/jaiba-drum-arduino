# Jaiba Control Hub + ESP32 7" UI — Implementation Plan

**Status:** draft for approval · 2026-11-02
**Scope:** replace the Teensy TFT UI with the Elecrow CrowPanel ESP32-S3 7"
LVGL UI; add a Teensy **control hub** (sensor UART bridge + knobs/buttons +
USB-MIDI) that talks to the ESP32 over I2C; keep the sensor USB-MIDI and the
hub USB-MIDI direct to the sampler.

---

## 1. Goals

1. One 7" touch UI that can:
   - monitor the **left and right** sensor Teensys (hits, calibration,
     tuning, pad assignments), and
   - configure the **knob/button Teensy** mappings (a fixed list of sampler
     controls, chosen from a drop-down).
2. No added latency on the sampler path. The sensor boards' USB-MIDI and the
   knob hub's USB-MIDI are separate USB devices; the ESP32 is never in that
   path.
3. The setup survives power cycles: hub config in EEPROM, sampler map in its
   settings, ESP32 UI prefs in NVS.
4. Minimal new hardware. The ESP32 display connects to the drum PCB with
   **I2C + power/GND only** — no second UART, no SC16IS752, no SD-pin UART
   hack.

## 2. Fixed decisions

- The **Teensy hub** bridges the two sensor **UARTs** to the ESP32 over I2C.
- The bridge is **transparent**: raw sensor lines are forwarded as
  `[board][len][bytes]` frames; the ESP32 keeps the sensor protocol parser.
- The sampler mapping is **fixed**, selected from a drop-down in the ESP32
  UI: a knob or button becomes one of the sampler's programmed controls.
- The hub is also the **knob/button controller** and a third USB-MIDI device
  (`Jaiba Knobs`).
- The sensor boards keep their existing firmware and USB-MIDI.

---

## 3. Hardware topology

### 3.1 Final (three Teensys)

```
U_R (sensor) ── Serial1 ─┐
                         ├─ Teensy Hub (U_UI) ── I2C 0x42 ── ESP32 7" UI
U_L (sensor) ── Serial1 ─┘        │
                                  ├─ knobs / buttons
                                  ├─ USB-MIDI "Jaiba Knobs" ── sampler
                                  └─ EEPROM

U_R USB-MIDI ─┐
U_L USB-MIDI ─┴──────────────────── sampler (direct, unchanged)
```

| Device | Connects to | Notes |
|---|---|---|
| `U_R` sensor | `U_UI` pins 0/1 (Serial1) + USB | 9 pads, notes 60–76 |
| `U_L` sensor | `U_UI` pins 15/14 (Serial3) + USB | 7 pads, notes 78–90 |
| Teensy Hub | `U_UI` socket, USB, I2C 18/19 | bridge + knobs/buttons + USB-MIDI |
| ESP32 display | hub I2C (display J6/J4 19/20) + 5 V/2 A + GND | LVGL, touch, no sensor UARTs |
| Sampler | three USB-MIDI ports | `Jaiba Right`, `Jaiba Left`, `Jaiba Knobs` |

### 3.2 Current bench (two Teensys)

Move the **left Teensy to the U_UI socket** and flash the hub firmware; leave
the left sensor socket `U_L` empty. The U_UI socket is the only place where
both sensor UARTs meet, so it is the only place a single Teensy can bridge
both.

```
U_R (right sensor) ── Serial1 ── U_UI 0/1 ── Hub (left Teensy in U_UI)
U_L socket: empty; left sensor UART is idle on U_UI 15/14
Hub ── USB-MIDI "Jaiba Knobs" ── sampler     (second port)
U_R ── USB-MIDI ──────────────── sampler     (first port)
ESP32 ── I2C ── Hub
```

- The right sensor's UART is read by the hub and forwarded to the ESP32.
- The left sensor is absent. The ESP32's left monitor is offline by default;
  the hub's **test mode** can inject fake left lines (`HIT`, `PADVAL`,
  `CAL_STATE`) so the left UI can be exercised.
- A 10K pot on a free U_UI analog pin (e.g. A2/16) simulates one knob; the hub
  sends CC, the ESP32 shows the value, the sampler receives it. Knob/button
  pin assignment is deferred.
- The sampler sees `Jaiba Right` + `Jaiba Knobs`; `Jaiba Left` is absent and
  retried by the auto-detect.
- The left sensor pads are not wired; they are not involved in the hub test.

### 3.3 Restoring the third Teensy

When the third Teensy is available:

1. Flash the hub firmware to the third Teensy and put it in `U_UI`.
2. Reflash the left board with the left sensor build and return it to `U_L`.
3. The sampler now sees three ports; the ESP32 monitors both boards.

---

## 4. Project / folder layout (answers the fourth-folder question)

| Folder | Role | Status |
|---|---|---|
| `jaiba-hexa-drum-sensor-code/` | right/left sensor firmware | existing |
| `jaiba-hexa-drum-ui/` | **hub env `[env:hub]`** + legacy Teensy TFT UI env | existing; hub is new |
| `jaiba-hexa-drum-esp32-ui/` | **ESP32-S3 CrowPanel LVGL UI** | **new — fourth folder** |
| `jaiba-sampler/` | PC sampler; third MIDI input + fixed map | existing |

**Yes, the ESP32 firmware needs its own PlatformIO project — the fourth
folder.** It is a different platform/board/toolchain (ESP32-S3, pioarduino,
LVGL 9.1, LovyanGFX + vendor patch) and cannot share the Teensy UI project's
environment.

The **hub firmware can reuse `jaiba-hexa-drum-ui`** as a new `[env:hub]` with
its own `src/hub/` and `build_src_filter`; the existing `[env:teensy41]` TFT
UI stays as a legacy/reference env (or is removed later). That keeps the
layout at four folders instead of adding a fifth.

If clean separation is preferred, create a fifth folder `jaiba-knob-hub/`.
The four-folder layout is the recommendation, because the hub is the new
U_UI Teensy role and the old TFT UI is retired.

The old TFT_eSPI screens are not reused; the old `uart_protocol`/`drum_state`
logic is ported to the ESP32. `UI_PLAN.md` and `UART_PROTOCOL.md` move/are
rewritten for the ESP32 transport.

The workspace root repo (`Docs/`, Arduino sketches) should add the new ESP32
folder to `.gitignore` and the README's "related repositories" list when the
folder is created.

---

## 5. Firmware responsibilities

| Firmware | Responsibilities |
|---|---|
| **Sensor right/left** | Unchanged. Read pads, send MIDI, speak the UART protocol on Serial1. |
| **Teensy Hub** | `Serial1` right + `Serial3` left ⇄ I2C FIFOs; knobs/buttons → USB-MIDI CC/notes; I2C slave 0x42; config EEPROM; watchdog; test mode. |
| **ESP32 UI** | I2C master; GT911 touch; LVGL screens; sensor monitor via `BridgeTransport`; knob/button config (fixed table + drop-down); NVS. |
| **Sampler** | Three auto-detected MIDI inputs; fixed control map; CC/note → parameter/action; persistence; disconnect behavior. |

---

## 6. Link protocols

### 6.1 Sensor UART → hub

- Both sensor boards: `Serial1`, 115200, 8N1, existing line protocol.
- Hub: `Serial1` (U_UI 0/1) = right; `Serial3` (U_UI 15/14) = left.
- The hub forwards raw bytes in frames `[board:1][len:1][bytes]`:
  - `board` 0 = right, 1 = left.
  - `len` ≤ 63 (matches `UART_LINE_MAX`).
- The hub does **not** parse `HIT`/`PADVAL`/`CAL_STATE`/`SET_*`; it just routes
  bytes. The ESP32 keeps the existing parser/state and per-board ACK logic.
- Hub side: one response FIFO per board (received bytes) and one command FIFO
  per board (ESP32 → sensor). A `response count` / `command space` status lets
  the ESP32 poll without blocking.
- The ESP32 polls the hub's status (1–2 bytes) and only reads a FIFO when
  data is present.

### 6.2 Hub USB-MIDI → sampler

- Product name **`Jaiba Knobs`** (USB descriptor override, same technique as
  `Jaiba Right`/`Jaiba Left`).
- 6 knobs: CC (7-bit or 14-bit for pitch-like controls) on change, with
  hysteresis; buttons: notes/CC per the fixed map.
- Sampler auto-detects the third port; no saved port id.

### 6.3 ESP32 ⇄ hub I2C

- ESP32 master `Wire1` (SDA 19, SCL 20); hub slave `Wire` (SDA 18, SCL 19).
- Address **0x42**; 400 kHz; shared with GT911 (0x5D) and PCA9557 (0x18).
- A single mutex serializes `Wire1` between the touch read and the hub poll.
- Suggested register map (can be refined during implementation):

| Address | Contents |
|---|---|
| `0x00` | magic `JK`, version |
| `0x04` | response counts (right, left) |
| `0x08` | command space (right, left) |
| `0x10…` | response FIFO (read; board-tagged frames) |
| `0x50…` | command FIFO (write; board-tagged frames) |
| `0x80…` | 6 knob values (u16 each) |
| `0x90…` | 6 knob configs: target ID, CC/note, channel, min, max, flags |
| `0xD0…` | button states/configs |
| `0xE0` | commit (1 = save config to EEPROM) |

- The hub's `onRequest`/`onReceive` only copy ring-buffer bytes; the main loop
  does the UART/MIDI work. No parsing or blocking in the I2C handlers.
- The ESP32 reads knob values at 20–50 Hz for the screen; writes config on
  Save; reads it back on boot.

### 6.4 Fixed control table and drop-down

The sampler's programmed controls are a **fixed table** shared by the ESP32 UI
and the sampler. Each row has a stable control ID, label, CC or note,
resolution, range and sampler target. Example shape (values to be filled when
the sampler control model is defined):

| ID | Label | CC/Note | Resolution | Sampler target |
|---|---|---|---|---|
| 0 | Selected Pad Pitch | CC 20 | 14-bit | selected pad pitch |
| 1 | Master Volume | CC 7 | 7-bit | master gain |
| 2 | Selected Pad Volume | CC 21 | 7-bit | selected pad volume |
| … | … | … | … | … |
| 100 | Panic | Note 0 / CC | — | all notes off |

- The ESP32's knob/button screen shows a **drop-down per control** listing the
  table. Selecting a row writes its control ID, CC/note, channel and range to
  the hub over I2C.
- The hub stores the selection in EEPROM and sends the CC/note when the knob
  or button moves.
- The sampler's **fixed map** maps the CC/note to the parameter/action. No
  SysEx or MIDI learn is needed.
- Keep the table in one shared place (e.g. `Docs/CONTROL_MAP.md` or a shared
  header) so the ESP32 drop-down and the sampler map cannot drift. If the
  sampler's control list changes, update the table in both places.

---

## 7. Two-Teensy test procedure

1. Move the left Teensy from `U_L` to `U_UI`. Leave `U_L` empty.
2. Flash the hub firmware:
   - `pio run -e hub -t upload` from `jaiba-hexa-drum-ui` (auto-detect port,
     only one Teensy should be in bootloader at a time).
3. Keep the right sensor in `U_R` with its existing firmware.
4. Wire the ESP32 display's I2C (J6/J4 SDA/SCL/GND) to the hub's U_UI
   pins 18/19 + GND; power the display from its own 5 V/2 A supply; common
   ground. Do not bridge the display's 3.3 V to the hub's 3.3 V.
5. Connect a 10K pot to a free U_UI analog pin (e.g. A2/16), 3.3 V/GND, for a
   simulated knob.
6. Confirm:
   - Hub enumerates as `Jaiba Knobs MIDI 1`.
   - Right sensor stream reaches the ESP32 (monitor pads move).
   - Hub test mode injects fake left lines; the ESP32 left tiles react.
   - The pot sends CC; the sampler's fixed map receives it; the ESP32 shows
     the value and can assign it from the drop-down.
   - The touch UI stays responsive with the hub polling (see §12).
7. The sampler sees two ports (`Jaiba Right`, `Jaiba Knobs`); `Jaiba Left`
   stays retrying. This is expected until the third Teensy/left sensor is
   restored.

---

## 8. ESP32 firmware plan

**Base:** the vendor `example/V3.0/PlatformIO70` from
`Elecrow-RD/CrowPanel-7.0-HMI-ESP32-Display-800x480`:

- pioarduino platform 55.03.38, Arduino 3.3.8, **LVGL 9.1.0**,
  **LovyanGFX 1.2.25** + `scripts/patch_lovyangfx.py`.
- Board JSON `qio_opi`, 4 MB flash, 8 MB octal PSRAM, `huge_app.csv`
  (3 MB app + 896 KB SPIFFS).
- 800×480 RGB panel at 24 MHz, two full-frame buffers in PSRAM.
- GT911 touch on `Wire1` 19/20, PCA9557 reset 0x18.
- Remove the DHT20/LED demo; keep the display/touch/LVGL init.

**Host note (2026-11-02):** the scaffold uses the vendor pioarduino 55.03.38 /
Arduino 3.3.8 base. The official `espressif32` 2.0.17 core cannot compile the
vendor's LovyanGFX RGB patch (it uses the ESP-IDF 5.x `esp_lcd` APIs). The
pioarduino penv setup uses the standalone `uv` binary on hosts without
`python3-venv`/`ensurepip`.

**New code:**

- `BridgeTransport`: reads/writes board-tagged frames over I2C; one
  `DrumLink` per board. Port the old `uart_protocol`/`drum_state` to
  `NUM_PADS` 9/7 (right/left) and per-link pending ACKs.
- `KnobConfig`: the fixed control table, per-control drop-down, hub read/write
  of values/config, NVS persistence.
- Screens: **Monitor** (right 9 + left 7 + 5 loop placeholders, hit flash),
  **Knobs** (6 rows, values, control drop-down, Save), **Tuning**,
  **Calibration**. Status bar `R/L/K` and hub/touch state.
- Tasks: LVGL + touch (core 1), I2C hub poll (core 0), shared `Wire1` mutex.
- UART0/J10 stays free for programming; SD stays free for assets.
- ESP32 NVS stores UI prefs, labels, selected control IDs.

The ESP32 does **not** connect to the sensor UARTs directly. Its only link to
the PCB is I2C + 5 V/GND.

---

## 9. Teensy hub firmware plan

Target: Teensy 4.1, `USB_MIDI_SERIAL`, `[env:hub]` in
`jaiba-hexa-drum-ui`.

- `Serial1.begin(115200)` — right sensor (U_UI 0/1).
- `Serial3.begin(115200)` — left sensor (U_UI 15/14).
- `Serial` (USB) — debug only.
- `usbMIDI` — product string `Jaiba Knobs`; CC/notes.
- `Wire.begin(0x42)` — I2C slave; register/FIFO map from §6.3.
- `EEPROM` — knob/button config (magic/version); load on boot, save on commit.
- Main loop:
  - read both UARTs non-blocking, push bytes into per-board response FIFOs
    (board tag);
  - drain per-board command FIFOs into the UARTs;
  - read knobs/buttons, send USB-MIDI on change;
  - service the I2C register buffer;
  - test-mode hooks.
- No blocking calls; the I2C handlers only move bytes.
- Watchdog: reset the hub if the loop stalls; the ESP32 shows hub-offline.
- Test mode (compile flag or USB serial command):
  - inject fake left sensor lines (`HIT`, `PADVAL`, `CAL_STATE`);
  - synthesize a knob sweep if no pot is wired;
  - dump bridge frames/knob values on USB Serial.

The hub is a single point of failure for **display monitoring/config**, not
for audio/MIDI. If it hangs, the drums and sampler keep working.

---

## 10. Sampler plan

- Add **`Jaiba Knobs`** to the auto-detect list (`kNumAutoMidiInputs = 3`);
  status `R/L/K`.
- Fixed control table: control ID → sampler parameter/action; CC/note;
  7/14-bit. The ESP32 drop-down and the sampler table share the same IDs.
- `handleIncomingMidiMessage`: handle CC/notes from the hub; apply to the
  selected pad/parameter; use atomics or a small queue so the MIDI thread
  never blocks; the audio thread reads the atomic target.
- Persist the sampler-side map in settings/kit; the hub persists the selected
  CC/note; the ESP32 persists labels.
- Disconnect behavior: the current auto-input watchdog panics all pads on any
  input loss. With a third port, refine it so a knob-hub disconnect does not
  silence the sensor drums (panic only sensor slots, or track notes per
  source).
- Bench test with two Teensys: right sensor + hub; left absent; the sampler
  receives right MIDI and knob CC. Final: three ports.

---

## 11. I2C sharing and touch lag

Sharing the I2C bus with the GT911 does **not** create a meaningful lag for
the touch UI, and it cannot add lag to the audio/MIDI path (those are USB).

- 400 kHz I2C: ~22.5 µs per byte.
- GT911 touch is polled at ~30 ms; a touch read is ~0.2–0.5 ms.
- Hub status read is ~1–2 bytes (~45 µs); a worst-case 65-byte sensor frame is
  ~1.5 ms.
- The touch can wait at most one short hub transaction; that is <5% of one
  30 ms touch poll. The GT911 poll and the LVGL full-frame flush dominate the
  perceived touch latency.
- Status-first polling: only read a FIFO when the status says data is
  available; keep reads ≤32–64 bytes.
- One mutex around all `Wire1` transactions; the touch read and the hub poll
  share it; the full-frame flush does not hold the bus.
- The sensor traffic is event-driven (`HIT`, `CAL_STATE`, `PADVAL`), not a
  continuous stream; the knob values are 20–50 Hz. The bus is not saturated.
- Keep the display-to-hub I2C cable short (<20–30 cm), twisted with GND; use
  the display's existing pull-ups. If edges are slow or NACKs appear, drop to
  100 kHz; the touch/knob still feel fine and the audio/MIDI are unaffected.
- Test the Teensy 4.1 `Wire` slave at 400 kHz early. If the hub's slave is
  marginal, 100 kHz is a safe fallback.
- The PCA9557 is written only at touch init/reset; no runtime cost.

**Bottom line:** the I2C sharing can add at most a millisecond or two to the
display's touch/monitoring response, far below the 30 ms GT911 poll. It cannot
add lag to the knobs' USB-MIDI or the sensor USB-MIDI.

---

## 12. Phases and acceptance

### Phase 0 — project setup
- Create `jaiba-hexa-drum-esp32-ui/` from the vendor PlatformIO70 base.
- Add `[env:hub]` + `src/hub/` to `jaiba-hexa-drum-ui`.
- Update workspace root `.gitignore`/README for the new folder.
- **Accept:** vendor demo builds and runs on the ESP32 (display, touch,
  LVGL); hub env builds.

### Phase 1 — hub bring-up (two Teensys)
- Flash the hub to the left Teensy in U_UI.
- Verify `Jaiba Knobs` USB-MIDI, I2C 0x42 slave, Serial1/Serial3 bridge,
  EEPROM, test mode, pot → CC.
- **Accept:** ESP32 reads hub registers; right sensor stream appears; fake
  left lines appear; pot CC reaches the sampler.

### Phase 2 — ESP32 bridge + monitor
- `BridgeTransport`, per-board `DrumLink`, port the sensor parser/state.
- Monitor screen right/left; fake left; hit flash; status bar.
- Knob config screen, fixed control table/drop-down, Save to hub.
- **Accept:** monitor shows right pads live; fake left works; knob assignment
  stored in hub EEPROM; touch remains responsive (§11).

### Phase 3 — sampler third input
- Auto-detect `Jaiba Knobs`; fixed map; CC/note handling; 14-bit pitch;
  persistence; disconnect refinement.
- **Accept:** right sensor + hub work together; sampler receives knob CC;
  setup survives restart; left absent is handled gracefully.

### Phase 4 — final hardware
- Knobs/buttons hardware and pin map.
- Third Teensy in U_UI; left sensor back in U_L.
- Three MIDI ports; both sensor monitors; 3-Teensy power/USB.
- **Accept:** full left/right monitor + knob config + sampler mapping; 5 loop
  placeholders remain until the loop backlog is implemented.

---

## 13. Risks and open items

- **Two-Teensy test:** the left sensor is absent; the hub's Serial3 is idle.
  Use the test-mode fake left stream. The left sensor pads are not involved.
- **Hub I2C slave at 400 kHz:** test early; 100 kHz fallback.
- **I2C bus sharing with touch:** status-first polling, short reads, mutex,
  short cable; see §11.
- **Hub single point for display/config:** audio/MIDI direct; watchdog;
  hub-offline status.
- **Hub firmware complexity:** two UARTs + knobs + USB-MIDI + I2C + EEPROM;
  the Teensy 4.1 has the resources; keep the main loop non-blocking.
- **Knob/button pins TBD:** hub can be scaffolded with 6 knob slots and
  configurable buttons; fill the pin map later.
- **Sampler fixed table sync:** the ESP32 drop-down and the sampler map must
  use the same control IDs/CCs/notes. Keep the table in one shared doc/header.
- **Sampler disconnect behavior:** refine the auto-input panic so a hub
  disconnect does not stop the sensor drums.
- **ESP32 flash:** 4 MB, 3 MB app + 896 KB SPIFFS; keep LVGL assets small or
  use the SD card (free, because the hub bridges the sensors).
- **5 loop pads:** still pending on the sensor side; leave UI placeholders.
- **Third Teensy power/USB:** U_UI socket 5 V/3.3 V; avoid back-feed when the
  hub is also USB-connected; common ground with the display's 5 V/2 A.
- **Legacy UI:** the old TFT_eSPI env is reference-only; its `UI_PLAN` /
  `UART_PROTOCOL` are superseded by the ESP32 plan and the bridge protocol.

---

## 14. First actions

1. Approve the four-folder layout and the two-Teensy test procedure.
2. Create the ESP32 project from the vendor PlatformIO70 base; build/flash the
   vendor demo.
3. Add `[env:hub]` and scaffold the hub; flash it to the left Teensy in
   U_UI.
4. Implement the I2C FIFO/bridge and the ESP32 `BridgeTransport`; verify the
   right sensor stream and the fake left.
5. Add the sampler third input and the fixed control table.
6. Define knob/button hardware later; then move the hub to the third Teensy
   and restore the left sensor.

---

## 15. References

- CrowPanel ESP32 HMI 7.0" wiki:
  https://www.elecrow.com/wiki/esp32-display-702727-intelligent-touch-screen-wi-fi26ble-800480-hmi-display.html
- PlatformIO/LVGL tutorial:
  https://www.elecrow.com/wiki/CrowPanel_ESP32_7.0-inch_with_PlatformIO.html
- Vendor repo:
  https://github.com/Elecrow-RD/CrowPanel-7.0-HMI-ESP32-Display-800x480
- Vendor example used as the base:
  `example/V3.0/PlatformIO70`
- Sensor pin map and UART rules:
  `jaiba-hexa-drum-sensor-code/PIN_MAP.md`, `UART_PROTOCOL.md`
- Existing UI protocol/state (to port):
  `jaiba-hexa-drum-ui/src/uart_protocol.*`, `drum_state.*`
- Sampler:
  `jaiba-sampler/` (third input + fixed map)
