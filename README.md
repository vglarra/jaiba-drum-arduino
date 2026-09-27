# Jaiba Drum — Arduino Deliverables & Docs

Single-file **Arduino IDE** sketches for the Jaiba Hexa Drum hardware, plus the
project documentation. **This repo is what is flashed onto the boards** — it is
the live/canonical implementation and may run ahead of the PlatformIO sources.

## Contents

| Path | What it is |
|---|---|
| `SensorFirmware_Arduino/SensorFirmware_Arduino.ino` | Sensor Teensy firmware (piezo + velostat, 6 pads), USB **Serial+MIDI** |
| `UIFirmware_Arduino/UIFirmware_Arduino.ino` | UI Teensy firmware: splash → hex-grid landing (L/R hemispheres) → per-pad tuning panel → Monitor |
| `UIFirmware_Arduino/tft_setup.h` | TFT_eSPI config (ST7796 + XPT2046); paste into the library's `User_Setup.h` for Arduino IDE |
| `Docs/ARDUINO_CLI_SETUP.md` | arduino-cli workflow, board/port identities, upload gotchas |
| `Docs/TEENSY41_MIGRATION_SETUP.md` | setting up the Teensy 4.1 toolchain on a new machine (pinned versions, TFT_eSPI config) |
| `Docs/UI_ROADMAP.md` | UI feature roadmap + phase status |
| `Docs/00-teensy.rules` | PJRC udev rules (install to `/etc/udev/rules.d/00-teensy.rules`) |
| `Docs/AUDIO_WORKSTATION.md` | audio workstation config: Delta 1010LT, ALSA routing, sampler latency settings |
| `Docs/AUDIO_MIGRATION_REPORT.md` | moving the audio tuning + sound-card setup to a new PC (inventory, runbook, findings) |
| `Docs/JAIBA_SAMPLER_ROADMAP.md` | sampler roadmap: multi-MIDI device requirement + latency analysis |
| `Scripts/` | setup/migration tooling for the audio workstation and Teensy toolchain |

## Setup & migration scripts

`Scripts/` holds the machine-setup tooling. The two installers are idempotent
and support a read-only `--verify` mode that reports problems without changing
anything; the flasher supports `--list` and `--dry-run`.

| Script | Purpose |
|---|---|
| `audio-workstation-setup.sh` | apply the audio tuning: lowlatency kernel + `threadirqs`, `performance` governor, `vm.swappiness`, `@audio` limits, ALSA routing, corrected IRQ unit |
| `set-delta-irq-priority.sh` + `.service` | boost the Delta/ICE1712 IRQ thread to SCHED_FIFO, discovering the IRQ by driver name (survives IRQ renumbering) |
| `teensy-setup.sh` | arduino-cli + `teensy:avr` core + pinned libraries + the TFT_eSPI `User_Setup.h` config |
| `teensy-flash.sh` | safe compile + flash; **refuses to run while more than one Teensy is connected** |

See `Docs/AUDIO_MIGRATION_REPORT.md` and `Docs/TEENSY41_MIGRATION_SETUP.md` for
the full runbooks.

## Related repositories (own repos — cloned into this workspace, not tracked here)

- **Sensor PlatformIO source:** https://github.com/vglarra/jaiba-hexa-drum-sensor-code
- **UI PlatformIO source:** https://github.com/vglarra/jaiba-hexa-drum-ui
- **Sampler (JUCE):** https://github.com/vglarra/jaiba-sampler

The `.ino` files are conversions of the PlatformIO sources into single Arduino
IDE sketches, then extended (hex grid, Monitor, per-pad tuning, status LED).
Porting changes back into the PlatformIO repos is a TODO until the Arduino
workflow is retired.

## Hardware quick reference

- **Sensor Teensy** (hub 1-1.5): USB Type must be **Serial + MIDI**
  (`arduino-cli ... :usb=serialmidi`). Pads: velostat A0–A5, piezo A6–A11,
  cal button 3, curve button 4. Status LED on pin 13: heartbeat + flash on
  UART TX.
- **UI Teensy** (hub 1-1.6): ST7796 (SPI 13/11/12/10/8/9) + XPT2046 (CS 6).
- Drum ↔ UI link: hardware Serial1 pins 0/1 crossed, 115200 baud
  (protocol in `UART_PROTOCOL.md` of the component repos).

See `Docs/ARDUINO_CLI_SETUP.md` for exact build/upload commands and the
two-boards-connected upload gotcha; use `Scripts/teensy-flash.sh` to enforce it.
