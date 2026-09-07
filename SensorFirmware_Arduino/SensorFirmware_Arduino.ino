// ============================================================================
// SensorFirmware_Arduino.ino  —  Arduino-IDE single-sketch version of
//   jaiba-hexa-drum-sensor-code (PlatformIO project, src/main.cpp)
//
// A 6-pad MIDI drum trigger controller for a Teensy 4.1. Each pad combines a
// piezo disc (fast attack detection) and a velostat pad (sustain / release /
// aftertouch / soft-touch fallback). Output is native USB-MIDI on channel 1;
// it also listens for tuning/calibration commands from the TFT UI Teensy over
// Serial1 (pins 0/1, 115200 baud), per UART_PROTOCOL.md.
//
// ARDUINO IDE SETUP (important — this is a Teensy 4.1 sketch):
//   * Board:      Tools > Board > Teensy 4.1
//   * USB Type:   Tools > USB Type > "Serial + MIDI"   <-- REQUIRED
//     (PlatformIO's build flag `-D USB_MIDI_SERIAL` selects exactly this USB
//      mode: it exposes BOTH the USB MIDI port AND the USB Serial used for
//      debug prints/commands. Without it usbMIDI is not compiled in.)
//     This is a BOARD configuration, not a sketch #include/#define — see the
//     compile-time USB type guard right after #include <Arduino.h> below.
//     Compiling with any other USB Type is a deliberate #error so the failure
//     message tells you exactly what to change instead of a cryptic
//     "'usbMIDI' was not declared" error.
//   * Upload port: the Teensy's Serial/MIDI port.
//
// Serial Monitor: 115200 baud (commands: c / e <num> / + / - / p).
// Serial1 link to the UI Teensy: 115200 baud, pins 0 (RX1) / 1 (TX1), crossed
// TX<->RX, shared GND. Full protocol: UART_PROTOCOL.md.
//
// Hardware notes (see CLAUDE.md in the original repo for details):
//   Piezo:   piezo + --[10k]-- ADC pin --[10k]-- GND, plus 1N4148 clamps to
//            3.3V/GND. Piezo analog pins: A6..A11 (one per pad).
//   Velostat: 3.3V -- velostat -- ADC pin --+-- 100ohm -- GND. Velostat pins: A0..A5.
//   Buttons:  pin 3 = calibration, pin 4 = curve-preset cycle (INPUT_PULLUP).
//   ADC resolution is set to 12-bit in setup() (analogReadResolution(12)).
//
// NOTE: the bodies of setup()/loop() and all helpers below are UNCHANGED from
// the PlatformIO source. Two layout edits were made for the .ino conversion:
//   1. This header was added.
//   2. The struct/enum declarations + calibration globals were moved above the
//      first function so the Arduino IDE's auto-generated prototypes compile.
// ============================================================================

#include <Arduino.h>

// ============================================================================
// USB TYPE GUARD — why your build needs "Serial + MIDI"
//
// usbMIDI is part of the Teensy core, not a library a sketch can include. The
// core compiles the USB-MIDI descriptors, endpoints AND the `usbMIDI` object
// only when the board's USB Type includes MIDI: usb_inst.cpp defines
// `usb_midi_class usbMIDI;` inside `#ifdef MIDI_INTERFACE`, and
// MIDI_INTERFACE is only defined by the core's usb_desc.h when a MIDI-capable
// USB Type was selected at build time. So neither of these can work:
//     #include <usb_midi.h>   // content is wrapped in #ifdef MIDI_INTERFACE
//     #define USB_MIDI_SERIAL // a sketch #define never reaches core sources
// The selection is made by the BOARD's USB Type setting, which in PlatformIO
// was the line `build_flags = -D USB_MIDI_SERIAL` in platformio.ini.
//
// This sketch ALSO uses USB Serial (115200 debug console + commands), so the
// required USB Type is "Serial + MIDI". Guarding here turns a wrong setting
// into one clear #error instead of several "'usbMIDI' was not declared" errors.
// ============================================================================
#if !defined(USB_MIDI_SERIAL) && !defined(USB_MIDI4_SERIAL) && !defined(USB_MIDI16_SERIAL) && !defined(USB_MIDI_AUDIO_SERIAL) && !defined(USB_MIDI16_AUDIO_SERIAL)
  #error "SensorFirmware_Arduino requires USB Type 'Serial + MIDI'. Arduino IDE: Tools > USB Type > 'Serial + MIDI'. PlatformIO: build_flags = -D USB_MIDI_SERIAL. arduino-cli: --fqbn teensy:avr:teensy41:usb=serialmidi. (#include <usb_midi.h> cannot substitute for this board setting.)"
#endif

// Uses Teensy's native USB-MIDI (usbMIDI), not an external MIDI library —
// enabled by the board's USB Type "Serial + MIDI" (see guard above), which
// exposes both a MIDI port and Serial (used below for debug prints).

// ============================================================
// VELOSTAT SCALE — AUTO-DETECT CALIBRATION, 6 SENSORS
// + PIEZO CROSSTALK FILTERING (v2)
//
// CHANGELOG from previous version:
//   - analogReadResolution(12) added (was defaulting to 10-bit)
//   - PIEZO_THRESHOLD: 15 -> 30   (measured noise floor was ~3-24 counts)
//   - PIEZO_MAX:       500 -> 800 (measured hard-hit range was ~400-900,
//                                   extreme outliers 1000-2600 now clip to 127)
//   - NEW: hit buffering + relative-amplitude crosstalk filter.
//     Every hard hit was found to bleed into all other pads within 1-4ms,
//     at roughly 3-25% of the real hit's peak (measured on the shared
//     mounting surface). Real chords, by contrast, land at 50%+ of the
//     loudest simultaneous hit. So: buffer all hits within a short window,
//     then only fire the ones that clear CROSSTALK_RATIO of that window's
//     loudest hit. This preserves chords while rejecting crosstalk.
//
// Calibration flow (button on calButton, or 'c' over Serial):
//   1st press (idle)      -> start: rest 3s, then wait for touch
//   touch an uncalibrated pad -> auto-detects it, captures peak,
//                                 marks it done, waits for next
//   touch an already-done pad -> tells you, keeps waiting
//   all pads done          -> calibration finishes automatically
//   press again (idle)     -> starts over from scratch
//   press again (mid-run)  -> ABORTS and copies the last captured
//                              max to every pad (quick uniform cal)
//
// Wiring per pad (3.3V logic):
//   3.3V -- velostat -- analog pin --+-- 100ohm -- GND
// All pads share the same 3.3V and GND rails.
//
// Serial Monitor commands (115200 baud):
//   c        -> same as pressing the button
//   e <num>  -> set curve exponent for ALL pads, e.g. "e 2.0"
//   +  / -   -> nudge exponent by 0.1
//   p        -> print current settings
// ============================================================

