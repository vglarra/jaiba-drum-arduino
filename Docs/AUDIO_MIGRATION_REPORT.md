# Jaiba Audio Workstation — Migration Report

**Goal:** move the complete Ubuntu audio tuning + sound-card setup from the current
machine to a new PC.
**Source machine:** `jaiba-sampler` (Ubuntu 24.04.4 LTS, kernel
`6.8.0-138-lowlatency`, M-Audio Delta 1010LT, JINGSHA B75-HM PLUS mainboard).
**Audited:** live system on 2026-09-27, cross-checked against
`Docs/AUDIO_WORKSTATION.md`.
**Method:** read-only inspection of the running system (`/proc/asound`,
`lsmod`, systemd unit state, `dpkg`, `/etc`, `~`) — nothing was modified.

---

## 1. Executive summary

The good news: **there is no driver to port.** The Delta 1010LT is driven by
`snd_ice1712`, which is an in-tree ALSA module shipped inside every Ubuntu
kernel. No DKMS, no proprietary blob, no compilation. "Transferring the sound
card driver" reduces to *installing a kernel that has the module* (all of them
do) and copying one 14-line ALSA config file.

The real work is entirely in **system tuning and device identity**, and there
are three things that will bite you on new hardware:

| # | Risk | Severity | Why |
|---|---|---|---|
| 1 | **Delta 1010LT is a PCI (not PCIe) card** | 🔴 Blocking | Modern mainboards usually have no legacy PCI slot. Must be verified *before* buying/building the new PC. |
| 2 | **`GRUB_DEFAULT="1>2"` is a positional boot entry** | 🟠 High | Menu positions are machine-specific. On the new PC it will silently boot the *generic* kernel, not lowlatency. |
| 3 | **IRQ-priority service is silently broken and hardcodes IRQ 18** | 🟠 High | Live thread is at `SCHED_FIFO/50`, not the documented 80. The `pgrep` pattern self-matches. IRQ numbers always change on new hardware. |

Additional finding: the live system **does not match** `AUDIO_WORKSTATION.md` on
two points — PulseAudio is running (doc says masked), and the ALSA card index
order has changed. See §7.

A ready-to-run installer that applies everything correctly is provided at
`Scripts/audio-workstation-setup.sh`; the corrected IRQ unit is at
`Scripts/set-delta-irq-priority.service`.

---

## 2. The setup as layers (what actually has to move)

### Layer A — Sound card & driver

| Item | Value |
|---|---|
| Card | M-Audio Delta 1010LT, PCI |
| PCI ID | `1412:1712` (VIA ICE1712 Envy24), subsystem `1412:d63b` |
| Kernel driver | `snd_ice1712` + `snd_ice17xx_ak4xxx`, `snd_cs8427`, `snd_i2c`, `snd_ak4xxx_adda`, `snd_ac97_codec`, `snd_mpu401_uart` |
| Source | In-tree, `/lib/modules/<kver>/kernel/sound/pci/ice1712/snd-ice1712.ko.zst` |
| Module options | **All defaults.** `index=-1` (auto), `enable=Y`, `omni=N`, `dxr_enable=0` |
| Firmware | None required |
| Mixer front-end | `envy24control` (package `alsa-tools-gui`) |

**Transfer action:** none beyond ensuring the kernel is installed. There is no
`/etc/modprobe.d/` entry specific to this card — `alsa-base.conf` is the Ubuntu
stock file.

> ⚠️ **PCI slot check.** `lspci` confirms `04:01.0 ... PCI Multi-Channel I/O
> Controller`. This is conventional PCI, *not* PCI Express. Verify the new
> mainboard has a physical PCI slot. If not, the options are a PCIe→PCI bridge
> riser (works often but adds latency/jitter risk and is not guaranteed with
> ICE1712 DMA), or replacing the interface with a USB/PCIe class-compliant one
> — which would invalidate the `.asoundrc` device string but nothing else.

### Layer B — Kernel

