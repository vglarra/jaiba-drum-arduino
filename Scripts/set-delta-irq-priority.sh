#!/bin/bash
#
# set-delta-irq-priority.sh — boost the M-Audio Delta 1010LT (ICE1712) IRQ
# thread to SCHED_FIFO at a given priority.
#
# Why this exists: with the `threadirqs` kernel parameter, IRQ handlers run as
# schedulable kernel threads and default to SCHED_FIFO/50. Giving the audio
# card's IRQ thread a higher priority keeps DMA refills ahead of everything
# else on the box.
#
# Unlike the original one-liner, this:
#   * does NOT hardcode an IRQ number (IRQs change with slot/mainboard/ACPI),
#   * does NOT use `pgrep -f`, whose pattern matches the shell running it,
#   * retries while the sound card is still being probed at boot.
#
# Usage: set-delta-irq-priority.sh [priority]     (default 80)
#
set -u

PRIO="${1:-80}"
DRIVER="${DELTA_DRIVER:-snd_ice1712}"
TIMEOUT_TENTHS="${TIMEOUT_TENTHS:-200}"   # 200 * 0.1s = 20s

log() { logger -t jaiba-irq "$*" 2>/dev/null; echo "jaiba-irq: $*"; }

boosted=0

for _ in $(seq 1 "$TIMEOUT_TENTHS"); do
    # Walk /proc looking for the IRQ thread owned by our driver. The kernel
    # names them "<irq>-<driver>", e.g. "irq/18-snd_ice1712". Reading comm
    # (not cmdline, not ps) avoids both truncation and self-matching.
    for comm in /proc/[0-9]*/comm; do
        [ -r "$comm" ] || continue
        read -r name < "$comm" 2>/dev/null || continue
        case "$name" in
            *-"$DRIVER")
                pid="${comm#/proc/}"
                pid="${pid%/comm}"
                if chrt -f -p "$PRIO" "$pid" 2>/dev/null; then
                    log "set $name (pid $pid) -> SCHED_FIFO/$PRIO"
                    boosted=1
                fi
                ;;
        esac
    done

    [ "$boosted" -eq 1 ] && break
    sleep 0.1
done

if [ "$boosted" -ne 1 ]; then
    log "ERROR: no IRQ thread matching *-$DRIVER found after ${TIMEOUT_TENTHS} tries"
    exit 1
fi

exit 0
