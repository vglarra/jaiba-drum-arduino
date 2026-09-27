#!/bin/bash
#
# teensy-flash.sh — safe compile + upload helper for the Jaiba Hexa Drum
# Teensy 4.1 boards.
#
# It exists because `arduino-cli upload` IGNORES -p when more than one Teensy
# is connected and auto-searches, silently flashing the Sensor board instead of
# the one you meant. On 2026-09-06 that overwrote the Sensor with the UI hex.
# This script refuses to run when more than one Teensy is plugged in.
#
# Companion to Docs/TEENSY41_MIGRATION_SETUP.md and Docs/ARDUINO_CLI_SETUP.md.
#
# Usage:
#   Scripts/teensy-flash.sh ui            compile + upload the UI firmware
#   Scripts/teensy-flash.sh sensor        compile + upload the Sensor firmware
#   Scripts/teensy-flash.sh --list        show connected Teensy boards and exit
#
# Options:
#   --compile-only        build but do not upload
#   --port DEVICE         force a port (e.g. /dev/ttyACM0)
#   --loader              force the teensy_loader_cli fallback (board must be
#                         in program/bootloader mode)
#   --output-dir DIR      where to put build artifacts (default: /tmp/jaiba-teensy/<target>)
#   --allow-multi         override the one-board guard (DANGEROUS — see above)
#   --dry-run             show what would happen
#   -h, --help            this text
#
# Exit codes: 0 ok, 1 usage/board error, 2 build or upload failure
#
set -euo pipefail

TARGET=""
COMPILE_ONLY=0
FORCE_PORT=""
FORCE_LOADER=0
ALLOW_MULTI=0
DRY_RUN=0
OUT_BASE="${TMPDIR:-/tmp}/jaiba-teensy"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# expected hub paths on the reference machine (informational only — a new
# machine's USB topology may legitimately differ)
EXPECT_SENSOR_HUB="${EXPECT_SENSOR_HUB:-1-1.5}"
EXPECT_UI_HUB="${EXPECT_UI_HUB:-1-1.6}"

MCU="imxrt1062"   # Teensy 4.1

# --------------------------------------------------------------------------
c_red() { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn() { printf '\033[32m%s\033[0m\n' "$*"; }
c_yel() { printf '\033[33m%s\033[0m\n' "$*"; }
c_dim() { printf '\033[2m%s\033[0m\n' "$*"; }
hdr()   { printf '\n\033[1m== %s\033[0m\n' "$*"; }
info()  { printf '  - %s\n' "$*"; }
ok()    { c_grn "  + $*"; }
warn()  { c_yel "  ! $*"; }
die()   { c_red "ERROR: $*" >&2; exit 1; }

usage() { sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

# --------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        ui|sensor)       TARGET="$1"; shift ;;
        --list)          TARGET="__list__"; shift ;;
        --compile-only)  COMPILE_ONLY=1; shift ;;
        --port)          FORCE_PORT="${2:?}"; shift 2 ;;
        --loader)        FORCE_LOADER=1; shift ;;
        --output-dir)    OUT_BASE="${2:?}"; shift 2 ;;
        --allow-multi)   ALLOW_MULTI=1; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        -h|--help)       usage ;;
        *) die "unknown argument: $1 (try --help)" ;;
    esac
done

[ -n "$TARGET" ] || { usage; }

# --------------------------------------------------------------------------
# Teensy USB discovery
#   running board    : 16c0:0483  (USB Serial, has a ttyACM)
#   bootloader board : 16c0:0478  (Halfkay HID, no ttyACM)
# --------------------------------------------------------------------------
declare -a TD_DEV=() TD_PID=() TD_PROD=() TD_TTY=()

