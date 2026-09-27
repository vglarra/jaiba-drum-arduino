#!/bin/bash
#
# audio-workstation-setup.sh
#
# Reproduce the Jaiba audio workstation tuning on a new Ubuntu machine.
# Companion to Docs/AUDIO_MIGRATION_REPORT.md — read that first.
#
# Idempotent: safe to re-run. Never overwrites /etc/security/limits.conf or
# /etc/sysctl.conf wholesale; it appends only what is missing.
#
# Usage:
#   sudo Scripts/audio-workstation-setup.sh [options]
#
# Options:
#   --user NAME             target desktop user (default: $SUDO_USER)
#   --audio-server MODE     alsa | pulse | keep   (default: keep)
#                             alsa  = mask PipeWire AND PulseAudio (ALSA-only,
#                                     lowest jitter; kills system/browser sound)
#                             pulse = keep PulseAudio (pin it off the Delta —
#                                     do that manually, see the report)
#                             keep  = touch nothing, just report current state
#   --priority N            IRQ thread FIFO priority (default: 80)
#   --verify                check-only, change nothing (implies --dry-run)
#   --dry-run               print actions without applying them
#   --skip-packages         do not apt-get install
#   -h, --help              this text
#
# Exit codes: 0 ok, 1 usage/env error, 2 verification found problems
#
set -euo pipefail

# --------------------------------------------------------------------------
# defaults
# --------------------------------------------------------------------------
TARGET_USER="${SUDO_USER:-$(id -un)}"
AUDIO_SERVER="keep"
IRQ_PRIO="80"
DRY_RUN=0
VERIFY_ONLY=0
SKIP_PACKAGES=0

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

PACKAGES=(
    linux-lowlatency
    linux-headers-lowlatency
    alsa-utils
    alsa-tools-gui
    cpufrequtils
    rt-tests
    rtkit
)

ASOUNDRC_CONTENT='# ALSA routing for Jaiba audio workstation (Delta 1010LT).
# Card-name based so it survives index changes (PCH=0, Teensy=1, Delta=2 now).
pcm.!default {
    type plug
    slave {
        pcm "hw:CARD=M1010LT,DEV=0"
        rate 48000
    }
}

ctl.!default {
    type hw
    card M1010LT
}
'

# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
c_red()  { printf '\033[31m%s\033[0m\n' "$*"; }
c_grn()  { printf '\033[32m%s\033[0m\n' "$*"; }
c_yel()  { printf '\033[33m%s\033[0m\n' "$*"; }
c_dim()  { printf '\033[2m%s\033[0m\n' "$*"; }
hdr()    { printf '\n\033[1m== %s\033[0m\n' "$*"; }

WARNINGS=0
warn() { c_yel "  ! $*"; WARNINGS=$((WARNINGS + 1)); }
ok()   { c_grn "  + $*"; }
info() { printf '  - %s\n' "$*"; }

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] $*"
    else
        "$@"
    fi
}

# write a file only if its content differs
run_write() {
    local path="$1" content="$2" mode="${3:-0644}" owner="${4:-root:root}"
    if [ -f "$path" ] && [ "$(cat "$path")" = "$content" ]; then
        info "unchanged: $path"
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] write $path (mode $mode, owner $owner)"
        return 0
    fi
    printf '%s' "$content" > "$path"
    chmod "$mode" "$path"
    chown "$owner" "$path"
    ok "wrote $path"
}

die() { c_red "ERROR: $*" >&2; exit 1; }

usage() { sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

# --------------------------------------------------------------------------
# argument parsing
# --------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --user)         TARGET_USER="${2:?}"; shift 2 ;;
        --audio-server) AUDIO_SERVER="${2:?}"; shift 2 ;;
        --priority)     IRQ_PRIO="${2:?}"; shift 2 ;;
        --verify)       VERIFY_ONLY=1; DRY_RUN=1; shift ;;
        --dry-run)      DRY_RUN=1; shift ;;
        --skip-packages) SKIP_PACKAGES=1; shift ;;
        -h|--help)      usage ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case "$AUDIO_SERVER" in
    alsa|pulse|keep) ;;
    *) die "--audio-server must be alsa, pulse or keep" ;;