| Item | Value | Portable? |
|---|---|---|
| Flavour | `linux-lowlatency` 6.8.0-138 (`6.8.0-138.138.1`) | ✅ package |
| Running kernel | `6.8.0-138-lowlatency` | ✅ |
| Also installed (unused) | `7.0.0-30-generic`, `7.0.0-31-generic` (HWE) | — |
| Boot selection | `GRUB_DEFAULT="1>2"` | 🔴 **must re-derive** |
| Kernel cmdline | `quiet splash threadirqs` | ✅ but re-apply |
| Other cmdline | `vt.handoff=7` (Ubuntu stock) | auto |

`threadirqs` is the load-bearing parameter: it turns IRQ handlers into
schedulable threads so the Delta's IRQ can be given real-time priority.

**Transfer notes:**
- Install `linux-lowlatency`. On **newer hardware** the 6.8 GA kernel may not
  drive the chipset/NVMe/USB at all — consider `linux-lowlatency-hwe-24.04`
  (newer base, still `-lowlatency`). Do not fall back to `-generic`: you lose
  the PREEMPT tuning that the whole latency budget depends on.
- `GRUB_DEFAULT="1>2"` means *submenu 1, entry 2* of the generated GRUB menu.
  Adding/removing a kernel changes this. **Recommended replacement:**
  ```bash
  # /etc/default/grub
  GRUB_DEFAULT=saved
  GRUB_SAVEDEFAULT=true
  # then:
  sudo update-grub
  sudo grub-set-default "Advanced options for Ubuntu>Ubuntu, with Linux 6.8.0-138-lowlatency"
  ```
  This is name-based and therefore survives kernel updates and machine changes.
- Verify after reboot: `uname -r` must end in `-lowlatency`, and
  `cat /proc/cmdline` must contain `threadirqs`.

### Layer C — ALSA routing

`~/.asoundrc` (289 bytes) — **card-name based**, which is exactly why it is
portable across index reshuffles:

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

`/etc/asound.conf` does **not** exist (intentional — routing is per-user).

**Transfer action:** copy the file to `~/.asoundrc` on the new machine as the
same user. Do **not** copy `~/.asoundrc.backup` or `~/.asoundrc.bak-dsh`: both
contain the obsolete `hw:0,0` / `card 0` form that caused the original
"silently routed to the motherboard" bug.

> The `plug` + `rate 48000` wrapper means any app asking for 44.1 kHz gets
> resampled transparently. This costs a little CPU but keeps the Delta locked
> at 48 kHz as the app expects.

### Layer D — Realtime & scheduling

| Item | Live value | Where |
|---|---|---|
| `@audio` rtprio | `95` | `/etc/security/limits.conf` (**edited** stock file) |
| `@audio` memlock | `unlimited` | `/etc/security/limits.conf` (**edited**) |
| User membership | `jaiba` ∈ `audio` (also `dialout`, `plugdev`) | `/etc/group` |
| IRQ threading | global `threadirqs` | kernel cmdline |
| Delta IRQ priority | **currently FIFO 50, not 80** ❌ | `set-delta-irq-priority.service` |
| `rtkit-daemon` | installed, `disabled` at boot but **active** (D-Bus activated) | systemd |
| `rt-tests` | installed (`cyclictest` available) | package |

**Transfer action:** append the two `@audio` lines to
`/etc/security/limits.conf`, add the user to `audio`, install `cpufrequtils`,
`rt-tests`, and install the **corrected** IRQ unit (see §8).

> `/etc/security/limits.conf` is a package conffile, so replacing the whole
> file is wrong — append, don't overwrite. The script does this idempotently.

### Layer E — Power & VM tuning

| Item | Value | Where |
|---|---|---|
| CPU governor | `performance` on all 4 cores | `/etc/default/cpufrequtils` → `GOVERNOR="performance"`, service `cpufrequtils` (sysv) enabled |
| `vm.swappiness` | `10` | `/etc/sysctl.conf` line 65 (**edited** stock file, verified via `dpkg -V`) |
| Swap | 8 GiB `/swap.img` | default |