const int sensorPins[] = { A0, A1, A2, A3, A4, A5 };
const int piezoPins[]  = { A6, A7, A8, A9, A10, A11 }; // one piezo per pad
const int scaleNotes[] = { 60, 62, 64, 66, 68, 70 }; // C major-ish, C4-A#4

const int NUM_SENSORS = sizeof(sensorPins) / sizeof(sensorPins[0]);
const int MAX_SENSORS = 16; // static allocation ceiling, raise if needed

const int calButton   = 3;
const int curveButton = 4;   // press to cycle through curve presets

// --- Status LED (Teensy 4.1 onboard LED, pin 13 / LED_BUILTIN) -------------
// Live "is the sensor alive" feed:
//   * slow heartbeat (500ms half-period) proves the firmware loop is running
//   * every UART message sent to the UI Teensy (Serial1) triggers a ~60ms
//     bright pulse, so hits / ACKs / CAL_STATE all show up as live activity
// Entirely non-blocking (millis()-based, no delay()) -- no effect on the
// piezo/MIDI latency path. Pin 13 is unused by the sensor wiring.
const int statusLedPin = LED_BUILTIN;
const unsigned long LED_HEARTBEAT_MS = 500;  // heartbeat half-period
const unsigned long LED_ACTIVITY_MS  = 60;   // TX pulse length

bool ledHeartbeatOn = false;
unsigned long ledHeartbeatAt = 0;
unsigned long ledActivityUntil = 0;


// --- Smoothing (shared) ---
const float VEL_SMOOTHING = 0.15f;

// --- Curve shaping (shared across all pads) ---
// Press curveButton to step through these in order, wrapping around.
const float curvePresets[] = { 1.0f, 1.4f, 1.8f, 2.2f, 2.6f, 3.0f };
const int NUM_CURVE_PRESETS = sizeof(curvePresets) / sizeof(curvePresets[0]);
int curvePresetIndex = 2; // starts at 1.8, matching the old default
float curveExponent = curvePresets[curvePresetIndex];

// --- Trigger / release thresholds, as % of range above rest ---
const float TRIGGER_PCT = 0.12f;
const float RELEASE_PCT = 0.02f;

// --- Play mode: restricts which sensor path can trigger a NEW note. Sustain/
// release/aftertouch stay velostat-owned regardless, once a note is sounding. ---
enum PlayMode { PLAY_BOTH, PLAY_PIEZO_ONLY, PLAY_VELOSTAT_ONLY };
PlayMode playMode = PLAY_BOTH;

// --- Arduino-IDE compatibility note: every struct/enum type declaration and every
// typed global used by function signatures below is kept ABOVE the first function
// definition (playModeName). The Arduino IDE generates prototypes for all sketch
// functions and inserts them just before the first function; types referenced by
// those prototypes must already be declared at that point or compilation fails.
// (PlatformIO / plain C++ does not need this ordering, but it is harmless there.) ---
enum TouchState { TOUCH_IDLE, TOUCH_ATTACK, TOUCH_HELD };

struct Sensor {
  int pin;
  int note;
  float smoothed;
  int veloRest;
  int veloMax;
  TouchState touchState;
  unsigned long attackStart;
  float attackPeak;
  int lastSentVelocity;
  unsigned long lastMidiSend;
  bool noteOn;          // true once a note is sounding, from either piezo or velostat

  // --- Piezo (fast-path attack detection) ---
  int piezoPin;
  bool hitting;
  int piezoPeak;
  unsigned long peakTime;
  unsigned long lastHitTime;
  float piezoAdaptiveMax; // rises instantly on a harder-than-ever hit, decays slowly otherwise
  int piezoThreshold;         // per-pad copy of PIEZO_THRESHOLD, initialized from it in setup()
  int piezoCeilingBaseline;   // per-pad copy of PIEZO_MAX, initialized from it in setup()
};

Sensor sensors[MAX_SENSORS];

// --- Calibration FSM ---
// CalState is the phase (rest/wait/capture); CalSensorType and CalScope are
// orthogonal axes selecting *what* is being calibrated. Kept as separate
// enums rather than crossed into one (e.g. CAL_RESTING_PIEZO_SINGLE) since
// the phase logic is identical for both sensors/all scopes and only a few
// read/write points inside each phase need to branch on sensor or scope.
enum CalState { CAL_IDLE, CAL_RESTING, CAL_WAITING_TOUCH, CAL_CAPTURING };
CalState calState = CAL_IDLE;

enum CalSensorType { CAL_SENSOR_VELOSTAT, CAL_SENSOR_PIEZO };
enum CalScope       { CAL_SCOPE_ALL, CAL_SCOPE_SINGLE, CAL_SCOPE_SINGLE_COPY_ALL };

CalSensorType calSensorType = CAL_SENSOR_VELOSTAT;
CalScope      calScope      = CAL_SCOPE_ALL;
int           calTargetPad  = -1;   // pad requested for SINGLE / SINGLE_COPY_ALL; -1 for ALL

const int PIEZO_CAL_MARGIN = 20;    // added to measured piezo noise floor to get calibrated piezoThreshold

bool calibratedFlag[MAX_SENSORS];
bool wasAboveDetect[MAX_SENSORS];   // for "already calibrated" one-shot notice
int  activePad = -1;
unsigned long captureStart = 0;
float capturePeak = 0;
int  lastCompletedMax = -1;         // most recent finished pad's max (for abort-uniform)

unsigned long calRestStart = 0;
long calRestTotal[MAX_SENSORS];
int  calRestSamples[MAX_SENSORS];
const unsigned long CAL_REST_DURATION = 3000;

