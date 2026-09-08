# Jaiba Audio Workstation — Sound Card & Settings

Machine: `jaiba-sampler` (Ubuntu 24.04.4 LTS, low-latency kernel). This is the
authoritative reference for the audio interface configuration used by the
Jaiba Sampler (and anything else that plays audio here).

Last validated: 2026-09 (sampler on the Delta at 128/48k — "sounds perfect").

---

## Sound card: M-Audio Delta 1010LT (PCI)

- Chipset: **ICE1712** (kernel driver `ice1712`, built into ALSA)
- 8 analog inputs + 8 analog outputs (10x10 counting S/PDIF)
- **S/PDIF** digital I/O (2 ch)
- **24-bit / up to 96 kHz**
- Hardware mixer: `envy24control` (alsa-tools-gui)
- No MIDI on this card (drum MIDI comes from the Teensys via USB)

### Card identity — use the NAME, not the index
ALSA card indices shift when devices are added/removed. Current order
(2026-09):

| Index | Card | Purpose |
|---|---|---|
| 0 | `PCH` (HDA Intel PCH / ALC662 onboard) | motherboard — avoid for audio work |
| 1 | `MIDI` (Teensy MIDI, USB) | drum USB-MIDI input |
| 2 | `M1010LT` (Delta 1010LT) | **audio output** |

So always address the Delta as **`hw:CARD=M1010LT,DEV=0`** (or
`plughw:CARD=M1010LT,DEV=0`) — never `hw:0,0` (that is now the onboard PCH).

---

## ALSA routing — `~/.asoundrc`

`pcm.!default` → Delta by name @ 48 kHz; `ctl` → Delta.

```conf
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
```

Backup of the previous (outdated `hw:0,0` → onboard) file:
`~/.asoundrc.bak-dsh`.

> History: the old file hardcoded `hw:0,0`, which was the Delta when it was
> card 0 — after the Teensy USB card appeared the Delta moved to index 2 and
> `default` silently started routing to the motherboard. Name-based routing
> fixes that permanently.

---

## Sampler audio settings (validated)

Stored in `~/JaibaSampler/JaibaSampler.settings` (`audioDeviceStateXml`):

| Setting | Value | Note |
|---|---|---|
| Device | ALSA `default` | → Delta via `.asoundrc` |
| Sample rate | **48000 Hz** | matches Delta; no resampling |
| Buffer | **128 samples** | ≈ 2.7 ms |

Measured feel: drums trigger cleanly, no audible latency ("sounds perfect").
Fallbacks if underruns ever appear: 256 samples ≈ 5.3 ms (still a big win
over the old 512 ≈ 10.7–11.6 ms). 64 is available experimentally.

Audio menu in the app: Settings → Audio → Buffer Size (64–512).

---

## System tuning (already applied, keep in mind)

- Low-latency kernel `6.8.0-138-lowlatency` (default boot)
- CPU governor `performance` (permanent)
- `vm.swappiness=10`
- Realtime: `@audio - rtprio 95`, `@audio - memlock unlimited`; user in
  `audio` group
- `threadirqs` on the kernel cmdline; Delta IRQ 18 → FIFO priority 80
  (systemd `set-delta-irq-priority.service`, enabled)
- **PipeWire / PulseAudio disabled & masked** (avoided "Device or resource
  busy" conflicts) — ALSA-only audio
- cyclictest: avg 2 µs, max 265 µs (excellent)

---

## Reference commands

```bash
# List cards / verify Delta present
cat /proc/asound/cards

# Play a tone through the Delta directly (working device string)
speaker-test -c 2 -r 48000 -t wav -D plughw:CARD=M1010LT,DEV=0

# Open Delta at small buffer (routing check; shell underruns are normal here)
aplay -D default -c 2 -r 48000 -f S16_LE --buffer-size=128 --period-size=64 -d 1 /dev/zero

# Hardware mixer
envy24control

# Delta IRQ priority status
ps -eo pid,comm,rtprio,policy | grep "irq/18"

# Who is holding audio devices?
fuser -v /dev/snd/*

# Check the sampler's own restored audio config on launch
# (run from a terminal; prints "[AUDIO] Device state restored — buf / SR")
```

## Full latency chain (pad → ears)

Sensor Teensy (~2 ms peak window) → USB-MIDI → sampler (midi → audio block,
sample-timestamped) → **Delta 128 @ 48 kHz (~2.7 ms block)** → outputs.
Target: sampler contribution ~3–5 ms, verified with the app's per-note
measured MIDI→audio latency printout.
