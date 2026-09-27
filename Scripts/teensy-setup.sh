#!/bin/bash
#
# teensy-setup.sh — stand up the arduino-cli + Teensyduino 4.1 toolchain
# for the Jaiba Hexa Drum (Sensor + UI Teensy 4.1) on a new machine.
#
# Companion to Docs/TEENSY41_MIGRATION_SETUP.md — read that first.
#
# Run as your NORMAL user (NOT root): the toolchain is per-user. The script
# calls sudo itself only for the udev rules.
#
# Idempotent: safe to re-run.
#
# Usage:
#   Scripts/teensy-setup.sh [options]
#
# Options:
#   --user NAME           target user (default: current user; must not be root)
#   --core VERSION        teensy:avr version to install (default: 1.62.0)
#   --tft-version V       TFT_eSPI version (default: 2.5.43)
#   --midi-version V      MIDI Library version (default: 5.0.2)
#   --touch-version V     XPT2046_Touchscreen version (default: 1.4)
#   --skip-cli            do not install arduino-cli (assume it is on PATH)
#   --skip-udev           do not touch /etc/udev/rules.d
#   --skip-core           do not install the teensy:avr core
#   --skip-libs           do not install user libraries
#   --skip-tft            do not apply the TFT_eSPI User_Setup.h config
#   --verify              check-only, change nothing
#   --dry-run             print actions without applying them
#   -h, --help            this text
#
# Exit codes: 0 ok, 1 usage/env error, 2 verification found problems
#
set -euo pipefail

TARGET_USER="${SUDO_USER:-$(id -un)}"
CORE_VERSION="1.62.0"
TFT_VERSION="2.5.43"
MIDI_VERSION="5.0.2"
TOUCH_VERSION="1.4"
CLI_VERSION="1.5.1"

DO_CLI=1; DO_UDEV=1; DO_CORE=1; DO_LIBS=1; DO_TFT=1
DRY_RUN=0; VERIFY_ONLY=0

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PJRC_URL="https://www.pjrc.com/teensy/package_teensy_index.json"

# --------------------------------------------------------------------------
c_red() { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn() { printf '\033[32m%s\033[0m\n' "$*"; }
c_yel() { printf '\033[33m%s\033[0m\n' "$*"; }
c_dim() { printf '\033[2m%s\033[0m\n' "$*"; }
hdr()   { printf '\n\033[1m== %s\033[0m\n' "$*"; }
info()  { printf '  - %s\n' "$*"; }
ok()    { c_grn "  + $*"; }
warn()  { c_yel "  ! $*"; WARNINGS=$((WARNINGS + 1)); }
die()   { c_red "ERROR: $*" >&2; exit 1; }

WARNINGS=0

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] $*"
    else
        "$@"
    fi
}

usage() { sed -n '2,36p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

# Installed version of a user library, by exact name ("" if absent).
# `lib list` pads and wraps names in a table, so parse the JSON instead.
lib_version() {
    arduino-cli lib list --json 2>/dev/null | python3 -c '
import json, sys
name = sys.argv[1]
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for e in d.get("installed_libraries", []):
    lib = e.get("library", {})
    if lib.get("name") == name:
        print(lib.get("version", ""))
        break
' "$1"
}

# --------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --user)          TARGET_USER="${2:?}"; shift 2 ;;
        --core)          CORE_VERSION="${2:?}"; shift 2 ;;
        --tft-version)   TFT_VERSION="${2:?}"; shift 2 ;;
        --midi-version)  MIDI_VERSION="${2:?}"; shift 2 ;;
        --touch-version) TOUCH_VERSION="${2:?}"; shift 2 ;;
        --skip-cli)      DO_CLI=0; shift ;;
        --skip-udev)     DO_UDEV=0; shift ;;
        --skip-core)     DO_CORE=0; shift ;;
        --skip-libs)     DO_LIBS=0; shift ;;
        --skip-tft)      DO_TFT=0; shift ;;
        --verify)        VERIFY_ONLY=1; DRY_RUN=1; shift ;;
        --dry-run)       DRY_RUN=1; shift ;;
        -h|--help)       usage ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