unsigned long lastPrint = 0;
const unsigned long PRINT_INTERVAL = 150;
const char* playModeName(PlayMode m) {
  if (m == PLAY_PIEZO_ONLY) return "PIEZO_ONLY";
  if (m == PLAY_VELOSTAT_ONLY) return "VELOSTAT_ONLY";
  return "BOTH";
}

// --- Attack capture window (for normal note playing) ---
const unsigned long ATTACK_WINDOW_MS = 30;
const unsigned long MIDI_UPDATE_INTERVAL = 15;

// --- Calibration detect / capture tuning (velostat) ---
// Percent of full playing range above rest, same definition as TRIGGER_PCT
// (which this was never updated to match when TRIGGER_PCT was tuned for the
// same underlying vibration-crosstalk problem during normal play) — a fixed
// raw-count margin doesn't scale with a pad's actual range, so a hard hit's
// mechanical bleed-through into a neighboring pad during calibration could
// swamp a small fixed margin without being a meaningful fraction of that
// pad's own range.
const float DETECT_MARGIN_PCT   = 0.12f;
const unsigned long CAPTURE_WINDOW_MS = 1200; // how long to sample peak once detected

// --- Piezo hit detection (fast path, low latency) ---
// Re-tuned from real 12-bit readings (rest ~3-24, soft hits ~40-200,
// hard hits ~400-900, rare outliers up to ~2600 which now clip to 127).
int PIEZO_THRESHOLD = 45;     // raw ADC counts above baseline = "that's a hit"
int PIEZO_MAX       = 800;    // raw peak that maps to velocity 127
const unsigned long PIEZO_DEBOUNCE   = 35;  // ms — min gap between hits on the same pad
const unsigned long PIEZO_PEAK_WINDOW = 2;  // ms — how long to sample the true peak of a hit
//   (was 3 ms; reduced to 2 ms for lower attack latency. Piezo rise time is
//   sub-ms, and the per-pad adaptive ceiling still maps your hardest hits to
//   velocity 127. Raise back to 3 if velocity feels compressed on soft hits.)
float PIEZO_CURVE_EXP = 0.45f;              // piezo response curve (separate from velostat's)

// --- Adaptive velocity ceiling (per pad) ---
// PIEZO_MAX above is the starting/floor ceiling. Any hit harder than a
// pad's current ceiling instantly becomes the new ceiling for that pad, so
// your hardest hit always maps to velocity 127 — no more guessing one fixed
// number that's sometimes too low (undershoots 127) or too high (compresses
// your normal-intensity range). The ceiling decays slowly back toward
// PIEZO_MAX if you stop hitting that hard, so a single outlier hit doesn't
// permanently flatten your dynamics for the rest of the session.
const unsigned long ADAPTIVE_DECAY_INTERVAL = 1000;  // ms between decay steps
const float ADAPTIVE_DECAY_FACTOR = 0.985f;          // ceiling *= this, each interval
unsigned long lastAdaptiveDecayTime = 0;

// --- Crosstalk filtering (low-latency version) ---
// The FIRST hit to arrive opens a short window and fires immediately —
// measured data shows the real strike always arrives before any crosstalk
// it causes, so there's no need to delay it. Any further hits that land
// within CROSSTALK_WINDOW ms of that first hit are only accepted (fired)
// if they're >= CROSSTALK_RATIO of the window-opening hit's peak; weaker
// ones are discarded as mechanical bleed-through. This keeps chords intact
// while cutting solo-hit latency down to just the peak-finding window.
unsigned long CROSSTALK_WINDOW = 5;   // ms
float CROSSTALK_RATIO = 0.40f;        // measured chords cleared 50-97%; worst crosstalk was 32%

bool windowOpen = false;
unsigned long windowStart = 0;
int windowMaxPeak = 0;


void printSettings();
void onCalibrationTrigger();
void startCalibration(CalSensorType sensor, CalScope scope, int targetPad);
void abortCalibration();
void abortToUniformCalibration();

void ledActivityPulse() { ledActivityUntil = millis() + LED_ACTIVITY_MS; }

void updateStatusLed() {
  if (millis() - ledHeartbeatAt >= LED_HEARTBEAT_MS) {
    ledHeartbeatAt = millis();
    ledHeartbeatOn = !ledHeartbeatOn;
  }
  bool on = (millis() < ledActivityUntil) || ledHeartbeatOn;
  digitalWrite(statusLedPin, on ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000);
  Serial1.begin(115200);
  analogReadResolution(12);
  pinMode(calButton, INPUT_PULLUP);
  pinMode(curveButton, INPUT_PULLUP);
  pinMode(statusLedPin, OUTPUT);
  digitalWrite(statusLedPin, LOW);

  // usbMIDI needs no real initialization on Teensy: the USB stack boots and
  // enumerates automatically as soon as the board powers up, and outgoing
  // notes are sent on demand. usbMIDI.begin() exists in the core as a no-op
  // (kept for API compatibility), and is called here explicitly so the
  // intent is visible.
  usbMIDI.begin();

  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].pin = sensorPins[i];
    sensors[i].note = scaleNotes[i];
    sensors[i].smoothed = 0;
    sensors[i].veloRest = 20;
    sensors[i].veloMax = 900;
    sensors[i].touchState = TOUCH_IDLE;
    sensors[i].attackStart = 0;
    sensors[i].attackPeak = 0;
    sensors[i].lastSentVelocity = -1;
    sensors[i].lastMidiSend = 0;
    sensors[i].noteOn = false;

    sensors[i].piezoPin = piezoPins[i];
    sensors[i].hitting = false;
    sensors[i].piezoPeak = 0;
    sensors[i].peakTime = 0;
    sensors[i].lastHitTime = 0;
    sensors[i].piezoThreshold = PIEZO_THRESHOLD;
    sensors[i].piezoCeilingBaseline = PIEZO_MAX;
    sensors[i].piezoAdaptiveMax = (float)sensors[i].piezoCeilingBaseline;
  }

  Serial.print("=== Velostat scale ("); Serial.print(NUM_SENSORS); Serial.println(" sensors) ===");
  Serial.println("Commands: c=calibrate  e <num>=set exponent  +/-=nudge  p=print");
  Serial.println("Buttons: calButton=calibrate  curveButton=cycle curve preset");
  Serial.println("------------------------------------");
  printSettings();
}

