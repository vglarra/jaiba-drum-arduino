# Teensy 4.1 Toolchain — Migration Setup Guide

**Goal:** stand up the complete `arduino-cli` + Teensyduino workflow for the Jaiba
Hexa Drum (Sensor + UI Teensy 4.1) on a new Ubuntu machine.

**Source machine:** `jaiba-sampler`, Ubuntu 24.04.4 LTS, `arduino-cli` 1.5.1,
`teensy:avr` core 1.62.0.
**Validated:** both sketches were **compiled successfully during this audit** —
see §9 for the exact memory figures.
**Companion:** `Docs/ARDUINO_CLI_SETUP.md` is the *day-to-day workflow*
(port identities, upload gotchas, serial monitor). This document is the
*machine setup*; read both.

A ready-to-run installer is at `Scripts/teensy-setup.sh`, and a safe flasher that
enforces the two-board rule is at `Scripts/teensy-flash.sh`.

---

## 1. What actually has to move

`arduino-cli` is a single static Go binary with no system dependencies, so there
is nothing to compile and no `apt` library hunting. The whole toolchain lives in
**two directories**:

| Path | Contents | Size |
|---|---|---|
| `~/.arduino15/` | config, package index, `teensy:avr` core + compiler toolchain | ~1.5 GB |
| `~/Arduino/libraries/` | the three user libraries | small |
| `~/bin/arduino-cli` | the CLI binary itself | 37 MB |

**You can either copy these across or rebuild them from scratch.** Rebuilding is
recommended and is what the script does — it takes a few minutes on a decent
connection and guarantees clean state. Copying `~/.arduino15` between machines
also works (it is self-contained), but the **compiler toolchain has absolute
paths baked in**, so cross-machine copies are fragile. Prefer a rebuild.

Things that are **not** in those directories and must be handled separately:

1. `/etc/udev/rules.d/00-teensy.rules` — required for uploads (§4).
2. `~/Arduino/libraries/TFT_eSPI/User_Setup.h` — **hand-edited per project**,
   not installed by any package manager (§6).
3. `teensy_loader_cli` — the fallback flasher, an apt package (§7).

---

## 2. Target versions (pin these)

Version drift here causes subtle, hard-to-debug failures — especially TFT_eSPI,
whose `User_Setup.h` is project-specific and whose pin macros change between
releases.

| Component | Version on source machine | Pin? |
|---|---|---|
| `arduino-cli` | 1.5.1 | any 1.x |
| `teensy:avr` core | **1.62.0** | pin |
| `teensy:teensy-compile` | 15.2.1 (auto) | auto |
| `teensy:teensy-tools` | 1.62.0 (auto) | auto |
| TFT_eSPI | **2.5.43** | **pin** — `User_Setup.h` layout depends on it |
| MIDI Library | 5.0.2 | pin |
| XPT2046_Touchscreen | 1.4 | pin (pulled by the UI code; touch is polled manually but the lib is present) |

> The `teensy:avr` core version is the *Arduino IDE* platform version, not the
> Teensyduino number. 1.62.0 corresponds to Teensyduino 1.62.

---

## 3. Install `arduino-cli`

### Option A — official installer (recommended on a new machine)

```bash
mkdir -p ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
  | BINDIR=~/.local/bin sh
~/.local/bin/arduino-cli version
```

Ensure `~/.local/bin` is on `PATH` (Ubuntu's default `~/.profile` already adds it
when the directory exists — log out/in afterwards).

### Option B — match the source machine exactly (what is deployed here)

The source machine has a **manually downloaded static binary** at
`~/bin/arduino-cli`, with `export PATH="$HOME/bin:$PATH"` in `~/.bashrc:123`:

```bash
mkdir -p ~/bin
cd /tmp
VER=1.5.1
curl -fsSL -o arduino-cli.tar.gz \
  "https://github.com/arduino/arduino-cli/releases/download/v${VER}/arduino-cli_${VER}_Linux_64bit.tar.gz"
tar -xzf arduino-cli.tar.gz arduino-cli
install -m 0755 arduino-cli ~/bin/arduino-cli
grep -q 'HOME/bin' ~/.bashrc || echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc
```

Either way, verify: `arduino-cli version` → `1.5.1` (or newer 1.x).

---

## 4. udev rules (do this BEFORE plugging in a Teensy)

```bash
sudo install -m 0644 Docs/00-teensy.rules /etc/udev/rules.d/00-teensy.rules
sudo udevadm control --reload
# then unplug/replug the Teensys
```

