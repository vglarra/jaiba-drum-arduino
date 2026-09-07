# Jaiba Drum UI — Development Roadmap

Status log for the touchscreen UI (Teensy 4.1 + ST7796 + XPT2046). Working
artifact: `UIFirmware_Arduino/UIFirmware_Arduino.ino` (single-file Arduino
version; upstream PlatformIO source lives in `jaiba-hexa-drum-ui/` at commit
fe07763 — hex grid landing preview).

Last updated: 2026-09-06

---

## System context (why the UI exists)

- 1–2 **Sensor Teensys**: read piezo + velostat pads, output USB-MIDI
  (percussive path, always lowest latency), and expose tuning/calibration +
  live events over hardware UART (Serial1, 115200, pins 0/1).
- **UI Teensy**: 320x480 touchscreen. Shows the drum shell as a **9-tile hex
  grid** (`hex_grid` widget, positions from the drum CAD). Planned roles:
  1. Per-pad tuning/monitoring of the Sensor Teensy(ies) over UART.
  2. Diagnostic / link-health screen ("is the sensor up?") — **Phase 1**.
  3. (future) USB-MIDI **CC controller** for jaiba-sampler (real knobs).
- **jaiba-sampler** (`jaiba-sampler/`): standalone JUCE audio sampler with
  MIDI input (per-pad note/channel/device config already exists).

### Future multi-board topology
Tile → sensor mapping will grow to `(teensyId, padIndex)` once a second sensor
Teensy is added (7 more pads). Keep tile data + monitor code ready for that.

---

## LATENCY GUARANTEE (design invariant)

The percussive path is ALWAYS: Sensor → USB-MIDI → sampler/DAW.
**Never route drum triggers through the UI Teensy.** The UART link is a
monitoring/control side channel only; UI work must never add work to the
sensor's loop beyond trivial non-blocking Serial1 parsing. Phase 1 changes
touch only the UI Teensy firmware (sensor untouched).

---

## Screen flow (target)

```
SPLASH ──► LANDING  (hex grid + bottom footer)
              ├─ tap populated hex ──► PAD DETAIL  (per-pad note/threshold/
              │                        ceiling/curve, ─/＋ → SET_*, Back)   [Ph.2]
              ├─ tap empty hex ──► toast "not installed"
              └─ footer [Monitor] ──► MONITOR  (link status + event log +
                                     per-pad readback, Back)               [Ph.1]
              └─ footer reserved: [Tuning] [Pad Assign] [Settings]
```

## Phase plan

### Phase 1 — MONITOR panel + navigation (IMPLEMENTED 2026-09-06, flashed)
- Footer bar on the hex landing screen (42 px) with a `[Monitor]` button.
- Real touch routing: mapped (x,y) → active screen (buttons/footer hit-test);
  hex tap area reserved for Phase 2.
- `MONITOR` screen:
  - Sensor link status: ONLINE / waiting / OFFLINE derived from
    `drumState.lastContactMs` (stamped on every parsed UART line; ~3 s
    timeout).
  - Per-pad readback (note / threshold / ceiling / curve) via `GET_PAD,0..5`
    on entry + periodic refresh (~2 s) while the screen is open.
  - Scrolling event log ring buffer (last 6 events: `HIT`, `ACK`, `ERR`, …).
  - `<-- Back` returns to the hex landing.
- Small additions: `drumState.lastContactMs`, `EventLog` ring buffer, log
  calls in the UART message handlers, ScreenId::MONITOR, screen tap dispatch.
- Does NOT touch the Sensor Teensy firmware (latency invariant).

### Phase 2 — per-pad panel (DONE 2026-09-06, flashed)
- Tap a **populated** hex on the RIGHT view → `PAD_DETAIL` for that pad
  (`sensorIndex` from the tapped tile). Left-view tiles are all unpopulated →
  no panel (nothing wired).
- Panel: live note / threshold / ceiling from `GET_PAD` (fetch on entry +
  every 2 s), read-only curve shown in Monitor. `[-][+]` per row sends
  `SET_NOTE` / `SET_THRESH` / `SET_CEILING_BASELINE` (respects the single
  pending-ACK slot; skips while PENDING). Status line shows applied /
  ERR reason / timed out. `<-- Back` → landing.
- Optimistic local update + ACK-triggered GET_PAD resync keeps the panel in
  sync with the drum Teensy.

### Hemisphere switching + left-side mock (2026-09-06, flashed)
- Geometry source: user-provided Blender CAD script (`flat_hex_plates.py`).
  The shell splits at x=0: RIGHT plate = 9 tiles (R00–R08, x=0 column
  included), LEFT plate = 7 tiles (L00–L06). Script currently uses
  HEX_FLAT_WIDTH=84; UI data is the 77 mm era (right table reproduced the
  repo's R00–R08 exactly at 77 mm, validating the extraction).
- Left hemisphere coords (77 mm, CAD +y-up), all `populated=false` (future
  sensor Teensy 2):
  `L00(-115.5,100) L01(-38.5,100) L02(-154,33.3) L03(-77,33.3)
   L04(-115.5,-33.3) L05(-38.5,-33.3) L06(-77,-100)`
- hex_grid now holds HEX_TILES_RIGHT[9] + HEX_TILES_LEFT[7], and
  `hexSelectSide(bool)` picks the active set for draw/hit-test.
- Landing footer: `[L][R]` hemisphere toggle (active side lit) + caption
  "SENSOR 1 (6 pads)" / "SENSOR 2 (mock)" + `[Monitor]`.
- LEFT view renders 7 dashed tiles (mock); RIGHT unchanged (6 solid pads).
- TODO next: tap a populated hex → per-pad panel (SET_* controls) — the
  original Phase 2 item.

### Phase 3 — (optional) live raw stream
Sensor firmware gains `STREAM,<on|off>` pushing raw per-pad readings over
Serial1; Monitor renders mini raw-value bars. Requires sensor + protocol
changes on both sides (doc in UART_PROTOCOL.md).

### Phase 4 — (future) UI as CC controller for jaiba-sampler
- UI Teensy USB Type → **Serial + MIDI** (board option, like the sensor).
- UI knob screens send `usbMIDI.sendControlChange()`; sampler adds CC→param
  mapping (its MIDI device/note/channel config already exists). Real-time
  (CC round-trip is ms-scale; UI touch poll ~15 ms is the only bound).
- Drum triggers stay Sensor→USB-MIDI→sampler (never through the UI).

---

## Firmware flash gotchas (see Docs/ARDUINO_CLI_SETUP.md)
- Upload with ONLY the target board connected (arduino-cli auto-search
  ignores `-p` when 2 Teensys are present and always grabs hub path 1-1.5).
- Sensor firmware: FQBN `teensy:avr:teensy41:usb=serialmidi`.
- UI firmware: FQBN `teensy:avr:teensy41` (until Phase 4 makes it midi too).

## Open questions
- Persistence of per-pad settings (EEPROM on sensor?) — later.
- How Monitor should indicate *which* sensor link (2nd Teensy later).