void printSettings() {
  Serial.print("exponent="); Serial.println(curveExponent, 2);
  for (int i = 0; i < NUM_SENSORS; i++) {
    Serial.print("  pad "); Serial.print(i);
    Serial.print(" (note "); Serial.print(sensors[i].note); Serial.print(")");
    Serial.print("  rest="); Serial.print(sensors[i].veloRest);
    Serial.print("  max="); Serial.print(sensors[i].veloMax);
    Serial.print("  piezoThresh="); Serial.print(sensors[i].piezoThreshold);
    Serial.print("  piezoCeil="); Serial.println(sensors[i].piezoAdaptiveMax, 0);
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "c") {
    onCalibrationTrigger();
  } else if (line == "p") {
    printSettings();
  } else if (line == "+") {
    curveExponent += 0.1f;
    printSettings();
  } else if (line == "-") {
    curveExponent = max(0.1f, curveExponent - 0.1f);
    printSettings();
  } else if (line.startsWith("e ")) {
    float v = line.substring(2).toFloat();
    if (v > 0) { curveExponent = v; printSettings(); }
  }
}

// Parses commands from the TFT Teensy over Serial1, per UART_PROTOCOL.md.
// Implements every command listed there.
void handleSerial1() {
  if (!Serial1.available()) return;
  String line = Serial1.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line.length() > 63) return; // guard against runaway/garbled lines

  int firstComma = line.indexOf(',');
  String cmd  = (firstComma == -1) ? line : line.substring(0, firstComma);
  String rest = (firstComma == -1) ? ""   : line.substring(firstComma + 1);

  // Every recognized line is answered (ACK/ERR/PADVAL/...), so pulse the
  // status LED to signal UART TX activity.
  ledActivityPulse();

  if (cmd == "SET_NOTE") {
    int comma = rest.indexOf(',');
    if (comma == -1) { Serial1.println("ERR,MALFORMED"); return; }
    int padIndex = rest.substring(0, comma).toInt();
    int note = rest.substring(comma + 1).toInt();
    if (padIndex < 0 || padIndex >= NUM_SENSORS) { Serial1.println("ERR,BAD_PAD_INDEX"); return; }
    sensors[padIndex].note = note;
    Serial1.println("ACK,SET_NOTE");

  } else if (cmd == "SET_CURVE") {
    if (rest.length() == 0) { Serial1.println("ERR,MALFORMED"); return; }
    float exponent = rest.toFloat();
    if (exponent <= 0) { Serial1.println("ERR,MALFORMED"); return; }
    PIEZO_CURVE_EXP = exponent;
    Serial1.println("ACK,SET_CURVE");

  } else if (cmd == "SET_XTALK") {
    int comma = rest.indexOf(',');
    if (comma == -1) { Serial1.println("ERR,MALFORMED"); return; }
    float ratio = rest.substring(0, comma).toFloat();
    long windowMs = rest.substring(comma + 1).toInt();
    if (ratio <= 0 || windowMs <= 0) { Serial1.println("ERR,MALFORMED"); return; }
    CROSSTALK_RATIO = ratio;
    CROSSTALK_WINDOW = (unsigned long)windowMs;
    Serial1.println("ACK,SET_XTALK");

  } else if (cmd == "CAL_START") {
    int c1 = rest.indexOf(',');
    String sensorStr = (c1 == -1) ? rest : rest.substring(0, c1);
    String afterSensor = (c1 == -1) ? "" : rest.substring(c1 + 1);

    CalSensorType sensor;
    if (sensorStr == "PIEZO") sensor = CAL_SENSOR_PIEZO;
    else if (sensorStr == "VELOSTAT") sensor = CAL_SENSOR_VELOSTAT;
    else { Serial1.println("ERR,MALFORMED"); return; }

    int c2 = afterSensor.indexOf(',');
    String scopeStr = (c2 == -1) ? afterSensor : afterSensor.substring(0, c2);
    String padStr   = (c2 == -1) ? ""          : afterSensor.substring(c2 + 1);

    CalScope scope;
    if (scopeStr == "ALL") scope = CAL_SCOPE_ALL;
    else if (scopeStr == "SINGLE") scope = CAL_SCOPE_SINGLE;
    else if (scopeStr == "SINGLE_COPY_ALL") scope = CAL_SCOPE_SINGLE_COPY_ALL;
    else { Serial1.println("ERR,MALFORMED"); return; }

    int padIndex = -1;
    if (scope != CAL_SCOPE_ALL) {
      if (padStr.length() == 0) { Serial1.println("ERR,MALFORMED"); return; }
      padIndex = padStr.toInt();
      if (padIndex < 0 || padIndex >= NUM_SENSORS) { Serial1.println("ERR,BAD_PAD_INDEX"); return; }
    }

    if (calState != CAL_IDLE) {
      if (sensor != calSensorType) { Serial1.println("ERR,CAL_BUSY"); return; }
      abortCalibration(); // CAL_START for the sensor already running aborts it, same as button/'c'
      Serial1.println("ACK,CAL_START");
      return;
    }

    startCalibration(sensor, scope, padIndex);
    Serial1.println("ACK,CAL_START");

  } else if (cmd == "SET_PLAY_MODE") {
    if (rest == "BOTH") playMode = PLAY_BOTH;
    else if (rest == "PIEZO_ONLY") playMode = PLAY_PIEZO_ONLY;
    else if (rest == "VELOSTAT_ONLY") playMode = PLAY_VELOSTAT_ONLY;
    else { Serial1.println("ERR,MALFORMED"); return; }
    Serial1.println("ACK,SET_PLAY_MODE");

  } else if (cmd == "SET_THRESH") {
    int comma = rest.indexOf(',');
    if (comma == -1) { Serial1.println("ERR,MALFORMED"); return; }
    int padIndex = rest.substring(0, comma).toInt();
    int threshold = rest.substring(comma + 1).toInt();
    if (padIndex < 0 || padIndex >= NUM_SENSORS) { Serial1.println("ERR,BAD_PAD_INDEX"); return; }
    if (threshold <= 0) { Serial1.println("ERR,MALFORMED"); return; }
    sensors[padIndex].piezoThreshold = threshold;
    Serial1.println("ACK,SET_THRESH");

  } else if (cmd == "SET_CEILING_BASELINE") {
    int comma = rest.indexOf(',');
    if (comma == -1) { Serial1.println("ERR,MALFORMED"); return; }
    int padIndex = rest.substring(0, comma).toInt();
    int baseline = rest.substring(comma + 1).toInt();
    if (padIndex < 0 || padIndex >= NUM_SENSORS) { Serial1.println("ERR,BAD_PAD_INDEX"); return; }
    if (baseline <= 0) { Serial1.println("ERR,MALFORMED"); return; }
    sensors[padIndex].piezoCeilingBaseline = baseline;
    // Snap the live ceiling to match immediately (both directions) so a
    // GET_PAD right after this reflects the new value, not a stale one
    // waiting on the next hard hit (raising) or gradual decay (lowering).
    sensors[padIndex].piezoAdaptiveMax = (float)baseline;
    Serial1.println("ACK,SET_CEILING_BASELINE");

  } else if (cmd == "GET_PAD") {
    int padIndex = rest.toInt();
    if (padIndex < 0 || padIndex >= NUM_SENSORS) { Serial1.println("ERR,BAD_PAD_INDEX"); return; }
    Sensor &s = sensors[padIndex];
    Serial1.print("PADVAL,"); Serial1.print(padIndex);
    Serial1.print(","); Serial1.print(s.note);
    Serial1.print(","); Serial1.print(s.piezoThreshold);
    Serial1.print(","); Serial1.print(s.piezoAdaptiveMax, 0);
    Serial1.print(","); Serial1.println(PIEZO_CURVE_EXP, 2);

  } else if (cmd == "GET_XTALK") {
    Serial1.print("XTALKVAL,"); Serial1.print(CROSSTALK_RATIO, 2);
    Serial1.print(","); Serial1.println(CROSSTALK_WINDOW);

  } else if (cmd == "GET_PLAY_MODE") {
    Serial1.print("PLAY_MODE,"); Serial1.println(playModeName(playMode));

  } else {
    Serial1.println("ERR,UNKNOWN_COMMAND");
  }
}