# --------------------------------------------------------------------------
hdr "Environment"
[ "$(id -u)" -eq 0 ] && die "run as your normal user, not root (the toolchain is per-user; usage: Scripts/teensy-setup.sh)"
id "$TARGET_USER" >/dev/null 2>&1 || die "no such user: $TARGET_USER"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[ -n "$TARGET_HOME" ] && [ -d "$TARGET_HOME" ] || die "no home dir for $TARGET_USER"

ARDUINO_DATA="$TARGET_HOME/.arduino15"
ARDUINO_USER="$TARGET_HOME/Arduino"
SKETCH_LIB="$ARDUINO_USER/libraries"
TFT_DIR="$SKETCH_LIB/TFT_eSPI"

info "target user : $TARGET_USER ($TARGET_HOME)"
info "repo root   : $REPO_ROOT"
info "core        : teensy:avr@$CORE_VERSION"
info "data dir    : $ARDUINO_DATA"
info "user dir    : $ARDUINO_USER"
[ "$DRY_RUN" -eq 1 ] && c_yel "  DRY RUN — nothing will be changed"
[ "$VERIFY_ONLY" -eq 1 ] && c_yel "  VERIFY MODE — read-only"

have_cli=0
command -v arduino-cli >/dev/null 2>&1 && have_cli=1

# --------------------------------------------------------------------------
# 1. arduino-cli
# --------------------------------------------------------------------------
hdr "arduino-cli"
if [ "$have_cli" -eq 1 ]; then
    ok "found: $(command -v arduino-cli) — $(arduino-cli version 2>/dev/null | head -1)"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
    warn "arduino-cli not on PATH"
elif [ "$DO_CLI" -eq 0 ]; then
    die "arduino-cli not on PATH and --skip-cli was given"
else
    info "installing arduino-cli $CLI_VERSION into ~/bin (as on the source machine)"
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] download + install arduino-cli $CLI_VERSION to $TARGET_HOME/bin"
    else
        mkdir -p "$TARGET_HOME/bin"
        tmp="$(mktemp -d)"
        curl -fsSL -o "$tmp/arduino-cli.tar.gz" \
          "https://github.com/arduino/arduino-cli/releases/download/v${CLI_VERSION}/arduino-cli_${CLI_VERSION}_Linux_64bit.tar.gz" \
          || die "download failed — check network / whether v$CLI_VERSION exists"
        tar -xzf "$tmp/arduino-cli.tar.gz" -C "$tmp" arduino-cli
        install -m 0755 "$tmp/arduino-cli" "$TARGET_HOME/bin/arduino-cli"
        rm -rf "$tmp"
        ok "installed $TARGET_HOME/bin/arduino-cli"
        if ! grep -q 'HOME/bin' "$TARGET_HOME/.bashrc" 2>/dev/null; then
            printf '\nexport PATH="$HOME/bin:$PATH"\n' >> "$TARGET_HOME/.bashrc"
            ok "added ~/bin to PATH in .bashrc"
        fi
        export PATH="$TARGET_HOME/bin:$PATH"
        have_cli=1
    fi
fi

if [ "$have_cli" -eq 1 ] || [ "$DRY_RUN" -eq 1 ]; then
    info "PATH check: ~/bin and ~/.local/bin should both be on PATH"
    case ":$PATH:" in
        *":$TARGET_HOME/bin:"*)    ok "~/bin on PATH" ;;
        *) warn "~/bin NOT on PATH (source ~/.bashrc or re-login)" ;;
    esac
fi

# --------------------------------------------------------------------------
# 2. udev rules
# --------------------------------------------------------------------------
hdr "Teensy udev rules"
UDEV_DST=/etc/udev/rules.d/00-teensy.rules
UDEV_SRC="$REPO_ROOT/Docs/00-teensy.rules"