**Transfer action:** install `cpufrequtils`, set `GOVERNOR="performance"`, and
append `vm.swappiness=10` to `/etc/sysctl.conf`.

> `vm.swappiness=10` matters more than it looks: swapping during a live
> performance is an audible dropout. On the new machine with more/faster RAM
> this is still worth keeping. `performance` governor prevents the CPU from
> dropping to 1.6 GHz mid-buffer.

### Layer F — Audio server (PipeWire / PulseAudio)

This is the **least consistent layer** on the source machine.

| Component | `is-enabled` | `is-active` |
|---|---|---|
| `pipewire` (user) | **masked** (`~/.config/systemd/user/pipewire.service` → `/dev/null`) | inactive |
| `pipewire-pulse` (user) | **masked** | inactive |
| `wireplumber` (user) | enabled | inactive |
| `pulseaudio` (user) | **enabled** | **ACTIVE** |

PulseAudio is running with `nice = -11` (not RT) and **has the Delta as its
default sink** (`alsa_output.pci-0000_04_01.0.analog-stereo`, currently
`SUSPENDED`). It holds `/dev/snd/controlC0` and `controlC1` open.

The doc claims *"PipeWire / PulseAudio disabled & masked"*. Only PipeWire is
masked; PulseAudio was re-enabled at some point and is live. See §7.

**Why this matters:** PulseAudio suspends the PCM when idle, which is why
direct ALSA access currently works — but any system sound, browser tab, or
notification that hits the Delta will grab the PCM and the sampler's direct
`hw:` open can fail with `Device or resource busy`, or steal the device
mid-performance. This is precisely the failure the original masking was meant
to prevent.

**Decision required — pick one on the new PC:**

- **Option 1 — Dedicated audio machine (recommended, matches the doc).**
  Mask PipeWire *and* PulseAudio. ALSA-only. Lowest jitter, no device stealing.
  Downside: no system/browser sound.
- **Option 2 — Shared desktop.** Keep PulseAudio, but **move its default sink
  off the Delta** (use the onboard PCH, index 1) so the Delta stays reserved
  for the sampler. Add `~/.config/pulse/` sink pinning or set the PCH as
  default in `pavucontrol`. Slightly higher risk, keeps the desktop usable.

### Layer G — Application settings

`~/JaibaSampler/JaibaSampler.settings` (862 bytes):

```xml
<VALUE name="audioDeviceStateXml">
  <DEVICESETUP deviceType="ALSA" audioOutputDeviceName="default"
               audioInputDeviceName=""
               audioDeviceRate="48000.0" audioDeviceBufferSize="128"/>
<VALUE name="midiDevice" val="Teensy MIDI MIDI 1"/>
```

| Setting | Value | Note |
|---|---|---|
| Device | ALSA `default` | resolves via `.asoundrc` → Delta |
| Rate | 48000 Hz | matches card, no resampling in-app |
| Buffer | 128 samples | ≈ 2.7 ms |
| MIDI device | `Teensy MIDI MIDI 1` | **name-matched** → check it matches on the new PC |

**Transfer action:** copy the settings file so the app opens at 128/48k
immediately. ⚠️ The MIDI device is matched by the string `Teensy MIDI MIDI 1`,
which depends on the Teensy's USB descriptor and enumeration order. Confirm it
after connecting the drum; if it differs, re-select it once in the app.

### Layer H — Device access (udev, groups)

| File | Purpose | Portable? |
|---|---|---|
| `/etc/udev/rules.d/00-teensy.rules` | PJRC rules — Teensy USB-MIDI + `ttyACM` perms, ModemManager ignore | ✅ copy verbatim (repo copy exists at `Docs/00-teensy.rules`) |
| `/etc/udev/rules.d/49-teensy.rules` | tombstone ("obsolete, renamed") | optional |
| `/etc/udev/rules.d/99-bal313.rules` | **unrelated** (BAL313Qt load-cell machine, `1c00:3250`) | ❌ do not copy |
| groups `dialout`, `plugdev` | serial + device access | re-add user |