// Edge-triggered, debounced button reads — fire once per physical press.
// Two separate functions (not a shared struct type) so Teensy/Arduino's
// auto-generated function prototypes — inserted near the top of the file,
// before any of our own struct definitions — don't choke on an unknown type.
bool calLastReading = HIGH, calDebouncedState = HIGH;
unsigned long calLastChangeTime = 0;

bool calButtonPressedEdge() {
  bool reading = digitalRead(calButton);
  if (reading != calLastReading) {
    calLastChangeTime = millis();
    calLastReading = reading;
  }
  if (millis() - calLastChangeTime > 40 && reading != calDebouncedState) {
    calDebouncedState = reading;
    if (calDebouncedState == LOW) return true;
  }
  return false;
}

bool curveLastReading = HIGH, curveDebouncedState = HIGH;
unsigned long curveLastChangeTime = 0;

bool curveButtonPressedEdge() {
  bool reading = digitalRead(curveButton);
  if (reading != curveLastReading) {
    curveLastChangeTime = millis();
    curveLastReading = reading;
  }
  if (millis() - curveLastChangeTime > 40 && reading != curveDebouncedState) {
    curveDebouncedState = reading;
    if (curveDebouncedState == LOW) return true;
  }
  return false;
}

void onCurveButtonTrigger() {
  curvePresetIndex = (curvePresetIndex + 1) % NUM_CURVE_PRESETS;
  curveExponent = curvePresets[curvePresetIndex];
  Serial.print("Curve exponent -> "); Serial.println(curveExponent, 2);
}

const char* calSensorName(CalSensorType s) {
  return (s == CAL_SENSOR_PIEZO) ? "PIEZO" : "VELOSTAT";
}

void sendCalState(const char* state) {
  Serial1.print("CAL_STATE,"); Serial1.print(calSensorName(calSensorType)); Serial1.print(","); Serial1.println(state);
  ledActivityPulse();
}

void applyVelostatCalToAllPads(int rest, int maxVal) {
  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].veloRest = rest;
    sensors[i].veloMax = maxVal;
  }
}

void applyPiezoCalToAllPads(int threshold, int ceilingBaseline) {
  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].piezoThreshold = threshold;
    sensors[i].piezoCeilingBaseline = ceilingBaseline;
    sensors[i].piezoAdaptiveMax = (float)ceilingBaseline; // snap immediately, don't wait for decay
  }
}

// ---- Single entry point for "the button/command was triggered" ----
// Legacy physical-button / USB 'c' path — always VELOSTAT + ALL scope,
// unchanged from before this generalization (see abortCalibration() for
// why its abort behavior is still safe when a differently-scoped run is
// active).
void onCalibrationTrigger() {
  if (calState == CAL_IDLE) {
    startCalibration(CAL_SENSOR_VELOSTAT, CAL_SCOPE_ALL, -1);
  } else {
    abortCalibration();
  }
}

void startCalibration(CalSensorType sensor, CalScope scope, int targetPad) {
  calSensorType = sensor;
  calScope = scope;
  calTargetPad = targetPad;

  Serial.print("=== CALIBRATION STARTED ("); Serial.print(calSensorName(sensor));
  Serial.print(scope == CAL_SCOPE_ALL ? ", ALL" : (scope == CAL_SCOPE_SINGLE ? ", SINGLE" : ", SINGLE_COPY_ALL"));
  if (scope != CAL_SCOPE_ALL) { Serial.print(" pad="); Serial.print(targetPad); }
  Serial.println(") ===");
  Serial.println(">>> Don't touch any pad. Resting for 3 sec...");
  sendCalState("RESTING");
  for (int i = 0; i < NUM_SENSORS; i++) {
    calRestTotal[i] = 0;
    calRestSamples[i] = 0;
    calibratedFlag[i] = false;
    wasAboveDetect[i] = false;
  }
  lastCompletedMax = -1;
  activePad = -1;
  calRestStart = millis();
  calState = CAL_RESTING;
}