**Why it matters:** without these rules a Teensy upload fails. The Halfkay
bootloader USB node is `root:root` with no write access, and ModemManager
interferes with the USB serial port (symptoms: the Arduino Serial Monitor drops
incoming bytes, and uploads fail with *"Unable to open /dev/ttyACM0 for reboot
request"*). The rules also set `ID_MM_DEVICE_IGNORE` so ModemManager leaves the
Teensy alone.

> The repo copy at `Docs/00-teensy.rules` is byte-identical to the one installed
> on the source machine. `/etc/udev/rules.d/49-teensy.rules` also exists there
> but is an obsolete tombstone — ignore it.

---

## 5. Configure the board manager and install the core

```bash
# 1. register the PJRC package index
arduino-cli config add board_manager.additional_urls \
  https://www.pjrc.com/teensy/package_teensy_index.json

# 2. refresh indexes and install the core
arduino-cli core update-index
arduino-cli core install teensy:avr

# 3. verify
arduino-cli core list
#    ID          Installed  Latest  Name
#    teensy:avr  1.62.0     1.62.0  Teensy (for Arduino IDE 2.0.4 or later)
```

The result on the source machine is `~/.arduino15/arduino-cli.yaml` containing
exactly:

```yaml
board_manager:
  additional_urls:
    - https://www.pjrc.com/teensy/package_teensy_index.json
```

`arduino-cli core install` automatically pulls the required tools
(`teensy:teensy-compile` 15.2.1 — the ARM cross-compiler — and
`teensy:teensy-tools` 1.62.0). This is the multi-hundred-MB step; give it time.

> To pin the core version: `arduino-cli core install teensy:avr@1.62.0`.

---

## 6. Install libraries and configure TFT_eSPI

```bash
arduino-cli lib install "MIDI Library@5.0.2"
arduino-cli lib install "TFT_eSPI@2.5.43"
arduino-cli lib install "XPT2046_Touchscreen@1.4"

arduino-cli lib list
```

### ⚠️ The critical manual step: TFT_eSPI's `User_Setup.h`

TFT_eSPI ships with a **default `User_Setup.h` that does not match this
hardware**, and it is not something a package manager can set. The UI sketch
will fail to compile or drive the wrong pins until it is replaced.

The project's authoritative config lives in the repo at
`UIFirmware_Arduino/tft_setup.h` (ST7796 + XPT2046, SPI 13/11/12/10/8/9, touch
CS 6). Apply it:

```bash
TFT=~/Arduino/libraries/TFT_eSPI

# back up the vendor default exactly once
[ -f "$TFT/User_Setup.h.backup" ] || cp -a "$TFT/User_Setup.h" "$TFT/User_Setup.h.backup"

# apply the Jaiba config
cp -a UIFirmware_Arduino/tft_setup.h "$TFT/User_Setup.h"

# verify: must print IDENTICAL
diff -q UIFirmware_Arduino/tft_setup.h "$TFT/User_Setup.h" && echo IDENTICAL
```

Also confirm `TFT_eSPI/User_Setup_Select.h` still includes `User_Setup.h` by
default (it does on a stock install — the line `#include <User_Setup.h>` is
active, and every alternative setup is commented out).

> The sketch does **not** `#include "tft_setup.h"`. That file is a *paste-in
> reference* for the library's own config, which is why it must be copied by
> hand. Do not "fix" this by adding an include to the sketch — TFT_eSPI reads
> its configuration from its own directory at compile time.

> Advanced alternative: TFT_eSPI honours `__has_include(<tft_setup.h>)`, so
> PlatformIO (and some Arduino setups) can pick the file up from the sketch
> folder instead. The Arduino IDE route is the pasted copy described above, and
> that is what is deployed here.

---

## 7. Fallback flasher: `teensy_loader_cli`

```bash
sudo apt install teensy-loader-cli     # installs /usr/bin/teensy_loader_cli
```

Use it when `arduino-cli upload` misbehaves (loader/soft-reboot quirks). The
Teensy 4 bootloader waits indefinitely, and **only a board in program mode can
be flashed**:

```bash
# press the PROGRAM button on the Teensy first, then:
teensy_loader_cli --mcu=imxrt1062 -v -w /path/to/Sketch.ino.hex
```

`imxrt1062` is the Teensy 4.1 MCU. Find the hex under
`~/.cache/arduino/sketches/<hash>/` after a compile, or use the `--output-dir`
shown in §8. The Teensy Loader GUI may not auto-flash; prefer this CLI.

---

## 8. Build the firmware

Both commands below were **run and verified during this audit**.

```bash
# UI Teensy — USB Type "Serial" (the default; no :usb= suffix needed)
arduino-cli compile --fqbn teensy:avr:teensy41 UIFirmware_Arduino

# Sensor Teensy — REQUIRES "Serial + MIDI"
arduino-cli compile --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino
```

To collect the `.hex` in a known location (useful for the fallback flasher):

```bash
arduino-cli compile --fqbn teensy:avr:teensy41:usb=serialmidi \
  --output-dir /tmp/jaiba-teensy/sensor SensorFirmware_Arduino
```

### The `usb=serialmidi` trap

The correct FQBN value for "Serial + MIDI" is **`usb=serialmidi`** — no
underscore. `usb=serial_midi` is **invalid** and will be rejected. It is
equivalent to PlatformIO's `-D USB_MIDI_SERIAL`.

The sensor sketch enforces this with a deliberate compile-time guard
(`SensorFirmware_Arduino.ino:62-64`):

```c
#if !defined(USB_MIDI_SERIAL) && !defined(USB_MIDI4_SERIAL) && ...
  #error "SensorFirmware_Arduino requires USB Type 'Serial + MIDI'. ...
          arduino-cli: --fqbn teensy:avr:teensy41:usb=serialmidi."
#endif
```

So a wrong FQBN fails **loudly with instructions** instead of a cryptic
`'usbMIDI' was not declared in this scope`.

> USB Type is a **board setting**, not a sketch `#define`. The comment block at
> the top of the sensor sketch explains why: `#define USB_MIDI_SERIAL` in a
> sketch never reaches the core sources. `#include <usb_midi.h>` cannot
> substitute for the board setting either.

---

## 9. Validated build results (source machine, 2026-09-27)

| Sketch | FQBN | FLASH code | FLASH data | RAM1 vars | RAM2 vars | Result |
|---|---|---|---|---|---|---|
| `UIFirmware_Arduino` | `teensy:avr:teensy41` | 80,432 | 24,736 | 11,456 | 12,416 | ✅ exit 0 |
| `SensorFirmware_Arduino` | `teensy:avr:teensy41:usb=serialmidi` | 47,264 | 9,264 | 11,936 | 17,568 | ✅ exit 0 |

Both leave ~8 MB of program space free on the 4.1. If your numbers are wildly
different (or TFT_eSPI-related compile errors appear), the `User_Setup.h`
step in §6 did not take effect.

> `ARDUINO_CLI_SETUP.md` records older figures (UI ≈62,000 B, sensor ≈47,008 B).
> The UI grew because the hex-grid landing, per-pad tuning and Monitor were
> merged into the single sketch. The sensor is essentially unchanged.

---

## 10. Board identity, ports and the two-board rule

### 10.1 `/dev/ttyACM*` is NOT a stable identity

Linux numbers ACM ports by enumeration order. What *is* stable is the USB hub
path:

| Board | USB hub path | USB ID (running) | USB ID (bootloader) |
|---|---|---|---|
| Sensor Teensy | `1-1.5` | `16c0:0483` | `16c0:0478` |
| UI Teensy | `1-1.6` | `16c0:0483` | `16c0:0478` |

A board in the Halfkay bootloader shows as `16c0:0478` and has **no ttyACM
port**; a running board shows `16c0:0483` (USB Serial). Always check before
flashing.

### 10.2 ⚠️ Never upload while BOTH Teensys are connected

This is the single most damaging gotcha in this project. With two boards
present, **`arduino-cli upload` ignores `-p` and auto-searches**, always
flashing the first board it finds (the Sensor at hub path `1-1.5`). On
2026-09-06 two UI uploads **silently overwrote the Sensor with the UI hex**.
The tool only warns:

> *"Found 2 Teensy boards, but using auto-search to find board for upload.
> Please use Tools > Ports(Teensy)..."*

**Rule: unplug the board you are NOT flashing → upload → replug.**

`Scripts/teensy-flash.sh` enforces this in code: it counts connected Teensy USB
devices and refuses to proceed if more than one is present.

### 10.3 Upload commands (single board connected)

```bash
# UI Teensy (hub 1-1.6)
arduino-cli upload -p /dev/ttyACM0 --fqbn teensy:avr:teensy41 UIFirmware_Arduino

# Sensor Teensy (hub 1-1.5, Serial + MIDI)
arduino-cli upload -p /dev/ttyACM1 --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino
```

With exactly one board connected, upload targets it correctly regardless of the
port number (the flash is confirmed by port path, e.g. `usb1/1-1/1-1.6
(teensy)`). Prefer `Scripts/teensy-flash.sh sensor|ui`, which picks the right
FQBN, checks board count, and falls back to `teensy_loader_cli`.

---

## 11. Serial monitor and debug

| Stream | Baud | Notes |
|---|---|---|
| Sensor USB Serial console | **115200** | commands `c` / `e <num>` / `+` / `-` / `p` |
| UI USB Serial console | **9600** | `Serial.begin(9600)` at `UIFirmware_Arduino.ino:1423` |
| Drum ↔ UI UART link | **115200** | hardware `Serial1`, pins 0 (RX1) / 1 (TX1), crossed TX↔RX, shared GND |

```bash
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200   # sensor
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=9600     # UI
```

Full protocol: `UART_PROTOCOL.md` in each component repo.

---

## 12. Verification checklist

```bash
arduino-cli version                     # 1.x
arduino-cli config dump                 # shows the pjrc.com URL
arduino-cli core list                   # teensy:avr 1.62.0
arduino-cli lib list                    # the 3 libs, pinned versions
ls -l /etc/udev/rules.d/00-teensy.rules # present

diff -q UIFirmware_Arduino/tft_setup.h ~/Arduino/libraries/TFT_eSPI/User_Setup.h

arduino-cli compile --fqbn teensy:avr:teensy41 UIFirmware_Arduino
arduino-cli compile --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino

# with exactly ONE Teensy plugged in:
arduino-cli board list                  # should show the board on ttyACM*

# USB identity of whatever is plugged in:
for d in /sys/bus/usb/devices/*/; do
  v=$(cat "$d/idVendor" 2>/dev/null); p=$(cat "$d/idProduct" 2>/dev/null)
  [ "$v" = "16c0" ] && echo "$(basename $d)  $v:$p  $(cat $d/product 2>/dev/null)"
done
```

Then run `Scripts/teensy-flash.sh --list` to see detected boards and their hub
paths.

---

## 13. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `'usbMIDI' was not declared in this scope` | Wrong USB Type | Add `:usb=serialmidi` to the FQBN (the `#error` guard should have caught this) |
| `usb=serial_midi` rejected / compile error | Invalid FQBN option value | Use `usb=serialmidi` (no underscore) |
| Upload flashes the wrong board / one board stops working | Both Teensys connected | Unplug the other board; see §10.2. Re-flash the overwritten board |
| `Unable to open /dev/ttyACM0 for reboot request` | Missing udev rules or ModemManager interference | Install `00-teensy.rules`, reload udev, replug; consider removing ModemManager |
| TFT blank / wrong colours / touch dead | `User_Setup.h` not applied | §6; confirm `diff -q` prints IDENTICAL |
| `TFT_eSPI.h: No such file or directory` | Library not installed | `arduino-cli lib install "TFT_eSPI@2.5.43"` |
| Compile error mentioning unknown TFT pins | TFT_eSPI version drift | Pin to `TFT_eSPI@2.5.43` |
| `arduino-cli: command not found` | `~/bin` or `~/.local/bin` not on `PATH` | Add to `~/.bashrc`, then `source ~/.bashrc` |
| Core install fails / is very slow | Large toolchain download (hundreds of MB) | Retry; check the PJRC URL is registered first |
| `teensy_loader_cli` not found | Package not installed | `sudo apt install teensy-loader-cli` |
| Board not detected at all | Charge-only USB cable, or hub power | Use a data cable; connect directly |

---

## 14. Repo layout note

This repo tracks the **single-file Arduino sketches that are flashed to the
boards** — it is the canonical/live implementation. The PlatformIO sources live
in their own repositories (`jaiba-hexa-drum-sensor-code`,
`jaiba-hexa-drum-ui`) and are cloned into the workspace but **not tracked here**.
Porting the Arduino-side changes back to PlatformIO is a standing TODO.

---

## 15. Appendix — raw captured state (source machine)

```
arduino-cli          1.5.1 (commit 01f3d4f2b, 2026-06-05), static x86-64 Go binary
binary path          /home/jaiba/bin/arduino-cli   (PATH via ~/.bashrc:123)
config file          ~/.arduino15/arduino-cli.yaml
  board_manager.additional_urls:
    - https://www.pjrc.com/teensy/package_teensy_index.json
data dir             ~/.arduino15       (index + packages, ~1.5 GB)
user dir             ~/Arduino          (libraries)
core                 teensy:avr 1.62.0
tools                teensy-compile 15.2.1, teensy-tools 1.62.0,
                     teensy-discovery 1.62.0, teensy-monitor 1.62.0
libraries (user)     MIDI Library 5.0.2, TFT_eSPI 2.5.43, XPT2046_Touchscreen 1.4
TFT config           ~/Arduino/libraries/TFT_eSPI/User_Setup.h
                     == UIFirmware_Arduino/tft_setup.h   (verified IDENTICAL)
                     vendor default kept at User_Setup.h.backup (18,621 B)
udev                 /etc/udev/rules.d/00-teensy.rules  (repo copy identical)
                     /etc/udev/rules.d/49-teensy.rules  (obsolete tombstone)
fallback flasher     /usr/bin/teensy_loader_cli  (apt: teensy-loader-cli)
board                teensy:avr:teensy41  (Teensy 4.1, modelID 0x25)
hub paths            sensor 1-1.5, UI 1-1.6
```