> `00-teensy.rules` is required for flashing the Teensys and for
> `ID_MM_DEVICE_IGNORE` (otherwise ModemManager eats MIDI bytes).

---

## 3. File-by-file migration manifest

Copy these from the source machine:

| Path | Destination | Mode |
|---|---|---|
| `~/.asoundrc` | `~/.asoundrc` (target user) | 664 |
| `~/JaibaSampler/JaibaSampler.settings` | same path | 644 |
| `~/.config/systemd/user/pipewire.service` → `/dev/null` | recreate mask | symlink |
| `~/.config/systemd/user/pipewire-pulse.service` → `/dev/null` | recreate mask | symlink |
| `Docs/00-teensy.rules` (repo) | `/etc/udev/rules.d/00-teensy.rules` | 644 root |

Recreate these system files (do not blindly copy — see §8):

| Path | Content |
|---|---|
| `/etc/default/grub` | `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash threadirqs"` + name-based `GRUB_DEFAULT` |
| `/etc/default/cpufrequtils` | `GOVERNOR="performance"` |
| `/etc/security/limits.conf` | append `@audio - rtprio 95`, `@audio - memlock unlimited` |
| `/etc/sysctl.conf` | append `vm.swappiness=10` |
| `/etc/systemd/system/set-delta-irq-priority.service` | **corrected** version (§8) |
| `/usr/local/sbin/set-delta-irq-priority.sh` | IRQ-number-agnostic helper (§8) |

Packages to install:

```bash
sudo apt install linux-lowlatency linux-headers-lowlatency \
                 alsa-utils alsa-tools-gui cpufrequtils rt-tests rtkit
# optional, only if you build the sampler on this machine:
sudo apt install build-essential cmake git libasound2-dev \
                 libfreetype6-dev libx11-dev libxrandr-dev libxinerama-dev \
                 libxcursor-dev libcurl4-openssl-dev
```

> `jaiba-sampler`'s README documents a **Windows/VS2022-only** build
> (`docs/ANALYSIS.md` §2 confirms: no Linux preset, links `vfw32.lib`). The
> Linux binary in use here is not described by the repo — plan for that
> separately if you intend to rebuild rather than copy the binary.

---

## 4. Recommended migration runbook

Ordered, with the blocking check first.

**Phase 0 — before touching the new PC**
1. **Verify a PCI slot exists.** `lspci`-check the new board, or plug the Delta
   in and confirm it appears in `lspci -nn | grep 1412`.
2. Copy `~/.asoundrc`, `~/JaibaSampler/`, and the sampler binary to the new PC.

**Phase 1 — OS base**
3. Install Ubuntu 24.04 LTS. Create the same username (or plan to edit paths —
   `.asoundrc` is path-independent, but `~/JaibaSampler` and the systemd user
   units are not).
4. Install the package set above.

**Phase 2 — apply tuning**
5. Run `sudo Scripts/audio-workstation-setup.sh`. It is idempotent and does
   Phases 2–3 in one shot.
6. Manually resolve `GRUB_DEFAULT` (the script prints the exact entry names and
   the command to run) — this cannot be automated safely.
7. Reboot.

**Phase 3 — verify (see §5)**

**Phase 4 — bring up the instrument**
8. Plug in the sensor Teensy, confirm `Teensy MIDI` appears, then the UI Teensy.
9. Launch the sampler, confirm it restores 128/48k and the MIDI device binds.
10. Re-measure latency; re-run the cyclic test.

---

## 5. Verification checklist

Run these on the new machine. Left column = command, right = expected.