void finishRestPhase() {
  bool restrictToTarget = (calScope != CAL_SCOPE_ALL);
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (restrictToTarget && i != calTargetPad) continue; // SINGLE/SINGLE_COPY_ALL: leave other pads untouched

    int restAvg = (calRestSamples[i] > 0) ? (int)(calRestTotal[i] / calRestSamples[i]) : 20;
    if (calSensorType == CAL_SENSOR_PIEZO) {
      sensors[i].piezoThreshold = restAvg + PIEZO_CAL_MARGIN;
      Serial.print("Pad "); Serial.print(i); Serial.print(" piezo rest -> threshold = "); Serial.println(sensors[i].piezoThreshold);
    } else {
      sensors[i].veloRest = restAvg;
      Serial.print("Pad "); Serial.print(i); Serial.print(" rest = "); Serial.println(sensors[i].veloRest);
    }
  }

  if (restrictToTarget) {
    Serial.print(">>> Touch pad "); Serial.print(calTargetPad); Serial.println(" and press it as hard as you'll ever play.");
  } else {
    Serial.println(">>> Touch ANY pad and press it as hard as you'll ever play.");
    Serial.println("    (order doesn't matter — I'll figure out which pad it is)");
  }
  sendCalState("WAITING_TOUCH");
  calState = CAL_WAITING_TOUCH;
}

// Legacy uncertain-data abort path — used only for ALL-scope runs (the
// physical button/USB 'c' always uses this scope, and it's an accepted,
// pre-existing risk for that low-stakes path). Copies one representative
// pad's calibration to every pad using whatever data is available, even if
// that data is incomplete (mid-capture) — NOT used for SINGLE or
// SINGLE_COPY_ALL, see abortCalibration() below for why.
void abortToUniformCalibration() {
  int uniformPeak;
  if (calState == CAL_CAPTURING && activePad >= 0) {
    // Use whatever peak has been seen so far on the pad currently being captured
    uniformPeak = (int)capturePeak;
  } else if (lastCompletedMax > 0) {
    uniformPeak = lastCompletedMax;
  } else {
    uniformPeak = 900; // no data yet at all — fall back to a safe default
  }

  if (calSensorType == CAL_SENSOR_PIEZO) {
    long threshSum = 0;
    for (int i = 0; i < NUM_SENSORS; i++) threshSum += sensors[i].piezoThreshold;
    int uniformThreshold = threshSum / NUM_SENSORS;

    applyPiezoCalToAllPads(uniformThreshold, uniformPeak);

    Serial.println("=== CALIBRATION ABORTED — applying uniform PIEZO values to ALL pads ===");
    Serial.print("uniform threshold="); Serial.print(uniformThreshold);
    Serial.print("  uniform ceiling="); Serial.println(uniformPeak);
  } else {
    // Average rest across pads that finished the rest phase (falls back to defaults if aborted super early)
    long restSum = 0;
    for (int i = 0; i < NUM_SENSORS; i++) restSum += sensors[i].veloRest;
    int uniformRest = restSum / NUM_SENSORS;

    applyVelostatCalToAllPads(uniformRest, uniformPeak);

    Serial.println("=== CALIBRATION ABORTED — applying uniform VELOSTAT values to ALL pads ===");
    Serial.print("uniform rest="); Serial.print(uniformRest);
    Serial.print("  uniform max="); Serial.println(uniformPeak);
  }
  sendCalState("ABORTED");
  printSettings();

  calState = CAL_IDLE;
  activePad = -1;
}

// Single entry point for "abort the active run" — dispatches on scope since
// SINGLE/SINGLE_COPY_ALL must NEVER apply partial/uncertain data to other
// pads (we've been burned by exactly this before: an incomplete velostat
// run once produced max=25-32 vs. a realistic ~900 and nearly went live).
// SINGLE_COPY_ALL fans out to every pad specifically because it's meant to
// be a deliberate, confident operation — applying uncertain data on abort
// would be a worse failure than the legacy ALL-scope fallback below, which
// stays as an accepted, low-stakes risk for that one specific path.
void abortCalibration() {
  if (calScope == CAL_SCOPE_ALL) {
    abortToUniformCalibration();
  } else {
    Serial.println("=== CALIBRATION ABORTED — no data applied ===");
    sendCalState("ABORTED");
    calState = CAL_IDLE;
    activePad = -1;
  }
}