find_tty_for() {
    local dev="$1" d
    for d in /sys/bus/usb/devices/"$dev"/*/tty/ttyACM*; do
        [ -e "$d" ] && { basename "$d"; return 0; }
    done
    return 1
}

scan_teensies() {
    TD_DEV=(); TD_PID=(); TD_PROD=(); TD_TTY=()
    local d dev vid pid prod tty
    for d in /sys/bus/usb/devices/*/; do
        [ -f "$d/idVendor" ] || continue
        vid="$(cat "$d/idVendor" 2>/dev/null || true)"
        [ "$vid" = "16c0" ] || continue
        pid="$(cat "$d/idProduct" 2>/dev/null || true)"
        case "$pid" in
            0483|0478) ;;
            *) continue ;;
        esac
        dev="$(basename "$d")"
        prod="$(cat "$d/product" 2>/dev/null || echo 'Teensy')"
        tty="$(find_tty_for "$dev" || true)"
        TD_DEV+=("$dev"); TD_PID+=("$pid"); TD_PROD+=("$prod"); TD_TTY+=("$tty")
    done
}

show_boards() {
    if [ "${#TD_DEV[@]}" -eq 0 ]; then
        warn "no Teensy boards detected (16c0:0483 running, 16c0:0478 bootloader)"
        return 1
    fi
    printf '  %-10s %-10s %-10s %-14s %s\n' "HUB PATH" "USB ID" "MODE" "PORT" "PRODUCT"
    local i mode
    for i in "${!TD_DEV[@]}"; do
        if [ "${TD_PID[$i]}" = "0478" ]; then mode="bootloader"; else mode="running"; fi
        printf '  %-10s %-10s %-10s %-14s %s\n' \
            "${TD_DEV[$i]}" "16c0:${TD_PID[$i]}" "$mode" "${TD_TTY[$i]:-—}" "${TD_PROD[$i]}"
    done
    return 0
}

scan_teensies

if [ "$TARGET" = "__list__" ]; then
    hdr "Connected Teensy boards"
    show_boards || true
    cat <<EOF

  Expected hub paths on the reference machine:
    sensor -> $EXPECT_SENSOR_HUB
    ui     -> $EXPECT_UI_HUB
  (a new machine's USB topology may differ; hub paths are informational)
EOF
    exit 0
fi

# --------------------------------------------------------------------------
# target definition
# --------------------------------------------------------------------------
case "$TARGET" in
    ui)
        SKETCH_DIR="$REPO_ROOT/UIFirmware_Arduino"
        FQBN="teensy:avr:teensy41"
        LABEL="UI Teensy"
        EXPECT_HUB="$EXPECT_UI_HUB"
        ;;
    sensor)
        SKETCH_DIR="$REPO_ROOT/SensorFirmware_Arduino"
        FQBN="teensy:avr:teensy41:usb=serialmidi"
        LABEL="Sensor Teensy"
        EXPECT_HUB="$EXPECT_SENSOR_HUB"
        ;;
esac

OUT_DIR="$OUT_BASE/$TARGET"

hdr "Target: $LABEL"
info "sketch : $SKETCH_DIR"
info "fqbn   : $FQBN"
info "build  : $OUT_DIR"
info "expect hub path: $EXPECT_HUB"

[ -d "$SKETCH_DIR" ] || die "sketch dir not found: $SKETCH_DIR"
command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli not on PATH (run Scripts/teensy-setup.sh)"

# --------------------------------------------------------------------------
# the safety guard
# --------------------------------------------------------------------------
hdr "Connected boards"
show_boards || true

n="${#TD_DEV[@]}"
if [ "$n" -gt 1 ] && [ "$ALLOW_MULTI" -eq 0 ]; then
    echo
    c_red "REFUSING TO FLASH: $n Teensy boards are connected."
    echo "  arduino-cli upload ignores -p with multiple boards and auto-searches,"
    echo "  which silently flashes the Sensor (hub 1-1.5) instead of your target."
    echo
    echo "  Unplug the board you are NOT flashing, then re-run. Or, if you really"
    echo "  know what you are doing: --allow-multi"
    exit 1
fi

# informational hub-path check
if [ "$n" -eq 1 ] && [ "${TD_DEV[0]}" != "$EXPECT_HUB" ]; then
    warn "connected board is at hub ${TD_DEV[0]}, expected $EXPECT_HUB for $LABEL"
    warn "USB topology may differ on this machine — verify this is the right board"
fi