esac

# --------------------------------------------------------------------------
# environment checks
# --------------------------------------------------------------------------
hdr "Environment"

if [ "$VERIFY_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    die "must run as root for changes: sudo $0 $*"
fi

# Running as root without sudo means we cannot infer the desktop user.
if [ "$TARGET_USER" = "root" ] && [ "$VERIFY_ONLY" -eq 0 ]; then
    die "cannot infer the target user when running as root directly; pass --user NAME"
fi

[ -n "$TARGET_USER" ] || die "cannot determine target user; pass --user NAME"
id "$TARGET_USER" >/dev/null 2>&1 || die "no such user: $TARGET_USER"
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
[ -n "$TARGET_HOME" ] && [ -d "$TARGET_HOME" ] || die "no home dir for $TARGET_USER"

info "target user : $TARGET_USER ($TARGET_HOME)"
info "repo root   : $REPO_ROOT"
info "audio server: $AUDIO_SERVER"
info "IRQ priority: $IRQ_PRIO"
[ "$DRY_RUN" -eq 1 ] && c_yel "  DRY RUN — nothing will be changed"

if [ "$VERIFY_ONLY" -eq 0 ]; then
    # hardware sanity: is a Delta actually present (or at least a PCI slot)?
    if lspci -nn 2>/dev/null | grep -q '1412:1712'; then
        ok "Delta 1010LT detected on the PCI bus"
    else
        warn "no ICE1712 (1412:1712) found — card not installed yet, or in a slot the kernel can't see"
    fi
fi

# --------------------------------------------------------------------------
# 1. packages
# --------------------------------------------------------------------------
if [ "$VERIFY_ONLY" -eq 0 ] && [ "$SKIP_PACKAGES" -eq 0 ]; then
    hdr "Packages"
    missing=()
    for p in "${PACKAGES[@]}"; do
        if dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q 'install ok installed'; then
            info "installed: $p"
        else
            missing+=("$p")
        fi
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        info "installing: ${missing[*]}"
        run apt-get update -qq
        run apt-get install -y "${missing[@]}"
    else
        ok "all packages present"
    fi
fi

# --------------------------------------------------------------------------
# 2. kernel cmdline: threadirqs
# --------------------------------------------------------------------------
if [ "$VERIFY_ONLY" -eq 0 ]; then
    hdr "Kernel command line (threadirqs)"
    if grep -qE '^GRUB_CMDLINE_LINUX_DEFAULT=.*threadirqs' /etc/default/grub 2>/dev/null; then
        ok "threadirqs already in /etc/default/grub"
    else
        if [ "$DRY_RUN" -eq 1 ]; then
            c_dim "    [dry-run] add threadirqs to GRUB_CMDLINE_LINUX_DEFAULT"
        else
            cp -a /etc/default/grub "/etc/default/grub.bak.$(date +%s)"
            if grep -qE '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
                sed -i -E 's/^(GRUB_CMDLINE_LINUX_DEFAULT=")([^"]*)(")/\1\2 threadirqs\3/' /etc/default/grub
            else
                echo 'GRUB_CMDLINE_LINUX_DEFAULT="quiet splash threadirqs"' >> /etc/default/grub
            fi
            ok "added threadirqs to /etc/default/grub"
        fi
        run update-grub
    fi

    # GRUB_DEFAULT cannot be portably automated: entry ORDER is machine-specific.
    hdr "Kernel selection (manual step)"
    if grep -qE '^GRUB_DEFAULT=saved' /etc/default/grub 2>/dev/null; then
        ok "GRUB_DEFAULT=saved (name-based) — good"
    else
        warn "GRUB_DEFAULT is positional: $(grep -E '^GRUB_DEFAULT=' /etc/default/grub 2>/dev/null || echo 'unset')"
        cat <<'EOF'
      A positional GRUB_DEFAULT (e.g. "1>2") points at a menu slot, not a
      kernel. After installing kernels the slot may move and you will silently
      boot the generic kernel instead of -lowlatency.

      Recommended (survives kernel updates and machine changes):

        # in /etc/default/grub:
        GRUB_DEFAULT=saved
        GRUB_SAVEDEFAULT=true

        sudo update-grub
        sudo grub-set-default "Advanced options for Ubuntu>Ubuntu, with Linux <VER>-lowlatency"

      List the exact entry names with:

        grep -E "^\s*(menuentry|submenu)" /boot/grub/grub.cfg
        # or, if grub.cfg is root-only:  sudo grep -E "^\s*(menuentry|submenu)" /boot/grub/grub.cfg
EOF
    fi
else
    hdr "Kernel command line"
    if grep -q 'threadirqs' /proc/cmdline; then ok "threadirqs active"; else warn "threadirqs NOT active in /proc/cmdline"; fi
fi

# --------------------------------------------------------------------------
# 3. CPU governor
# --------------------------------------------------------------------------
if [ "$VERIFY_ONLY" -eq 0 ]; then
    hdr "CPU governor"
    run_write /etc/default/cpufrequtils 'GOVERNOR="performance"' 0644 root:root
    if [ "$DRY_RUN" -eq 0 ]; then
        systemctl enable cpufrequtils >/dev/null 2>&1 || true
        systemctl restart cpufrequtils >/dev/null 2>&1 || true
    fi
fi

# --------------------------------------------------------------------------
# 4. sysctl swappiness
# --------------------------------------------------------------------------
hdr "vm.swappiness"
if grep -qE '^\s*vm\.swappiness\s*=\s*10\s*$' /etc/sysctl.conf 2>/dev/null; then
    ok "already set in /etc/sysctl.conf"
else
    if [ "$VERIFY_ONLY" -eq 0 ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            c_dim "    [dry-run] append vm.swappiness=10 to /etc/sysctl.conf"
        else
            printf '\n# Jaiba audio workstation: avoid swap-induced dropouts\nvm.swappiness=10\n' >> /etc/sysctl.conf
            sysctl -w vm.swappiness=10 >/dev/null
            ok "appended vm.swappiness=10"
        fi
    else
        warn "vm.swappiness not set to 10"
    fi
fi

# --------------------------------------------------------------------------
# 5. realtime limits
# --------------------------------------------------------------------------
hdr "Realtime limits (@audio)"
for spec in '@audio - rtprio 95' '@audio - memlock unlimited'; do
    if grep -qF "$spec" /etc/security/limits.conf 2>/dev/null; then
        ok "present: $spec"
    elif [ "$VERIFY_ONLY" -eq 1 ]; then
        warn "missing: $spec"
    else
        if [ "$DRY_RUN" -eq 1 ]; then
            c_dim "    [dry-run] append '$spec' to /etc/security/limits.conf"
        else
            printf '%s\n' "$spec" >> /etc/security/limits.conf
            ok "appended: $spec"
        fi
    fi
done

hdr "Group membership"
if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx audio; then
    ok "$TARGET_USER already in 'audio'"
elif [ "$VERIFY_ONLY" -eq 1 ]; then
    warn "$TARGET_USER not in 'audio'"
else
    run usermod -aG audio "$TARGET_USER"
    ok "added $TARGET_USER to 'audio' (re-login required)"
fi

# --------------------------------------------------------------------------
# 6. ALSA routing
# --------------------------------------------------------------------------
hdr "ALSA routing (~/.asoundrc)"
ASOUNDRC="$TARGET_HOME/.asoundrc"
if [ -f "$ASOUNDRC" ] && grep -q 'CARD=M1010LT' "$ASOUNDRC"; then
    ok "name-based .asoundrc already in place"
else
    if [ -f "$ASOUNDRC" ]; then
        warn "existing .asoundrc does not reference CARD=M1010LT"
        info "current device line: $(grep -m1 'pcm ' "$ASOUNDRC" 2>/dev/null || echo '?')"
        if [ "$VERIFY_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
            cp -a "$ASOUNDRC" "$ASOUNDRC.bak.$(date +%s)"
            info "backed up the old file alongside"
        fi
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] write $ASOUNDRC"
    else
        printf '%s' "$ASOUNDRC_CONTENT" > "$ASOUNDRC"
        chmod 0664 "$ASOUNDRC"
        chown "$TARGET_USER:$TARGET_USER" "$ASOUNDRC"
        ok "wrote $ASOUNDRC"
    fi
fi

# --------------------------------------------------------------------------
# 7. IRQ priority service (corrected)
# --------------------------------------------------------------------------
hdr "Delta IRQ priority service"
IRQ_HELPER=/usr/local/sbin/set-delta-irq-priority.sh
IRQ_UNIT=/etc/systemd/system/set-delta-irq-priority.service

if [ "$VERIFY_ONLY" -eq 0 ]; then
    SRC_SH="$REPO_ROOT/Scripts/set-delta-irq-priority.sh"
    SRC_UNIT="$REPO_ROOT/Scripts/set-delta-irq-priority.service"
    if [ -f "$SRC_SH" ] && [ -f "$SRC_UNIT" ]; then
        run install -m 0755 -o root -g root "$SRC_SH" "$IRQ_HELPER"
        run install -m 0644 -o root -g root "$SRC_UNIT" "$IRQ_UNIT"
    else
        warn "Scripts/set-delta-irq-priority.{sh,service} not found next to repo root; skipping"
    fi
    if [ "$DRY_RUN" -eq 0 ] && [ -f "$IRQ_HELPER" ]; then
        systemctl daemon-reload
        systemctl enable set-delta-irq-priority.service >/dev/null 2>&1 || true
        systemctl restart set-delta-irq-priority.service || warn "service failed on first run (card may not be probed yet — it will retry at boot)"
    fi
else
    info "verify mode: checking live IRQ thread priority"
fi

# --------------------------------------------------------------------------
# 8. Teensy udev rules
# --------------------------------------------------------------------------
if [ "$VERIFY_ONLY" -eq 0 ]; then
    hdr "Teensy udev rules"
    UDEV_SRC="$REPO_ROOT/Docs/00-teensy.rules"
    UDEV_DST=/etc/udev/rules.d/00-teensy.rules
    if [ -f "$UDEV_DST" ]; then
        ok "already installed"
    elif [ -f "$UDEV_SRC" ]; then
        run install -m 0644 -o root -g root "$UDEV_SRC" "$UDEV_DST"
        run udevadm control --reload
        info "unplug/replug the Teensys for the rules to take effect"
    else
        warn "Docs/00-teensy.rules not found; install manually from pjrc.com"
    fi
fi

# --------------------------------------------------------------------------
# 9. audio server state
# --------------------------------------------------------------------------
hdr "Audio server (PipeWire / PulseAudio)"
USER_UNIT_DIR="$TARGET_HOME/.config/systemd/user"

mask_user_unit() {
    local unit="$1"
    if [ "$DRY_RUN" -eq 1 ]; then
        c_dim "    [dry-run] mask $unit for $TARGET_USER"
        return 0
    fi
    install -d -m 0755 -o "$TARGET_USER" -g "$TARGET_USER" "$USER_UNIT_DIR"
    ln -sfn /dev/null "$USER_UNIT_DIR/$unit"
    ok "masked $unit"
}

case "$AUDIO_SERVER" in
    keep)
        info "no change requested (--audio-server keep)"
        ;;
    alsa)
        c_yel "  masking PipeWire and PulseAudio — system/browser sound will stop working"
        for u in pipewire.service pipewire-pulse.service wireplumber.service pulseaudio.service pulseaudio.socket; do
            mask_user_unit "$u"
        done
        if [ "$DRY_RUN" -eq 0 ]; then
            run sudo -u "$TARGET_USER" XDG_RUNTIME_DIR="/run/user/$(id -u "$TARGET_USER")" \
                systemctl --user stop pulseaudio.service pulseaudio.socket 2>/dev/null || true
            systemctl --global disable pulseaudio.service pulseaudio.socket >/dev/null 2>&1 || true
        fi
        info "log out/in for masking to take full effect"
        ;;
    pulse)
        info "keeping PulseAudio"
        c_yel "  ACTION REQUIRED: point PulseAudio's default sink at the ONBOARD card,"
        c_yel "  not the Delta, so it cannot steal the sampler's device."
        cat <<'EOF'
      Easiest: install pavucontrol, open it, and on the Output Devices tab click
      the green tick on the onboard "HDA Intel PCH" sink. Or pin it:

        mkdir -p ~/.config/pulse
        printf 'set-default-sink alsa_output.pci-0000_00_1b.0.analog-stereo\n' \
            > ~/.config/pulse/default.pa.d/50-jaiba.pa   # check the exact name:
        pactl list short sinks