void updateCalibration() {
  unsigned long now = millis();

  if (calState == CAL_RESTING) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      int pin = (calSensorType == CAL_SENSOR_PIEZO) ? sensors[i].piezoPin : sensors[i].pin;
      calRestTotal[i] += analogRead(pin);
      calRestSamples[i]++;
    }
    if (now - calRestStart >= CAL_REST_DURATION) {
      finishRestPhase();
    }
    return;
  }

  if (calState == CAL_WAITING_TOUCH) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (calScope != CAL_SCOPE_ALL && i != calTargetPad) continue; // SINGLE/SINGLE_COPY_ALL: ignore every other pad

      int pin = (calSensorType == CAL_SENSOR_PIEZO) ? sensors[i].piezoPin : sensors[i].pin;
      int raw = analogRead(pin);
      int detectThreshold;
      if (calSensorType == CAL_SENSOR_PIEZO) {
        // Piezo's detect threshold is its just-measured piezoThreshold (already rest+margin, set in finishRestPhase()).
        detectThreshold = sensors[i].piezoThreshold;
      } else {
        // Percent of full playing range above rest, matching TRIGGER_PCT's definition.
        float range = (float)(sensors[i].veloMax - sensors[i].veloRest);
        if (range < 1) range = 1;
        detectThreshold = sensors[i].veloRest + (int)(range * DETECT_MARGIN_PCT);
      }
      bool above = raw > detectThreshold;

      if (above && !wasAboveDetect[i]) {
        // Fresh touch onset on pad i
        wasAboveDetect[i] = true;
        if (calibratedFlag[i]) {
          Serial.print("Pad "); Serial.print(i); Serial.println(" already calibrated — try a different pad.");
        } else {
          activePad = i;
          capturePeak = raw;
          captureStart = now;
          calState = CAL_CAPTURING;
          Serial.print(">>> Detected pad "); Serial.print(i); Serial.println(" — capturing peak, keep pressing...");
          Serial1.print("CAL_STATE,"); Serial1.print(calSensorName(calSensorType)); Serial1.print(",CAPTURING,"); Serial1.println(i);
          ledActivityPulse();
          break; // handle one pad at a time
        }
      } else if (!above) {
        wasAboveDetect[i] = false;
      }
    }
    return;
  }

  if (calState == CAL_CAPTURING) {
    int pin = (calSensorType == CAL_SENSOR_PIEZO) ? sensors[activePad].piezoPin : sensors[activePad].pin;
    int raw = analogRead(pin);
    if (raw > capturePeak) capturePeak = raw;

    if (now - captureStart >= CAPTURE_WINDOW_MS) {
      if (calSensorType == CAL_SENSOR_PIEZO) {
        int rest = sensors[activePad].piezoThreshold;
        sensors[activePad].piezoCeilingBaseline = (capturePeak > rest + 20) ? (int)capturePeak : rest + 200;
        sensors[activePad].piezoAdaptiveMax = (float)sensors[activePad].piezoCeilingBaseline; // snap immediately, don't wait for decay
        lastCompletedMax = sensors[activePad].piezoCeilingBaseline;
        Serial.print("Pad "); Serial.print(activePad);
        Serial.print(" piezo calibrated. ceiling="); Serial.println(sensors[activePad].piezoCeilingBaseline);
      } else {
        int rest = sensors[activePad].veloRest;
        sensors[activePad].veloMax = (capturePeak > rest + 20) ? (int)capturePeak : rest + 200;
        lastCompletedMax = sensors[activePad].veloMax;
        Serial.print("Pad "); Serial.print(activePad);
        Serial.print(" calibrated. max="); Serial.println(sensors[activePad].veloMax);
      }
      calibratedFlag[activePad] = true;
      Serial1.print("CAL_STATE,"); Serial1.print(calSensorName(calSensorType)); Serial1.print(",PAD_DONE,"); Serial1.println(activePad);
      ledActivityPulse();

      int finishedPad = activePad;
      activePad = -1;
      for (int i = 0; i < NUM_SENSORS; i++) wasAboveDetect[i] = false; // require full release before next detect

      if (calScope == CAL_SCOPE_ALL) {
        int doneCount = 0;
        for (int i = 0; i < NUM_SENSORS; i++) if (calibratedFlag[i]) doneCount++;
        Serial.print(doneCount); Serial.print(" of "); Serial.print(NUM_SENSORS); Serial.println(" pads done.");

        if (doneCount >= NUM_SENSORS) {
          Serial.println("=== CALIBRATION COMPLETE ===");
          sendCalState("COMPLETE");
          printSettings();
          calState = CAL_IDLE;
        } else {
          Serial.println(">>> Touch the next uncalibrated pad.");
          calState = CAL_WAITING_TOUCH;
        }
      } else {
        // SINGLE / SINGLE_COPY_ALL: exactly one pad is in scope, and it just finished.
        if (calScope == CAL_SCOPE_SINGLE_COPY_ALL) {
          if (calSensorType == CAL_SENSOR_PIEZO) {
            applyPiezoCalToAllPads(sensors[finishedPad].piezoThreshold, sensors[finishedPad].piezoCeilingBaseline);
          } else {
            applyVelostatCalToAllPads(sensors[finishedPad].veloRest, sensors[finishedPad].veloMax);
          }
        }
        Serial.println("=== CALIBRATION COMPLETE ===");
        sendCalState("COMPLETE");
        printSettings();
        calState = CAL_IDLE;
      }
    }
    return;
  }
}

int mapToVelocity(Sensor &s, float pressure) {
  float range = (float)(s.veloMax - s.veloRest);
  if (range < 1) range = 1;
  float norm = (pressure - s.veloRest) / range;
  norm = constrain(norm, 0.0f, 1.0f);
  float mapped = pow(norm, curveExponent);
  return constrain((int)(mapped * 127), 0, 127);
}

int getPiezoVelocity(int peak, int threshold, float ceiling) {
  if (peak <= threshold) return 0;
  float clamped = constrain((float)peak, (float)threshold, ceiling);
  float norm = (clamped - threshold) / (ceiling - threshold);
  float mapped = pow(norm, PIEZO_CURVE_EXP);
  return constrain((int)(mapped * 127), 1, 127);
}

// Actually fires a piezo-triggered note.
void firePiezoNote(int padIndex, int peak) {
  Sensor &s = sensors[padIndex];

  // A new hardest hit instantly raises this pad's ceiling, so THIS hit
  // itself maps to exactly 127 rather than clipping against an old ceiling.
  if ((float)peak > s.piezoAdaptiveMax) {
    s.piezoAdaptiveMax = (float)peak;
  }

  int hitVelocity = getPiezoVelocity(peak, s.piezoThreshold, s.piezoAdaptiveMax);
  if (hitVelocity >= 3) {
    usbMIDI.sendNoteOn(s.note, hitVelocity, 1);
    s.lastSentVelocity = hitVelocity;
    s.noteOn = true;
    Serial1.print("HIT,"); Serial1.print(padIndex); Serial1.print(","); Serial1.print(hitVelocity); Serial1.println(",P");
    ledActivityPulse();
    Serial.print("PAD "); Serial.print(padIndex);
    Serial.print(" PIEZO HIT  note="); Serial.print(s.note);
    Serial.print("  peak="); Serial.print(peak);
    Serial.print("  ceil="); Serial.print(s.piezoAdaptiveMax, 0);
    Serial.print("  vel="); Serial.println(hitVelocity);
  }
}

// Called the instant a hit's peak has been captured. If no window is
// currently open (or the previous one has expired), this hit starts a new
// window and fires right away — no added latency for solo hits. If a
// window is already open (i.e. this hit landed within CROSSTALK_WINDOW ms
// of the hit that opened it), it only fires if it's a strong enough
// fraction of that hit's peak to be a real simultaneous strike (chord)
// rather than mechanical crosstalk.
void handleNewHit(int padIndex, int peak) {
  unsigned long now = millis();

  if (!windowOpen || (now - windowStart > CROSSTALK_WINDOW)) {
    windowOpen = true;
    windowStart = now;
    windowMaxPeak = peak;
    firePiezoNote(padIndex, peak); // first hit in the window — fire immediately
    return;
  }

  // Inside an a lready-open window: only accept if strong enough relative
  // to the hit that opened it.
  if (peak >= CROSSTALK_RATIO * windowMaxPeak) {
    firePiezoNote(padIndex, peak); // real chord note
  }
  // else: rejected as crosstalk, discarded silently
}