# --------------------------------------------------------------------------
# compile
# --------------------------------------------------------------------------
hdr "Compile"
if [ "$DRY_RUN" -eq 1 ]; then
    c_dim "    [dry-run] arduino-cli compile --fqbn $FQBN --output-dir $OUT_DIR $SKETCH_DIR"
else
    mkdir -p "$OUT_DIR"
    if ! arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT_DIR" "$SKETCH_DIR"; then
        c_red "compile FAILED"
        exit 2
    fi
    ok "compiled -> $OUT_DIR"
fi

[ "$COMPILE_ONLY" -eq 1 ] && { c_grn "Compile-only requested; not uploading."; exit 0; }

# --------------------------------------------------------------------------
# choose port / mode
# --------------------------------------------------------------------------
hex_file=""
for f in "$OUT_DIR"/*.hex; do [ -e "$f" ] && hex_file="$f"; done

# resolve the port unless forced or in loader mode
PORT="$FORCE_PORT"
if [ -z "$PORT" ] && [ "$n" -eq 1 ]; then
    PORT="${TD_TTY[0]}"
fi

if [ "$FORCE_LOADER" -eq 1 ]; then
    # ---------------------------------------------------------------------
    hdr "Upload via teensy_loader_cli (forced)"
    [ -n "$hex_file" ] || die "no .hex found in $OUT_DIR"
    command -v teensy_loader_cli >/dev/null 2>&1 || die "teensy_loader_cli not installed (sudo apt install teensy-loader-cli)"
    echo "  Press the PROGRAM button on the $LABEL now if it is not already in bootloader mode."
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] teensy_loader_cli --mcu=$MCU -v -w $hex_file"
    else
        teensy_loader_cli --mcu="$MCU" -v -w "$hex_file" || { c_red "loader flash FAILED"; exit 2; }
        c_grn "$LABEL flashed via teensy_loader_cli."
    fi
    exit 0
fi

if [ "$n" -eq 1 ] && [ "${TD_PID[0]}" = "0478" ]; then
    # board is sitting in the bootloader — arduino-cli cannot soft-reboot it,
    # so use the CLI loader directly
    hdr "Upload via teensy_loader_cli (board is in bootloader mode)"
    [ -n "$hex_file" ] || die "no .hex found in $OUT_DIR"
    command -v teensy_loader_cli >/dev/null 2>&1 || die "teensy_loader_cli not installed"
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] teensy_loader_cli --mcu=$MCU -v -w $hex_file"
    else
        teensy_loader_cli --mcu="$MCU" -v -w "$hex_file" || { c_red "loader flash FAILED"; exit 2; }
        c_grn "$LABEL flashed via teensy_loader_cli."
    fi
    exit 0
fi

if [ "$n" -eq 0 ]; then
    c_red "No Teensy detected — connect exactly ONE board, then re-run."
    info "build artifacts are ready in $OUT_DIR"
    exit 1
fi

if [ -z "$PORT" ]; then
    die "could not determine a ttyACM port; pass --port /dev/ttyACMx"
fi

# --------------------------------------------------------------------------
# upload
# --------------------------------------------------------------------------
hdr "Upload to $PORT"
info "compile is cached; uploading the existing build"

if [ "$DRY_RUN" -eq 1 ]; then
    c_dim "    [dry-run] arduino-cli upload -p $PORT --fqbn $FQBN --input-dir $OUT_DIR $SKETCH_DIR"
    exit 0
fi

if arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$OUT_DIR" "$SKETCH_DIR"; then
    c_grn "$LABEL uploaded successfully."
    exit 0
fi

warn "arduino-cli upload failed — trying the teensy_loader_cli fallback"
if [ -n "$hex_file" ] && command -v teensy_loader_cli >/dev/null 2>&1; then
    echo "  Press the PROGRAM button on the $LABEL now."
    if teensy_loader_cli --mcu="$MCU" -v -w "$hex_file"; then
        c_grn "$LABEL flashed via teensy_loader_cli."
        exit 0
    fi
fi

c_red "Upload FAILED. Check the udev rules (Docs/TEENSY41_MIGRATION_SETUP.md §4)"
c_red "and the troubleshooting table (§13)."
exit 2