EOF
        ;;
esac

# --------------------------------------------------------------------------
# 10. verification
# --------------------------------------------------------------------------
hdr "Verification"
PROBLEMS=0
check() { # check "label" "expected-ish" command...
    local label="$1"; shift
    local out
    out="$("$@" 2>&1 | head -1 || true)"
    printf '  %-28s %s\n' "$label" "$out"
}

printf '  %-28s %s\n' "kernel"        "$(uname -r)"
printf '  %-28s %s\n' "cmdline"       "$(grep -o threadirqs /proc/cmdline || echo 'MISSING threadirqs')"
printf '  %-28s %s\n' "governor cpu0" "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
printf '  %-28s %s\n' "vm.swappiness" "$(sysctl -n vm.swappiness 2>/dev/null)"
printf '  %-28s %s\n' "asoundrc"      "$(grep -c M1010LT "$ASOUNDRC" 2>/dev/null || echo 0) ref(s) to M1010LT"

if grep -q 'snd_ice1712' /proc/asound/cards 2>/dev/null || grep -rqs 'M1010LT' /proc/asound/cards; then
    printf '  %-28s %s\n' "card" "$(grep -m1 M1010LT /proc/asound/cards | tr -s ' ')"
else
    c_red "  card                          M1010LT NOT PRESENT"; PROBLEMS=$((PROBLEMS+1))