// Called once per loop; every ADAPTIVE_DECAY_INTERVAL ms, each pad's ceiling
// relaxes slightly toward the tuned baseline (PIEZO_MAX). This means a
// single outlier-hard hit doesn't permanently compress your normal dynamic
// range — if you don't keep hitting that hard, sensitivity gradually comes
// back. The ceiling never decays below PIEZO_MAX itself.
void decayAdaptiveCeilings() {
  unsigned long now = millis();
  if (now - lastAdaptiveDecayTime < ADAPTIVE_DECAY_INTERVAL) return;
  lastAdaptiveDecayTime = now;

  for (int i = 0; i < NUM_SENSORS; i++) {
    float decayed = sensors[i].piezoAdaptiveMax * ADAPTIVE_DECAY_FACTOR;
    sensors[i].piezoAdaptiveMax = max((float)sensors[i].piezoCeilingBaseline, decayed);
  }
}

void loop() {
  updateStatusLed();   // heartbeat + UART-TX activity LED (non-blocking)
  handleSerial();
  handleSerial1();

  if (calButtonPressedEdge()) {
    onCalibrationTrigger();
  }
  if (curveButtonPressedEdge()) {
    onCurveButtonTrigger();
  }

  if (calState != CAL_IDLE) {
    updateCalibration();
    return;
  }

  unsigned long now = millis();
  decayAdaptiveCeilings();
  bool doPrint = (now - lastPrint >= PRINT_INTERVAL);
  if (doPrint) lastPrint = now;
  // Never let the USB debug stream stall the loop: if no host is reading the
  // console the TX buffer can fill, and Serial.print() would then block and
  // delay pad sampling. Skip this burst unless there is room for it. The
  // cadence timer above still advances, so printing resumes when space frees.
  bool canPrint = doPrint && (Serial.availableForWrite() > 96);

  for (int i = 0; i < NUM_SENSORS; i++) {
    Sensor &s = sensors[i];

    // ---------- PIEZO: fast attack detection (low latency) ----------
    // Guarded with !s.noteOn so a note that's already sustaining (e.g. held
    // during a chord) can't be retriggered by small settling vibration —
    // only a fresh hit after release should start a new piezo attack.
    int rawPiezo = analogRead(s.piezoPin);
    if (!s.hitting && !s.noteOn && playMode != PLAY_VELOSTAT_ONLY && rawPiezo > s.piezoThreshold && (now - s.lastHitTime > PIEZO_DEBOUNCE)) {
      s.hitting = true;
      s.piezoPeak = rawPiezo;
      s.peakTime = now;
      // A real hit is starting — cancel any in-progress velostat attack capture
      // so it doesn't also fire a duplicate, slower note right after this one.
      s.touchState = TOUCH_IDLE;
    } else if (s.hitting) {
      if (rawPiezo > s.piezoPeak) s.piezoPeak = rawPiezo;
      if (now - s.peakTime >= PIEZO_PEAK_WINDOW) {
        s.hitting = false;
        s.lastHitTime = now;
        // Evaluate immediately — fires right away if it's the first hit in
        // a window, or checked against that hit's peak if it's a follower.
        handleNewHit(i, s.piezoPeak);
      }
    }

    // ---------- VELOSTAT: sustain / release, and soft-touch fallback trigger ----------
    int raw = analogRead(s.pin);
    s.smoothed = s.smoothed * (1.0f - VEL_SMOOTHING) + raw * VEL_SMOOTHING;

    float range = (float)(s.veloMax - s.veloRest);
    if (range < 1) range = 1;
    float pct = (s.smoothed - s.veloRest) / range;

    if (!s.noteOn) {
      // No note sounding yet — velostat can still trigger one for touches too
      // soft to excite the piezo (e.g. slow presses with no sharp attack).
      if (s.touchState == TOUCH_IDLE) {
        if (playMode != PLAY_PIEZO_ONLY && pct >= TRIGGER_PCT) {
          s.touchState = TOUCH_ATTACK;
          s.attackStart = now;
          s.attackPeak = s.smoothed;
        }
      } else if (s.touchState == TOUCH_ATTACK) {
        if (s.smoothed > s.attackPeak) s.attackPeak = s.smoothed;
        if (now - s.attackStart >= ATTACK_WINDOW_MS) {
          int hitVelocity = mapToVelocity(s, s.attackPeak);
          usbMIDI.sendNoteOn(s.note, max(hitVelocity, 1), 1);
          s.lastSentVelocity = hitVelocity;
          s.noteOn = true;
          s.lastHitTime = now;
          s.touchState = TOUCH_HELD;
          Serial1.print("HIT,"); Serial1.print(i); Serial1.print(","); Serial1.print(hitVelocity); Serial1.println(",V");
          ledActivityPulse();
          Serial.print("PAD "); Serial.print(i);
          Serial.print(" VELO TRIGGER  note="); Serial.print(s.note);
          Serial.print("  vel="); Serial.println(hitVelocity);
        }
      }
    } else {
      // Note is sounding (from piezo or velostat) — velostat now owns
      // sustain (continuous aftertouch) and release, regardless of origin.
      if (pct <= RELEASE_PCT) {
        usbMIDI.sendNoteOff(s.note, 0, 1);
        s.noteOn = false;
        s.touchState = TOUCH_IDLE;
        Serial.print("PAD "); Serial.print(i); Serial.println(" NOTE OFF");
      } else if (now - s.lastMidiSend >= MIDI_UPDATE_INTERVAL) {
        int heldVelocity = mapToVelocity(s, s.smoothed);
        if (heldVelocity != s.lastSentVelocity) {
          usbMIDI.sendAfterTouchPoly(s.note, heldVelocity, 1); // per-note aftertouch
          s.lastSentVelocity = heldVelocity;
        }
        s.lastMidiSend = now;
        s.touchState = TOUCH_HELD;
      }
    }

    if (canPrint) {
      Serial.print("pad"); Serial.print(i);
      Serial.print(" raw="); Serial.print(raw);
      Serial.print(" pct="); Serial.print(pct * 100, 0);
      Serial.print("%  ");
    }
  }

  if (canPrint) Serial.println();
}