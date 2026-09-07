# arduino-cli — Jaiba Drum Teensy workflow (project config)

Toolchain: `arduino-cli` (in PATH) + Teensyduino `teensy:avr` core (installed).
Board: Teensy 4.1 (`teensy:avr:teensy41`). See also the .ino headers for the
Arduino-IDE equivalents of the steps below.

## Boards — IMPORTANT identity note (verified on 2026-09-06)

**`/dev/ttyACM0` vs `/dev/ttyACM1` are NOT fixed identities** — Linux numbers
ACM ports by enumeration order. What IS stable is the USB hub path:

| Board              | USB hub path | Typical ACM port (both plugged) |
|--------------------|--------------|----------------------------------|
| Sensor Teensy      | `1-1.5`      | `/dev/ttyACM0` |
| UI Teensy          | `1-1.6`      | `/dev/ttyACM1` |

Check identities with `arduino-cli board list` and/or
`for d in /sys/bus/usb/devices/*/; do ...` (16c0 vendor). Verify before flashing:
a board in the bootloader shows as `16c0:0478` (Halfkay) and has NO ttyACM port;
a running board shows as `16c0:0483` (USB Serial).

## USB permissions — FIXED (2026-09-06)

The PJRC udev rules are now installed at `/etc/udev/rules.d/00-teensy.rules`
(copy kept in this repo at `Docs/00-teensy.rules`). Without them, Teensy uploads
fail: the Halfkay bootloader USB node is `root:root` with no write access, and
ModemManager can grab the serial port. Uploads now work with plain arduino-cli.

## Compile

UI firmware (USB Type = Serial, default is fine):

    arduino-cli compile --fqbn teensy:avr:teensy41 UIFirmware_Arduino

Sensor firmware (needs USB MIDI + Serial):

    arduino-cli compile --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino

### IMPORTANT — USB type option value
The correct FQBN value for "Serial + MIDI" is **`usb=serialmidi`** (no underscore).
`usb=serial_midi` is invalid. `usb=serialmidi` == PlatformIO `-D USB_MIDI_SERIAL`.
The sensor sketch #errors at compile time with setup instructions if compiled
with a non-MIDI USB type.

## Upload (verified working)

**⚠️ CRITICAL GOTCHA — never upload while BOTH Teensys are connected.**
With two boards present, `arduino-cli upload` IGNORES the `-p` port and
auto-searches, always flashing the first board it finds (the Sensor at hub path
1-1.5). This bit us on 2026-09-06: two UI uploads silently overwrote the Sensor
with the UI hex. The tool even warns: "Found 2 Teensy boards, but using
auto-search to find board for upload. Please use Tools > Ports(Teensy)..."

**Rule: unplug the board you are NOT flashing, upload, then replug.** With a
single Teensy connected, upload targets it correctly (e.g. the UI flash used
port path `usb1/1-1/1-1.6 (teensy)`).

UI Teensy (hub path 1-1.6):

    arduino-cli upload -p /dev/ttyACM0 --fqbn teensy:avr:teensy41 UIFirmware_Arduino

Sensor Teensy (hub path 1-1.5, Serial+MIDI type):

    arduino-cli upload -p /dev/ttyACM1 --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino

### Fallback: direct CLI flashing
If arduino-cli upload misbehaves (loader/soft-reboot quirks), flash over HID with
the already-installed `teensy_loader_cli` (works while the board sits in
bootloader mode after pressing its PROGRAM button; Teensy 4 bootloader waits
indefinitely, and only the board in program mode can be flashed):

    teensy_loader_cli --mcu=imxrt1062 -v -w UIFirmware_Arduino.ino.hex

(the .hex lives under ~/.cache/arduino/sketches/<hash>/ after a compile).
The Teensy Loader GUI ("teensy" binary in teensy-tools) may not auto-flash in
this session; prefer the CLI loader.

## Prereqs / gotchas

- UI sketch: TFT_eSPI must be configured before compiling. Applied 2026-09-06:
  `/home/jaiba/Arduino/libraries/TFT_eSPI/User_Setup.h` now contains the Jaiba
  config (ST7796/pins/fonts), identical to `UIFirmware_Arduino/tft_setup.h`;
  the library's original default is backed up as `User_Setup.h.backup`.
- Serial Monitor: sensor debug console 115200 baud; UI console 9600 baud.
  Drum↔TFT UART link is hardware Serial1 at 115200 on both boards (pins 0/1).
- Verified compiles (core teensy:avr 1.62.0): sensor `usb=serialmidi` OK
  (FLASH 47,008 B); UI OK (FLASH 62,000 B — includes the hex_grid landing
  preview from repo commit fe07763). Both firmwares flashed 2026-09-06:
  UI = splash + hexagonal pad grid landing; Sensor = Serial+MIDI drum
  firmware (confirmed by 115200-baud pad stream). UI `UIFirmware_Arduino.ino`
  merges hex_grid.h/.cpp alongside the other modules.
