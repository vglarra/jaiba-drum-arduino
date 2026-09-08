# Jaiba Sampler — Requirements & Latency Notes

JUCE sampler (`jaiba-sampler/`, standalone desktop app). Companion docs:
`Docs/UI_ROADMAP.md` (UI Teensy screens + Phase 4 knob-controller plan),
`Docs/ARDUINO_CLI_SETUP.md` (Teensy toolchain).

---

## REQUIREMENT — Multi-device MIDI input (3 Teensys)

Context: the drum system has three Teensy 4.1 boards, each its own USB-MIDI port:
- **Sensor 1** (right hemisphere, hub 1-1.5) → drum note-ons
- **Sensor 2** (left hemisphere, future) → drum note-ons
- **UI Teensy** (hub 1-1.6) → control (CC knobs), NOT notes

Current sampler limitation (2026-09): only ONE MIDI input device is opened at
a time (`std::unique_ptr<MidiInput> midiInput` + a single saved `midiDevice`
name). Routing is note-number-only inside that one stream.

### Required behaviour
1. Persist a **list of MIDI input devices with a role per device**
   (`Drums` vs `Control`), e.g. in settings:
   `midiDevices = ["Teensy MIDI MIDI 1"->Drums, "Teensy MIDI MIDI 2"->Drums,
   "Teensy MIDI"->Control]`.
2. Open **all listed devices simultaneously**; each auto-restores on launch
   (reuse the timed-retry logic in `maybeRestoreMidiDevice()` per device).
3. Route by **source device + message type**:
   - `Drums` devices → note-on/off routed to pads by their per-pad
     `midiNote`. Keep notes **globally unique across both sensor devices**
     (sampler pads 1..16 already map to distinct notes) so a note can never
     be ambiguous between hemispheres. Optional per-device note-base offset
     to organise ranges.
   - `Control` device (UI Teensy) → **CC → sampler parameter** map (knobs:
     volume, loop, filter/EQ, ADSR, transport, fx). Ignore CC from Drums
     devices and note-ons from Control.
4. UI: a "MIDI Devices" panel where each available device is added once with
   its role; disable/remove entries; settings persist + auto-connect all.

### Open sub-questions
- Do the two drum devices need to distinguish which *bank* (kit) they play,
  or is a single 16-pad bank with hemisphere note ranges enough?
- CC map: fixed (documented) or learnable/assignable per knob? UI_ROADMAP
  Phase 4 assumes the UI Teensy sends a defined CC table.

---

## LATENCY — percussive path & improvements

### The full chain (pad → ears)
```
Sensor Teensy: piezo→vel (~2 ms peak window, 2 ms after tuning 2026-09)
   → USB-MIDI → (host)
Sampler: JUCE MidiInput → midiCollector (sample-timestamped)
   → audio callback → pad voice starts at the event's sample position
   → audio device output (interface buffer) → ears
```

### What the sampler already does well (verified in code)
- Note-ons are queued via `MidiMessageCollector` and merged into the audio
  block **at their sample timestamp** (mid-block triggers, not next-block) —
  good, this is the low-latency design.
- MIDI→audio latency is actually measured per note (`lastMidiLatencyUs`,
  printed by the 500 ms CPU timer in `updateDeviceInfo`) — we can read it.
- Block-budget/underrun monitor exists; sample changes mute atomically and
  engines are preloaded async so triggers never load from disk.

### Dominant remaining costs (biggest lever first)
1. **Audio output device + buffer size.** Saved config is ALSA `default`
   512 samples @ 44.1 kHz ≈ **11.6 ms** block + whatever the backend adds.
   - If `default` routes through PipeWire/PulseAudio, expect EXTRA latency
     and hard-to-control periods.
   - Action: select a **direct ALSA device** (`hw:...` / interface) or tune
     PipeWire quantum/rate; run **64–128 samples @ 48 kHz** (~1.3–2.7 ms) if
     the interface + CPU sustain it. Expose a "Low-Latency / Safe" preset
     that sets device+rate+buffer, and make 128@48k the shipped default.
2. **Interface latency floor** — USB interfaces add their own buffering;
   pick the device with the lowest reported latency.
3. **CPU headroom at small buffers** — with 64–128 samples the audio thread
   budget is ~1.3–2.7 ms; heavy per-block work (FFT viz, per-pad DSP, resample)
   must stay lean or underruns appear. Keep the budget monitor on; consider
   decimating the visualizer update rate if it spikes.
4. **Micro-tuning:**
   - Keep `handleIncomingMidiMessage` trivial (UI label flashes off the audio
     thread) — already structured that way; don't add blocking work there.
   - Ensure pads don't resample on load at trigger time (preload already
     handles this — keep it).
   - Debug prints must never run on the audio thread (already avoided).

### Suggested next steps (in order)
1. Add a **"Latency" readout** (measured `lastMidiLatencyUs`, smoothed) + the
   audio device/buffer info — so changes are measurable, not guessed.
2. Ship **low-latency defaults/presets** (direct ALSA or tuned PipeWire;
   128/48 preferred, 64 experimental) and confirm no underruns.
3. Measure with the real setup (pad → sampler) using the readout; target
   ~3–5 ms sampler contribution and a repeatable underrun-free value.
4. Only then touch DSP/voice internals if the readout still shows excess.

Status: analysis 2026-09 — not yet implemented.