if [ -f "$UDEV_DST" ]; then
    ok "already installed: $UDEV_DST"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
    warn "$UDEV_DST missing — Teensy uploads will fail"
elif [ "$DO_UDEV" -eq 0 ]; then
    info "skipped (--skip-udev)"
elif [ ! -f "$UDEV_SRC" ]; then
    warn "Docs/00-teensy.rules not found in the repo; get it from pjrc.com"
else
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] sudo install -m 0644 $UDEV_SRC $UDEV_DST && sudo udevadm control --reload"
    else
        sudo install -m 0644 -o root -g root "$UDEV_SRC" "$UDEV_DST"
        sudo udevadm control --reload
        ok "installed udev rules — unplug/replug the Teensys"
    fi
fi

# --------------------------------------------------------------------------
# 3. board manager URL + core
# --------------------------------------------------------------------------
if [ "$have_cli" -eq 1 ]; then
    hdr "Board manager URL (PJRC)"
    if arduino-cli config dump 2>/dev/null | grep -q 'pjrc.com/teensy'; then
        ok "PJRC package index already registered"
    elif [ "$VERIFY_ONLY" -eq 1 ]; then
        warn "PJRC package index not registered"
    else
        run arduino-cli config add board_manager.additional_urls "$PJRC_URL"
        ok "registered $PJRC_URL"
    fi

    hdr "Teensy core"
    installed_core="$(arduino-cli core list 2>/dev/null | awk '/^teensy:avr/ {print $2}')"
    if [ "$installed_core" = "$CORE_VERSION" ]; then
        ok "teensy:avr $CORE_VERSION already installed"
    elif [ "$VERIFY_ONLY" -eq 1 ]; then
        if [ -n "$installed_core" ]; then
            warn "teensy:avr $installed_core installed, expected $CORE_VERSION"
        else
            warn "teensy:avr core not installed"
        fi
    elif [ "$DO_CORE" -eq 0 ]; then
        info "skipped (--skip-core)"
    else
        info "installing teensy:avr@$CORE_VERSION (downloads a large ARM toolchain — be patient)"
        run arduino-cli core update-index
        run arduino-cli core install "teensy:avr@$CORE_VERSION"
    fi
fi

# --------------------------------------------------------------------------
# 4. user libraries
# --------------------------------------------------------------------------
if [ "$have_cli" -eq 1 ]; then
    hdr "User libraries"
    if [ "$DO_LIBS" -eq 0 ]; then
        info "skipped (--skip-libs)"
    elif [ "$VERIFY_ONLY" -eq 1 ]; then
        for spec in "TFT_eSPI:$TFT_VERSION" "MIDI Library:$MIDI_VERSION" "XPT2046_Touchscreen:$TOUCH_VERSION"; do
            name="${spec%:*}"; want="${spec##*:}"
            got="$(lib_version "$name")"
            if [ "$got" = "$want" ]; then ok "$name $want"; else warn "$name: have '${got:-none}', want $want"; fi
        done
    else
        run arduino-cli lib install "TFT_eSPI@$TFT_VERSION"
        run arduino-cli lib install "MIDI Library@$MIDI_VERSION"
        run arduino-cli lib install "XPT2046_Touchscreen@$TOUCH_VERSION"
    fi
fi

# --------------------------------------------------------------------------
# 5. TFT_eSPI User_Setup.h  (the critical manual step)
# --------------------------------------------------------------------------
hdr "TFT_eSPI User_Setup.h"
TFT_SRC="$REPO_ROOT/UIFirmware_Arduino/tft_setup.h"

if [ ! -f "$TFT_SRC" ]; then
    warn "UIFirmware_Arduino/tft_setup.h not found; cannot apply the TFT config"
elif [ ! -d "$TFT_DIR" ]; then
    warn "$TFT_DIR not present (library not installed yet?)"
elif [ "$DO_TFT" -eq 0 ]; then
    info "skipped (--skip-tft)"