```bash
uname -r                        # 6.8.0-138-lowlatency  (must end -lowlatency)
cat /proc/cmdline               # must contain threadirqs
cat /proc/asound/cards          # M1010LT present (index may differ — that's fine)
lspci -nnk | grep -A3 1412:1712 # "Kernel driver in use: snd_ice1712"
lsmod | grep snd_ice1712        # loaded
cat ~/.asoundrc                 # name-based, hw:CARD=M1010LT,DEV=0
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # performance
sysctl vm.swappiness            # = 10
ulimit -r; ulimit -l            # 95 ; unlimited   (after re-login)
id | grep -o audio              # audio group present

# audio actually comes out of the Delta:
speaker-test -c 2 -r 48000 -t wav -D plughw:CARD=M1010LT,DEV=0

# routing check through default (shell underruns are normal here):
aplay -D default -c 2 -r 48000 -f S16_LE --buffer-size=128 --period-size=64 \
      -d 1 /dev/zero

# IRQ priority — EXPECT SCHED_FIFO / 80
for p in $(pgrep -x -f 'irq/.*-snd_ice1712'); do chrt -p $p; done

# realtime health (should be near the source machine's avg 2 µs / max 265 µs)
sudo cyclictest -m -p80 -n -i 200 -h 400 -l 100000

# nothing else should be holding the card (ALSA-only setup):
fuser -v /dev/snd/*
```

If `ulimit -r` shows 0, the limits were applied but the session predates them —
log out and back in (or reboot).

---

## 6. Reference: full latency chain (unchanged by migration)

```
sensor Teensy (~2 ms peak window)
  → USB-MIDI
  → sampler (MIDI → audio block, sample-timestamped)
  → Delta 128 @ 48 kHz (~2.7 ms block)
  → analog outputs
```

Target contribution from the sampler: ~3–5 ms. Fallback if underruns appear on
the new hardware: 256 samples (≈5.3 ms). The sampler exposes
Settings → Audio → Buffer Size (64–512).

---

## 7. Drift: live system vs `AUDIO_WORKSTATION.md`

Two documented claims are no longer true. Neither breaks audio today, but both
should be corrected in the doc (or consciously re-decided) before the migration,
otherwise you will faithfully reproduce a state you did not intend.

| Doc claim | Live reality | Impact |
|---|---|---|
| Card order: PCH=0, MIDI=1, M1010LT=2 | **M1010LT=0, PCH=1**, no MIDI card (Teensy unplugged) | None — name-based `.asoundrc` is exactly the fix. The doc's *table* is just a stale snapshot; its *advice* is correct. |
| "PipeWire / PulseAudio disabled & masked" | PipeWire **is** masked; **PulseAudio is enabled and running**, holding the Delta as default sink | 🟠 Device-stealing risk. See §2 Layer F for the two options. |
| "Delta IRQ 18 → FIFO priority 80" | IRQ thread is `SCHED_FIFO` at **priority 50** | 🟠 Silent regression — see §8. |
| "Low-latency kernel 6.8.0-138 (default boot)" | True (`GRUB_DEFAULT="1>2"` selects it), but 7.0.0-31-generic is installed and owns the `/boot/vmlinuz` symlink | 🟠 Fragile: a GRUB regeneration can change which entry `1>2` points at. |

---

## 8. Bugs found (fix these on the new machine)

### 8.1 `set-delta-irq-priority.service` never boosts the right thread

The installed unit runs:

```bash
ExecStart=/usr/bin/bash -c 'PID=$(pgrep -f "irq/18-snd_ice"); [ ! -z "$PID" ] && chrt -f -p 80 $PID'
```

Three defects:

1. **`pgrep` self-match.** `pgrep -f` matches against the full command line, and
   the command line *contains the pattern string*. Demonstrated live:

   ```
   $ pgrep -f 'irq/18-snd_ice'
   772      <- the real IRQ thread
   6314     <- the bash running pgrep
   ```

   The wrapper shell gets boosted (or the whole list is passed to `chrt`), then
   exits — so systemd records `status=0/SUCCESS` while the IRQ thread is left
   at the `threadirqs` default of 50. That is exactly what the live system shows.

2. **Hardcoded IRQ 18.** IRQ assignment is a function of PCI slot, ACPI tables
   and the mainboard — it *will* differ on the new PC.

3. **Race.** `After=sysinit.target` does not guarantee the `snd_ice1712` IRQ
   thread exists yet. There is no retry.