fi

# IRQ thread priority — the thing that was silently broken
irq_state="none"
for comm in /proc/[0-9]*/comm; do
    [ -r "$comm" ] || continue
    read -r n < "$comm" 2>/dev/null || continue
    case "$n" in
        *-snd_ice1712)
            pid="${comm#/proc/}"; pid="${pid%/comm}"
            pol="$(chrt -p "$pid" 2>/dev/null | sed -n 's/.*policy: \([A-Z_]*\).*/\1/p')"
            pri="$(chrt -p "$pid" 2>/dev/null | sed -n 's/.*priority: \([0-9]*\).*/\1/p')"
            irq_state="$n -> $pol/$pri"
            if [ "$pol" = "SCHED_FIFO" ] && [ "${pri:-0}" -ge 80 ] 2>/dev/null; then
                c_grn "  IRQ priority OK              $irq_state"
            else
                c_yel "  IRQ priority LOW             $irq_state (expected SCHED_FIFO/80+)"
                PROBLEMS=$((PROBLEMS+1))
            fi
            ;;
    esac
done
[ "$irq_state" = "none" ] && { c_yel "  IRQ thread                   not found (*-snd_ice1712)"; PROBLEMS=$((PROBLEMS+1)); }

echo
if [ "$WARNINGS" -gt 0 ]; then c_yel "$WARNINGS warning(s) — see above"; fi
if [ "$PROBLEMS" -gt 0 ]; then
    c_red "$PROBLEMS verification problem(s)."
    [ "$VERIFY_ONLY" -eq 0 ] && info "a reboot is likely required before re-running --verify"
    exit 2
fi

c_grn "All checks passed."
if [ "$VERIFY_ONLY" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    cat <<'EOF'

Next manual steps:
  1. Resolve GRUB_DEFAULT (name-based) as printed above, if it was positional.
  2. Reboot, then re-run:  sudo Scripts/audio-workstation-setup.sh --verify
  3. Confirm sound on the Delta:
       speaker-test -c 2 -r 48000 -t wav -D plughw:CARD=M1010LT,DEV=0
  4. Copy ~/JaibaSampler/JaibaSampler.settings and re-check the MIDI device name.
  5. Re-measure:  sudo cyclictest -m -p80 -n -i 200 -h 400 -l 100000
EOF
fi
exit 0