elif cmp -s "$TFT_SRC" "$TFT_DIR/User_Setup.h"; then
    ok "User_Setup.h already matches tft_setup.h"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
    warn "User_Setup.h DIFFERS from tft_setup.h — the UI will not drive the display correctly"
else
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] back up and replace $TFT_DIR/User_Setup.h"
    else
        if [ ! -f "$TFT_DIR/User_Setup.h.backup" ]; then
            cp -a "$TFT_DIR/User_Setup.h" "$TFT_DIR/User_Setup.h.backup"
            ok "backed up vendor default to User_Setup.h.backup"
        else
            info "vendor backup already exists (kept)"
        fi
        cp -a "$TFT_SRC" "$TFT_DIR/User_Setup.h"
        ok "applied the Jaiba TFT config"
        if cmp -s "$TFT_SRC" "$TFT_DIR/User_Setup.h"; then
            ok "verified IDENTICAL"
        else
            warn "copy verification failed"
        fi
    fi
fi

# --------------------------------------------------------------------------
# 6. fallback flasher
# --------------------------------------------------------------------------
hdr "teensy_loader_cli (fallback flasher)"
if command -v teensy_loader_cli >/dev/null 2>&1; then
    ok "found: $(command -v teensy_loader_cli)"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
    warn "teensy_loader_cli not installed (optional fallback)"
else
    info "install with:  sudo apt install teensy-loader-cli"
fi

# --------------------------------------------------------------------------
# 7. verification
# --------------------------------------------------------------------------
hdr "Verification"
PROBLEMS=0
if [ "$have_cli" -eq 1 ]; then
    printf '  %-26s %s\n' "arduino-cli" "$(arduino-cli version 2>/dev/null | sed 's/.*Version: \([^ ]*\).*/\1/')"
    printf '  %-26s %s\n' "core" "$(arduino-cli core list 2>/dev/null | awk '/^teensy:avr/ {print $2}')"
    printf '  %-26s %s\n' "tft_eSPI" "$(lib_version TFT_eSPI)"
    printf '  %-26s %s\n' "midi library" "$(lib_version 'MIDI Library')"
    printf '  %-26s %s\n' "touch library" "$(lib_version XPT2046_Touchscreen)"
fi

if [ -f "$UDEV_DST" ]; then printf '  %-26s %s\n' "udev rules" "present"; else c_red "  udev rules                 MISSING"; PROBLEMS=$((PROBLEMS+1)); fi

if [ -f "$TFT_DIR/User_Setup.h" ] && [ -f "$TFT_SRC" ]; then
    if cmp -s "$TFT_SRC" "$TFT_DIR/User_Setup.h"; then
        printf '  %-26s %s\n' "TFT config" "matches tft_setup.h"
    else
        c_red "  TFT config                 DIFFERS from tft_setup.h"; PROBLEMS=$((PROBLEMS+1))
    fi
fi

# compiled sketches present?
for d in "$REPO_ROOT/UIFirmware_Arduino" "$REPO_ROOT/SensorFirmware_Arduino"; do
    [ -d "$d" ] || c_red "  sketch missing             $d"
done

echo
if [ "$PROBLEMS" -gt 0 ]; then
    c_red "$PROBLEMS verification problem(s)."
    exit 2
fi
c_grn "Toolchain looks good."
[ "$WARNINGS" -gt 0 ] && c_yel "$WARNINGS warning(s) — see above"

if [ "$VERIFY_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    cat <<'EOF'

Next steps:
  1. source ~/.bashrc          (pick up ~/bin on PATH)
  2. Unplug/replug the Teensys (pick up the udev rules)
  3. Build both sketches to prove the toolchain:
       arduino-cli compile --fqbn teensy:avr:teensy41 UIFirmware_Arduino
       arduino-cli compile --fqbn teensy:avr:teensy41:usb=serialmidi SensorFirmware_Arduino
  4. Flash with the safe helper (it enforces ONE board at a time):
       Scripts/teensy-flash.sh --list
       Scripts/teensy-flash.sh ui
       Scripts/teensy-flash.sh sensor
EOF
fi
exit 0