**Fix** (`Scripts/set-delta-irq-priority.service` +
`/usr/local/sbin/set-delta-irq-priority.sh`): discover the thread by scanning
`/proc/*/comm` for the `*-snd_ice1712` suffix, retry for up to ~20 s, and never
match a shell. No IRQ number, no `pgrep` self-match, no race.

### 8.2 Onboard HDA (`snd_hda_intel`) is still active as card 1

`irq/46-snd_hda_intel:card1` exists and the PCH codec is loaded. On a dedicated
workstation this is dead weight that can also confuse PulseAudio's sink choice
(§2 Layer F). Optional hardening on the new machine: blacklist the onboard
codec, or leave it and just pin PulseAudio to it (Option 2).

---

## 9. Corrections recommended for `AUDIO_WORKSTATION.md`

1. Update the card-index table, or replace it with "indices float — always use
   `hw:CARD=M1010LT,DEV=0`" and drop the table entirely.
2. Rewrite the PipeWire/PulseAudio bullet to reflect the chosen Option 1/2, and
   state the current live state.
3. Fix the IRQ-priority bullet to match reality, and link the corrected unit.
4. Replace `GRUB_DEFAULT="1>2"` advice with `GRUB_DEFAULT=saved` +
   `grub-set-default`.
5. Add a one-line note that the driver is in-tree (no DKMS to port) and a
   prominent **PCI-slot** warning for any future machine change.

---

## 10. Appendix — raw captured values (source machine)

```
Host              jaiba-sampler
OS                Ubuntu 24.04.4 LTS (noble)
Kernel            6.8.0-138-lowlatency #138.1-Ubuntu SMP PREEMPT_DYNAMIC
CPU               Intel Core i5-3570 @ 3.40 GHz, 4 cores, 1 socket, no HT
RAM               31 GiB      Swap 8 GiB (/swap.img)
Mainboard         JINGSHA B75-HM PLUS, firmware 4.6.5 (2023-01-31)
Boot              UEFI, shimx64.efi, Boot0000
Cmdline           ro quiet splash threadirqs vt.handoff=7
GRUB              GRUB_DEFAULT="1>2", GRUB_CMDLINE_LINUX_DEFAULT="quiet splash threadirqs"

/proc/asound/cards
 0 [M1010LT ]: ICE1712 - M Audio Delta 1010LT  at 0xd040, irq 18
 1 [PCH     ]: HDA-Intel - HDA Intel PCH       at 0xf7f10000 irq 46

lspci             04:01.0 Multimedia audio controller [0401]:
                  VIA ICE1712 [Envy24] [1412:1712] (rev 02)
                  Subsystem: M-Audio Delta 1010LT [1412:d63b]
                  Kernel driver in use: snd_ice1712

IRQ threads       irq/18-snd_ice1712   SCHED_FIFO 50   (expected 80)
                  irq/46-snd_hda_intel:card1  SCHED_FIFO 50

limits            @audio - rtprio 95 ; @audio - memlock unlimited
                  /etc/security/limits.conf md5 b496603f… ≠ pkg 0b1967ff…  (edited)
sysctl            vm.swappiness = 10  (/etc/sysctl.conf md5 differs from package)
governor          performance (all 4 cores), cpufrequtils enabled
groups            jaiba: adm lp dialout cdrom sudo audio dip plugdev users lpadmin
snd modules       snd_ice1712, snd_ice17xx_ak4xxx, snd_cs8427, snd_i2c,
                  snd_ak4xxx_adda, snd_ac97_codec, snd_mpu401_uart,
                  snd_hda_intel, snd_hda_codec_realtek, snd_hda_codec_hdmi
audio packages    alsa-base 1.0.25, alsa-utils 1.2.9, alsa-tools-gui 1.2.11,
                  cpufrequtils 008-2build2, rt-tests 2.5-1, rtkit 0.13-5build1,
                  pulseaudio 16.1 (active), pipewire 1.0.5 (masked),
                  libjack-jackd2-0 1.9.21
sampler settings  ALSA / default / 48000.0 Hz / 128 samples
                  midiDevice = "Teensy MIDI MIDI 1"
cyclictest        avg 2 µs, max 265 µs  (as documented)
```
