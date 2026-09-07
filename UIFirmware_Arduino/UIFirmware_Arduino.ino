// ============================================================================
// UIFirmware_Arduino.ino  —  Arduino-IDE single-sketch version of the
//   jaiba-hexa-drum-ui PlatformIO project (src/main.cpp + uart_protocol.* +
//   drum_state.* + screens.*), for a Teensy 4.1 with an ST7796 (320x480) SPI
//   display and an XPT2046 resistive touch controller.
//
// THIS SKETCH FOLDER CONTAINS TWO FILES — this is intentional:
//   * UIFirmware_Arduino.ino  — all firmware code, merged into one file
//   * tft_setup.h             — TFT_eSPI compile-time configuration
// TFT_eSPI is configured with preprocessor defines, never at runtime. The
// original PlatformIO project kept them in platformio.ini build_flags; the
// Arduino IDE has no per-sketch build-flag mechanism. tft_setup.h (which
// mirrors those build_flags 1:1) is provided in this folder — see its header
// comment for the two ways the defines reach the compiler. The guaranteed
// Arduino-IDE route is: paste the contents of tft_setup.h into the installed
// TFT_eSPI library's User_Setup.h (replacing its default content). Some
// toolchains instead auto-pick tft_setup.h from the sketch folder via
// TFT_eSPI's __has_include(<tft_setup.h>) check — PlatformIO and some Arduino
// IDE builds do; the arduino-cli/IDE-2.x Teensy build tested for this
// conversion does not, so treat Route 1 as the required setup for the IDE.
//
// ARDUINO IDE SETUP (Teensy 4.1):
//   * Board:       Tools > Board > Teensy 4.1
//   * USB Type:    Serial (default) is fine — this board talks to the drum
//                  Teensy over hardware Serial1, not over USB MIDI.
//   * Library:     Tools > Manage Libraries > install "TFT_eSPI" by Bodmer
//                  (2.5.43 or later), then apply the config per tft_setup.h.
//
// WIRING (display + touch share one SPI bus; see Claude.md in the original
// repo for the confirmed board photo mapping, board silkscreen "X320 V1.1"):
//   Display ST7796: SCK=13  MOSI=11  MISO=12  CS=10  DC=8  RST=9  LED->3.3V
//   Touch XPT2046:  T_CLK=13 T_DIN=11 T_DO=12  T_CS=6   T_IRQ=7 (unused;
//                   code polls instead of using the IRQ line)
//   Do NOT use 5V anywhere on this module — it is 3.3V logic throughout.
//
// UART LINK TO THE DRUM TEENSY (Serial1, per UART_PROTOCOL.md):
//   * Serial1 at 115200 baud, pins 0 (RX1) / 1 (TX1), wired crossed TX<->RX
//     to the drum Teensy's Serial1, shared GND. 115200 on both sides.
//   * USB Serial runs at 9600 baud (Serial Monitor): type "DUMP" to print the
//     drum state; any other line is forwarded to the drum Teensy over Serial1
//     (manual protocol testing, e.g. "GET_PAD,0" or "CAL_START,VELOSTAT,ALL").
//
// BEHAVIOR (unchanged from the original project):
//   * Boot -> splash screen ("Initializing / Jaiba Hexa Drum UI"), auto-
//     advances to the landing placeholder after ~1.8s. Screen switching is the
//     minimal switchScreen()/updateScreen() manager from screens.*.
//   * The XPT2046 touch layer from the phase-1 hardware bring-up is restored
//     here (it had been replaced by throwaway test code in the repo's history)
//     and wired into the screen loop: a press draws a red dot at the mapped
//     location + prints raw/mapped coordinates to Serial; a tap during the
//     splash screen advances to the landing page. Calibration constants were
//     derived by touching the physical screen corners — re-derive if the
//     display is ever swapped (see the touch section below).
//   * The UART protocol layer (uart_protocol.*) and drum state model
//     (drum_state.*) are merged verbatim below; HIT/PADVAL/CAL_STATE/
//     XTALKVAL/PLAY_MODE/ACK/ERR are parsed into drumState, and screens read
//     drumState directly.
// ============================================================================

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <stdio.h>    // snprintf (monitor readback rows)
#include <string.h>   // strncpy/memcpy for the event-log ring buffer

TFT_eSPI tft = TFT_eSPI();

// ============================================================================
// SECTION: drum_state.h  (shared state model)
// ============================================================================


// Number of physical pads currently wired (indices 0-5, matching the drum
// sketch's sensors[] array). Update if more pads get built.
constexpr int NUM_PADS = 6;

struct PadState {
    int note = -1;
    int threshold = -1;
    int ceiling = -1;
    float curveExp = -1.0f;
    unsigned long lastUpdated = 0; // millis() of last PADVAL received; 0 = never
};

enum class PlayMode { UNKNOWN, BOTH, PIEZO_ONLY, VELOSTAT_ONLY };

enum class CalSensor { NONE, PIEZO, VELOSTAT };
enum class CalPhase { NONE, RESTING, WAITING_TOUCH, CAPTURING, PAD_DONE, COMPLETE, ABORTED };

struct CalState {
    CalSensor sensor = CalSensor::NONE;
    CalPhase phase = CalPhase::NONE;
    int padIndex = -1; // only meaningful when phase is CAPTURING or PAD_DONE
    unsigned long lastUpdated = 0;
};

// Fixed-buffer ring of the most recent human-readable protocol events,
// shown on the MONITOR screen. Char buffers on purpose -- no String
// allocation in the UART receive path.
constexpr int EVENT_LOG_LINES = 6;
constexpr int EVENT_LOG_COLS  = 28;

struct EventLog {
    char lines[EVENT_LOG_LINES][EVENT_LOG_COLS];
    int head = 0;   // index of the newest entry
    int count = 0;  // number of valid entries (<= EVENT_LOG_LINES)

    void add(const char* text) {
        head = (head + 1) % EVENT_LOG_LINES;
        int len = strlen(text);
        if (len > EVENT_LOG_COLS - 1) len = EVENT_LOG_COLS - 1;
        memcpy(lines[head], text, len);
        lines[head][len] = '\0';
        if (count < EVENT_LOG_LINES) count++;
    }

    // Entry for display row r: 0 = oldest .. count-1 = newest.
    const char* get(int r) const {
        int idx = (head - (count - 1 - r) + EVENT_LOG_LINES) % EVENT_LOG_LINES;
        return lines[idx];
    }
};

struct DrumState {
    PadState pads[NUM_PADS];

    float crosstalkRatio = -1.0f;
    int crosstalkWindowMs = -1;
    unsigned long xtalkLastUpdated = 0;

    PlayMode playMode = PlayMode::UNKNOWN;
    unsigned long playModeLastUpdated = 0;

    CalState calState;

    // Link health: stamped with millis() whenever ANY line is parsed from
    // the drum Teensy. The MONITOR screen treats > LINK_TIMEOUT_MS without
    // a message as OFFLINE.
    unsigned long lastContactMs = 0;

    // Recent protocol events (HIT / ACK / ERR / ...) for the MONITOR log.
    EventLog eventLog;
};

// Single global instance -- there is exactly one drum Teensy link. Screens
// read from this directly; only uart_protocol.cpp's message handlers write
// to it.
DrumState drumState;  // single global instance (one drum Teensy link; only the UART-protocol handlers write to it)

// Screen identifiers for the screen manager (screens section below).
// Declared here, together with the other types, so that the Arduino IDE's
// auto-generated function prototypes -- inserted just before the FIRST function
// definition in this file -- can see ScreenId (switchScreen() takes it by
// value). Types used in function signatures must precede that insertion point.
enum class ScreenId {
    SPLASH,
    LANDING_PLACEHOLDER,   // hex pad grid + footer (hemisphere toggle + Monitor)
    MONITOR,               // sensor link status + event log + per-pad readback
    PAD_DETAIL,            // per-pad tuning panel (Phase 2)
};



// ============================================================================
// SECTION: uart_protocol.h  (interface)
// ============================================================================


// Result of the most recent SET_*/CAL_START command sent via sendCommand().
// GET_* queries don't use this -- their responses land directly in
// drumState (see drum_state.h) and screens read the state, not this result.
enum class CommandResult { NONE, PENDING, SUCCESS, FAILED, TIMED_OUT };

// Must be called once from setup() before any other uart_protocol function.
void uartProtocolInit();

// Must be called every loop() iteration -- reads Serial1, accumulates
// lines, and dispatches complete messages into drumState.
void uartProtocolUpdate();

// Sends a command line to the drum Teensy (newline appended automatically).
//
// NOTE: only one SET_*/CAL_START command can be in flight at a time --
// there is a single pending-ACK slot, not a queue. Sending a second
// mutating command before the first resolves (see getLastCommandResult())
// silently overwrites the pending slot, so the first command's eventual
// ACK/ERR will look like a stray/unmatched response. Screens must wait
// for getLastCommandResult() to leave PENDING before firing another
// SET_*/CAL_START.
void sendCommand(const String& cmd);

// Status of the last SET_*/CAL_START command sent. Returns TIMED_OUT if
// no ACK/ERR arrived within the timeout window, so a screen never gets
// stuck showing an indefinite "waiting" spinner on a dropped message.
CommandResult getLastCommandResult();

// Reason string from the most recent ERR. Only meaningful when
// getLastCommandResult() == FAILED.
const String& getLastFailureReason();


// ============================================================================
// SECTION: hex_grid  (hexagonal pad-layout widget — from src/hex_grid.h/.cpp)
// ============================================================================
// Renders the drum shell's 9 tile positions (hexagonal, from the drum CAD)
// scaled to the display: populated pads as solid labeled hexagons, unbuilt
// tile slots as dashed hexagons. drawHexGrid() is currently previewed as the
// whole landing screen (see screens section); hexGridHitTest() is ready for
// when a screen owns touch input. Declared here, before the first function in
// this file, so the Arduino IDE's auto-generated prototypes can see the types.



// One of the drum shell's 9 right-hemisphere tile positions, in mm, using
// the drum CAD's coordinate convention (+y is up -- flipped to screen
// space, +y down, when rendered). Fixed design-time layout pulled from the
// drum project's Blender CAD, not computed at runtime.
struct HexTile {
    const char* id;   // e.g. "R02" -- reference/debugging only, not shown
    float xMm;
    float yMm;
    bool populated;
    int sensorIndex;  // 0-based UART sensor index; -1 when not populated
};

// ---- Hemisphere tile tables (from the drum Blender CAD script) -------------
// The shell is split at x=0 into two plates: RIGHT (9 tiles, x=0 column
// included) and LEFT (7 tiles). Units are mm in the CAD convention (+y up);
// rendering flips y to screen space. sensorIndex is a 0-based UART sensor
// index (right plate == sensor Teensy 1 today; left plate will map to
// sensor Teensy 2). populated=false = real shell position, not wired yet.
const HexTile HEX_TILES_RIGHT[9] = {
    { "R00",  38.5f,  100.0f, false, -1 },
    { "R01", 115.5f,  100.0f, false, -1 },
    { "R02",   0.0f,   33.3f, true,   0 },
    { "R03",  77.0f,   33.3f, true,   1 },
    { "R04", 154.0f,   33.3f, false, -1 },
    { "R05",  38.5f,  -33.3f, true,   3 },
    { "R06", 115.5f,  -33.3f, true,   5 },
    { "R07",   0.0f, -100.0f, true,   4 },
    { "R08",  77.0f, -100.0f, true,   2 },
};

// LEFT hemisphere -- currently a MOCK: geometry is real (mirror of the
// right plate around x=0, derived from the Blender CAD split script at the
// same 77 mm scale), but no pads are wired yet (future sensor Teensy 2).
const HexTile HEX_TILES_LEFT[7] = {
    { "L00", -115.5f,  100.0f, false, -1 },
    { "L01",  -38.5f,  100.0f, false, -1 },
    { "L02", -154.0f,   33.3f, false, -1 },
    { "L03",  -77.0f,   33.3f, false, -1 },
    { "L04", -115.5f,  -33.3f, false, -1 },
    { "L05",  -38.5f,  -33.3f, false, -1 },
    { "L06",  -77.0f, -100.0f, false, -1 },
};

constexpr int HEX_TILES_RIGHT_COUNT = 9;
constexpr int HEX_TILES_LEFT_COUNT  = 7;
constexpr int MAX_HEX_TILES = 16;   // upper bound for either side's layout cache

// Active hemisphere -- drawHexGrid()/hexGridHitTest() render the side
// selected with hexSelectSide().
const HexTile* activeTiles = HEX_TILES_RIGHT;
int activeTileCount = HEX_TILES_RIGHT_COUNT;
bool activeSideRight = true;

// Width across flats (mm) of each hexagonal tile, from the drum CAD.
constexpr float HEX_FLAT_WIDTH_MM = 77.0f;

// Invoked with a HEX_TILES index (0..HEX_TILE_COUNT-1) when a tile is
// tapped. Nothing calls this yet -- accepted and stored so the landing
// page and Pad Assignment can both reuse this component for tap-to-select
// once they own touch input.
using HexTileTapHandler = void (*)(int tileIndex);

// Renders all 9 tiles, uniformly scaled and centered to fit inside the
// given content rectangle (screen px). Callers should size that rectangle
// to leave room for a footer below it -- this component only fills what
// it's given. `selectedIndex` (a HEX_TILES index, or -1 for none)
// highlights one populated tile; `onTap` is stored for hexGridHitTest()'s
// future caller, not invoked here.
void drawHexGrid(TFT_eSPI& tft, int contentX, int contentY, int contentW, int contentH,
                  int selectedIndex = -1, HexTileTapHandler onTap = nullptr);

// Returns the HEX_TILES index whose hexagon contains (screenX, screenY),
// or -1 if none -- uses the layout computed by the most recent
// drawHexGrid() call. Not called by anything yet; here so a future
// screen's touch handling doesn't need to reimplement hex hit-testing.
int hexGridHitTest(int screenX, int screenY);

// Select which hemisphere drawHexGrid()/hexGridHitTest() operate on.
void hexSelectSide(bool right);



constexpr float SQRT_3 = 1.7320508f;

void hexSelectSide(bool right) {
    activeSideRight = right;
    if (right) { activeTiles = HEX_TILES_RIGHT; activeTileCount = HEX_TILES_RIGHT_COUNT; }
    else       { activeTiles = HEX_TILES_LEFT;  activeTileCount = HEX_TILES_LEFT_COUNT;  }
}

// Circumradius (center-to-vertex): for a regular hexagon,
// width-across-flats = sqrt(3) * circumradius.
constexpr float HEX_CIRCUMRADIUS_MM = HEX_FLAT_WIDTH_MM / SQRT_3;

// Ratio of apothem (center-to-flat-edge) to circumradius, i.e. cos(30deg)
// -- used for hit-testing (see hexGridHitTest).
constexpr float HEX_APOTHEM_RATIO = 0.8660254f;

constexpr float LAYOUT_MARGIN_FACTOR = 0.92f; // breathing room at the content edges

struct TileLayout {
    int cx = 0, cy = 0; // pixel center
    int r = 0;          // pixel circumradius, as drawn
};

TileLayout lastLayout[MAX_HEX_TILES];
bool haveLayout = false;
HexTileTapHandler tapHandler = nullptr;

void hexVertex(int cx, int cy, int r, int i, int& vx, int& vy) {
    // 30-degree start -> pointy-top hexagons (vertices up/down, flat edges
    // left/right), matching the Blender CAD script's HEX_ANGLES and the
    // tile-center spacing derived from it. A 0-degree start (flat-top)
    // mismatches that spacing and produces overlapping/gapping tiles.
    float angleRad = (30.0f + 60.0f * i) * (PI / 180.0f);
    vx = cx + (int)roundf(r * cosf(angleRad));
    vy = cy + (int)roundf(r * sinf(angleRad));
}

void drawFilledHex(TFT_eSPI& tft, int cx, int cy, int r, uint16_t fillColor, uint16_t outlineColor) {
    int vx[6], vy[6];
    for (int i = 0; i < 6; i++) hexVertex(cx, cy, r, i, vx[i], vy[i]);

    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        tft.fillTriangle(cx, cy, vx[i], vy[i], vx[j], vy[j], fillColor);
    }
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        tft.drawLine(vx[i], vy[i], vx[j], vy[j], outlineColor);
    }
}

void drawDashedLine(TFT_eSPI& tft, float x0, float y0, float x1, float y1, uint16_t color) {
    constexpr float DASH_LEN = 4.0f;
    constexpr float GAP_LEN = 3.0f;

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) return;

    float ux = dx / len, uy = dy / len;
    for (float pos = 0.0f; pos < len; pos += DASH_LEN + GAP_LEN) {
        float segEnd = (pos + DASH_LEN < len) ? pos + DASH_LEN : len;
        tft.drawLine((int)roundf(x0 + ux * pos), (int)roundf(y0 + uy * pos),
                      (int)roundf(x0 + ux * segEnd), (int)roundf(y0 + uy * segEnd), color);
    }
}

void drawDashedHex(TFT_eSPI& tft, int cx, int cy, int r, uint16_t color) {
    int vx[6], vy[6];
    for (int i = 0; i < 6; i++) hexVertex(cx, cy, r, i, vx[i], vy[i]);

    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        drawDashedLine(tft, vx[i], vy[i], vx[j], vy[j], color);
    }
}

void drawHexGrid(TFT_eSPI& tft, int contentX, int contentY, int contentW, int contentH,
                  int selectedIndex, HexTileTapHandler onTap) {
    tapHandler = onTap;

    // Bounding box of tile centers, in screen-space mm (CAD's +y-up
    // flipped to +y-down).
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < activeTileCount; i++) {
        float x = activeTiles[i].xMm;
        float y = -activeTiles[i].yMm;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }

    // Pad by each hex's own extent so edge tiles aren't clipped. Pointy-top
    // orientation (see hexVertex) puts flat edges left/right and vertices
    // up/down, so the horizontal reach from center is the apothem
    // (half the flat width) and the vertical reach is the circumradius --
    // the opposite of what a flat-top hex would need here.
    minX -= HEX_FLAT_WIDTH_MM / 2.0f;
    maxX += HEX_FLAT_WIDTH_MM / 2.0f;
    minY -= HEX_CIRCUMRADIUS_MM;
    maxY += HEX_CIRCUMRADIUS_MM;

    float layoutWidthMm = maxX - minX;
    float layoutHeightMm = maxY - minY;

    float scaleX = (float)contentW / layoutWidthMm;
    float scaleY = (float)contentH / layoutHeightMm;
    float scale = (scaleX < scaleY ? scaleX : scaleY) * LAYOUT_MARGIN_FACTOR;

    float layoutCenterXmm = (minX + maxX) / 2.0f;
    float layoutCenterYmm = (minY + maxY) / 2.0f;
    int contentCenterX = contentX + contentW / 2;
    int contentCenterY = contentY + contentH / 2;

    int hexRadiusPx = (int)roundf(HEX_CIRCUMRADIUS_MM * scale);

    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);

    for (int i = 0; i < activeTileCount; i++) {
        const HexTile& tile = activeTiles[i];
        float xMm = tile.xMm;
        float yMm = -tile.yMm;

        int cx = contentCenterX + (int)roundf((xMm - layoutCenterXmm) * scale);
        int cy = contentCenterY + (int)roundf((yMm - layoutCenterYmm) * scale);

        lastLayout[i] = { cx, cy, hexRadiusPx };

        if (tile.populated) {
            bool selected = (i == selectedIndex);
            uint16_t fillColor = selected ? TFT_ORANGE : TFT_DARKCYAN;
            uint16_t outlineColor = selected ? TFT_YELLOW : TFT_WHITE;
            drawFilledHex(tft, cx, cy, hexRadiusPx, fillColor, outlineColor);

            tft.setTextColor(TFT_WHITE, fillColor);
            String label = "Pad " + String(tile.sensorIndex + 1);
            tft.drawString(label, cx, cy);
        } else {
            drawDashedHex(tft, cx, cy, hexRadiusPx, TFT_DARKGREY);
        }
    }

    tft.setTextDatum(TL_DATUM);
    haveLayout = true;
}

int hexGridHitTest(int screenX, int screenY) {
    if (!haveLayout) return -1;

    for (int i = 0; i < activeTileCount; i++) {
        // Hit-test against the inscribed circle (apothem), not the
        // circumradius used for drawing -- adjacent tile centers are
        // spaced exactly one flat-width apart, so apothem-radius hit
        // circles are tangent rather than overlapping. Undershoots the
        // hexagon's pointed top/bottom corners slightly (pointy-top
        // orientation, see hexVertex), but avoids ambiguous double-hits
        // along shared edges. The apothem/circumradius ratio itself
        // (cos(30deg)) is rotation-invariant, so this needed no change
        // when hexVertex's angle offset changed.
        int hitR = (int)(lastLayout[i].r * HEX_APOTHEM_RATIO);
        int dx = screenX - lastLayout[i].cx;
        int dy = screenY - lastLayout[i].cy;
        if (dx * dx + dy * dy <= hitR * hitR) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// SECTION: uart_protocol.cpp  (implementation)
// ============================================================================


constexpr int UART_LINE_MAX = 63;
constexpr unsigned long ACK_TIMEOUT_MS = 1000; // every round-trip so far has been well under 100ms

struct PendingAck {
    bool active = false;
    String command;
    unsigned long sentAt = 0;
    int padIndex = -1; // only meaningful when command is a pad-field setter (see isPadFieldSetter)
};

PendingAck pendingAck;
CommandResult lastResult = CommandResult::NONE;
String lastFailureReason;

String commandName(const String& line) {
    int comma = line.indexOf(',');
    return comma < 0 ? line : line.substring(0, comma);
}

bool expectsAck(const String& name) {
    return name.startsWith("SET_") || name == "CAL_START";
}

// These three all take padIndex as their first argument and represent a
// single pad's live-readable fields (see PADVAL) -- on success, drumState
// should be resynced for that pad without the screen having to ask.
bool isPadFieldSetter(const String& name) {
    return name == "SET_NOTE" || name == "SET_THRESH" || name == "SET_CEILING_BASELINE";
}

// Extracts the field at `index` from a comma-separated argument string.
String fieldAt(const String& args, int index) {
    int start = 0;
    for (int i = 0; i < index; i++) {
        int next = args.indexOf(',', start);
        if (next < 0) return "";
        start = next + 1;
    }
    int end = args.indexOf(',', start);
    return end < 0 ? args.substring(start) : args.substring(start, end);
}

int intArg(const String& args, int index) {
    return fieldAt(args, index).toInt();
}

float floatArg(const String& args, int index) {
    return fieldAt(args, index).toFloat();
}

String stringArg(const String& args, int index) {
    String field = fieldAt(args, index);
    field.trim();
    return field;
}

// Appends "prefix + payload" to the on-screen event log, truncated to fit a
// log line. Also mirrors to USB Serial for host-side debugging.
void logUartEvent(const char* prefix, const String& payload) {
    char buf[EVENT_LOG_COLS];
    int i = 0;
    for (const char* p = prefix; *p && i < EVENT_LOG_COLS - 1; p++) buf[i++] = *p;
    for (const char* p = payload.c_str(); *p && i < EVENT_LOG_COLS - 1; p++) buf[i++] = *p;
    buf[i] = '\0';
    drumState.eventLog.add(buf);
    Serial.print(prefix);
    Serial.println(payload);
}

void handleHit(const String& args) {
    // HIT is an event stream -- log it for the MONITOR screen (raw event
    // state / hit monitoring screens come later). No persisted per-pad hit
    // fields yet.
    logUartEvent("HIT ", args);
}

void handlePadval(const String& args) {
    int padIndex = intArg(args, 0);
    if (padIndex < 0 || padIndex >= NUM_PADS) return;

    PadState& pad = drumState.pads[padIndex];
    pad.note = intArg(args, 1);
    pad.threshold = intArg(args, 2);
    pad.ceiling = intArg(args, 3);
    pad.curveExp = floatArg(args, 4);
    pad.lastUpdated = millis();
}

CalSensor parseCalSensor(const String& s) {
    if (s == "PIEZO") return CalSensor::PIEZO;
    if (s == "VELOSTAT") return CalSensor::VELOSTAT;
    return CalSensor::NONE;
}

CalPhase parseCalPhase(const String& s) {
    if (s == "RESTING") return CalPhase::RESTING;
    if (s == "WAITING_TOUCH") return CalPhase::WAITING_TOUCH;
    if (s == "CAPTURING") return CalPhase::CAPTURING;
    if (s == "PAD_DONE") return CalPhase::PAD_DONE;
    if (s == "COMPLETE") return CalPhase::COMPLETE;
    if (s == "ABORTED") return CalPhase::ABORTED;
    return CalPhase::NONE;
}

void handleCalState(const String& args) {
    CalPhase phase = parseCalPhase(stringArg(args, 1));
    bool hasPadIndex = (phase == CalPhase::CAPTURING || phase == CalPhase::PAD_DONE);

    drumState.calState.sensor = parseCalSensor(stringArg(args, 0));
    drumState.calState.phase = phase;
    drumState.calState.padIndex = hasPadIndex ? intArg(args, 2) : -1;
    drumState.calState.lastUpdated = millis();
}

void handleXtalkval(const String& args) {
    drumState.crosstalkRatio = floatArg(args, 0);
    drumState.crosstalkWindowMs = intArg(args, 1);
    drumState.xtalkLastUpdated = millis();
}

PlayMode parsePlayMode(const String& s) {
    if (s == "BOTH") return PlayMode::BOTH;
    if (s == "PIEZO_ONLY") return PlayMode::PIEZO_ONLY;
    if (s == "VELOSTAT_ONLY") return PlayMode::VELOSTAT_ONLY;
    return PlayMode::UNKNOWN;
}

void handlePlayMode(const String& args) {
    drumState.playMode = parsePlayMode(stringArg(args, 0));
    drumState.playModeLastUpdated = millis();
}

void handleAck(const String& args) {
    // ACK,<command> echoes back which command succeeded -- check it against
    // what's actually pending rather than trusting any ACK that arrives
    // while something's pending. Otherwise a late ACK for a since-timed-out
    // command could land after a *different* command has taken the pending
    // slot, and silently mark that unrelated command as successful.
    String ackedCommand = args;
    ackedCommand.trim();

    if (!pendingAck.active || ackedCommand != pendingAck.command) {
        Serial.print("Stray ACK: ");
        Serial.println(args);
        return;
    }

    bool needsPadSync = isPadFieldSetter(pendingAck.command);
    int padIndex = pendingAck.padIndex;

    pendingAck.active = false;
    lastResult = CommandResult::SUCCESS;
    logUartEvent("ACK ", ackedCommand);

    if (needsPadSync && padIndex >= 0) {
        // Plain query -- expectsAck() doesn't match GET_PAD, so this
        // doesn't touch the pending-ACK slot we just cleared above.
        sendCommand("GET_PAD," + String(padIndex));
    }
}

void handleErr(const String& args) {
    // ERR,<reason> does NOT echo the command name, unlike ACK. Correlation
    // relies on the single-pending-slot assumption documented on
    // sendCommand(): if something is pending, this ERR must be for it.
    if (!pendingAck.active) {
        Serial.print("Stray ERR: ");
        Serial.println(args);
        return;
    }
    pendingAck.active = false;
    lastResult = CommandResult::FAILED;
    lastFailureReason = args;
    logUartEvent("ERR ", args);
}

void handleIncomingLine(const String& line) {
    if (line.length() == 0) return;

    // Any received line proves the UART link is alive.
    drumState.lastContactMs = millis();

    int comma = line.indexOf(',');
    String cmd = commandName(line);
    String args = comma < 0 ? "" : line.substring(comma + 1);

    if (cmd == "HIT") handleHit(args);
    else if (cmd == "PADVAL") handlePadval(args);
    else if (cmd == "CAL_STATE") handleCalState(args);
    else if (cmd == "XTALKVAL") handleXtalkval(args);
    else if (cmd == "PLAY_MODE") handlePlayMode(args);
    else if (cmd == "ACK") handleAck(args);
    else if (cmd == "ERR") handleErr(args);
    else {
        logUartEvent("? ", line);
    }
}

void uartProtocolInit() {
    Serial1.begin(115200);
}

void uartProtocolUpdate() {
    static String line;

    while (Serial1.available()) {
        char c = Serial1.read();

        if (c == '\n') {
            line.trim(); // drop trailing \r
            handleIncomingLine(line);
            line = "";
        } else {
            line += c;
            if (line.length() > UART_LINE_MAX) line = ""; // discard garbage/noise line
        }
    }
}

void sendCommand(const String& cmd) {
    Serial1.print(cmd);
    Serial1.print('\n');

    String name = commandName(cmd);
    if (expectsAck(name)) {
        int comma = cmd.indexOf(',');
        String args = comma < 0 ? "" : cmd.substring(comma + 1);

        pendingAck.active = true;
        pendingAck.command = name;
        pendingAck.sentAt = millis();
        pendingAck.padIndex = isPadFieldSetter(name) ? intArg(args, 0) : -1;
        lastResult = CommandResult::PENDING;
    }
}

CommandResult getLastCommandResult() {
    if (pendingAck.active && millis() - pendingAck.sentAt > ACK_TIMEOUT_MS) {
        pendingAck.active = false;
        lastResult = CommandResult::TIMED_OUT;
    }
    return lastResult;
}

const String& getLastFailureReason() {
    return lastFailureReason;
}


// ============================================================================
// SECTION: Debug-dump helpers (from main.cpp)
// ============================================================================
const char* playModeStr(PlayMode m) {
    switch (m) {
        case PlayMode::BOTH: return "BOTH";
        case PlayMode::PIEZO_ONLY: return "PIEZO_ONLY";
        case PlayMode::VELOSTAT_ONLY: return "VELOSTAT_ONLY";
        default: return "UNKNOWN";
    }
}

const char* calSensorStr(CalSensor s) {
    switch (s) {
        case CalSensor::PIEZO: return "PIEZO";
        case CalSensor::VELOSTAT: return "VELOSTAT";
        default: return "NONE";
    }
}

const char* calPhaseStr(CalPhase p) {
    switch (p) {
        case CalPhase::RESTING: return "RESTING";
        case CalPhase::WAITING_TOUCH: return "WAITING_TOUCH";
        case CalPhase::CAPTURING: return "CAPTURING";
        case CalPhase::PAD_DONE: return "PAD_DONE";
        case CalPhase::COMPLETE: return "COMPLETE";
        case CalPhase::ABORTED: return "ABORTED";
        default: return "NONE";
    }
}

const char* commandResultStr(CommandResult r) {
    switch (r) {
        case CommandResult::PENDING: return "PENDING";
        case CommandResult::SUCCESS: return "SUCCESS";
        case CommandResult::FAILED: return "FAILED";
        case CommandResult::TIMED_OUT: return "TIMED_OUT";
        default: return "NONE";
    }
}

void printDrumStateDebug() {
    Serial.println("---- drumState ----");
    for (int i = 0; i < NUM_PADS; i++) {
        PadState& pad = drumState.pads[i];
        Serial.print("pad "); Serial.print(i);
        Serial.print(": note="); Serial.print(pad.note);
        Serial.print(" threshold="); Serial.print(pad.threshold);
        Serial.print(" ceiling="); Serial.print(pad.ceiling);
        Serial.print(" curveExp="); Serial.print(pad.curveExp);
        Serial.print(" lastUpdated="); Serial.println(pad.lastUpdated);
    }

    Serial.print("crosstalk: ratio="); Serial.print(drumState.crosstalkRatio);
    Serial.print(" windowMs="); Serial.print(drumState.crosstalkWindowMs);
    Serial.print(" lastUpdated="); Serial.println(drumState.xtalkLastUpdated);

    Serial.print("playMode: "); Serial.print(playModeStr(drumState.playMode));
    Serial.print(" lastUpdated="); Serial.println(drumState.playModeLastUpdated);

    Serial.print("calState: sensor="); Serial.print(calSensorStr(drumState.calState.sensor));
    Serial.print(" phase="); Serial.print(calPhaseStr(drumState.calState.phase));
    Serial.print(" padIndex="); Serial.print(drumState.calState.padIndex);
    Serial.print(" lastUpdated="); Serial.println(drumState.calState.lastUpdated);

    Serial.print("lastCommandResult: "); Serial.println(commandResultStr(getLastCommandResult()));
    Serial.print("lastFailureReason: "); Serial.println(getLastFailureReason());
    Serial.println("-------------------");
}



// ============================================================================
// SECTION: screens.h  (screen-manager interface)
// ============================================================================
// ScreenId is declared near the top of this file (see above) so the Arduino
// IDE's auto-generated prototypes can see it. The rest of the interface:
// Minimal screen manager: each screen is a case in switchScreen() (draws
// once, on entry) and updateScreen() (called every loop() iteration, for
// anything a screen needs to do on its own -- timers, animations, reading
// drumState). Screens don't own state beyond what a couple of statics in
// screens.cpp can hold; this is intentionally not a framework, just enough
// structure to keep adding screens from turning into a rewrite each time.

// Call once from setup(), after tft.init(). Takes ownership of drawing to
// `display` and shows the initial screen (SPLASH).
void screensInit(TFT_eSPI& display);

// Switches to `id` immediately: draws its initial content and resets its
// per-screen timer/state.
void switchScreen(ScreenId id);

// Call every loop() iteration. Lets the current screen do per-frame work
// (e.g. the splash screen's auto-advance timer).
void updateScreen();


// ============================================================================
// SECTION: screens.cpp  (screen-manager implementation)
// ============================================================================

// Splash has nothing real to wait on yet (no boot/init work gates it) --
// just a fixed minimum display time so it doesn't flash by unreadably
// fast, per UI_PLAN.md.
constexpr unsigned long SPLASH_MIN_DURATION_MS = 1800;

ScreenId currentScreen = ScreenId::SPLASH;
unsigned long screenEnteredAt = 0;

void drawCenteredLines(const char* line1, const char* line2, int textSize) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(textSize);
    tft.setTextDatum(MC_DATUM); // middle-center anchor

    int lineHeight = 8 * textSize + 6; // GLCD font is 8px tall before scaling
    int midX = tft.width() / 2;
    int midY = tft.height() / 2;

    if (line2 == nullptr) {
        tft.drawString(line1, midX, midY);
    } else {
        tft.drawString(line1, midX, midY - lineHeight / 2);
        tft.drawString(line2, midX, midY + lineHeight / 2);
    }

    tft.setTextDatum(TL_DATUM); // restore default anchor
}

void drawSplash() {
    drawCenteredLines("Initializing", "Jaiba Hexa Drum UI", 2);
}

// ----------------------------------------------------------------------------
// Phase 1 screen layout: LANDING (hex grid + footer) and MONITOR (link
// status / event log / per-pad readback). See Docs/UI_ROADMAP.md.
// ----------------------------------------------------------------------------
constexpr int FOOTER_H = 42;                    // bottom footer bar height
constexpr int MON_BTN_W = 108;
constexpr int MON_BTN_H = 28;
constexpr int SIDE_BTN_W = 40;
constexpr unsigned long LINK_TIMEOUT_MS = 3000;  // no msg for this long => OFFLINE
constexpr unsigned long QUERY_INTERVAL_MS = 2000; // GET_PAD refresh cadence

int footerY()   { return tft.height() - FOOTER_H; }
int monitorBtnX() { return tft.width() - MON_BTN_W - 8; }

bool pointInRect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

void drawButtonFrame(int x, int y, int w, int h, bool active) {
    uint16_t fill = active ? TFT_DARKCYAN : TFT_DARKGREY;
    tft.fillRoundRect(x, y, w, h, 4, fill);
    tft.drawRoundRect(x, y, w, h, 4, TFT_WHITE);
}

// --- LANDING ----------------------------------------------------------------
// Which hemisphere the landing shows. RIGHT = the wired plate (sensor
// Teensy 1, pads 1-6). LEFT = mock layout for the future sensor Teensy 2.
bool landingSideRight = true;

void drawLandingPlaceholder() {
    tft.fillScreen(TFT_BLACK);

    // Select and draw the active hemisphere's hex grid above the footer.
    hexSelectSide(landingSideRight);
    drawHexGrid(tft, 0, 0, tft.width(), tft.height() - FOOTER_H);

    // Footer: [L][R] side toggle + side caption  ...  [Monitor]
    int fy = footerY();
    int by = fy + (FOOTER_H - MON_BTN_H) / 2;
    tft.drawFastHLine(0, fy, tft.width(), TFT_DARKGREY);

    drawButtonFrame(8, by, SIDE_BTN_W, MON_BTN_H, !landingSideRight);
    tft.setTextColor(landingSideRight ? TFT_DARKGREY : TFT_WHITE, landingSideRight ? TFT_BLACK : TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString("L", 8 + SIDE_BTN_W / 2, by + MON_BTN_H / 2);

    drawButtonFrame(8 + SIDE_BTN_W + 6, by, SIDE_BTN_W, MON_BTN_H, landingSideRight);
    tft.setTextColor(landingSideRight ? TFT_WHITE : TFT_DARKGREY, landingSideRight ? TFT_DARKCYAN : TFT_BLACK);
    tft.drawString("R", 8 + SIDE_BTN_W + 6 + SIDE_BTN_W / 2, by + MON_BTN_H / 2);

    tft.setTextSize(1);
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(landingSideRight ? "SENSOR 1  (6 pads)" : "SENSOR 2  (mock)", 8 + 2 * (SIDE_BTN_W + 6), by + 8);

    int bx = monitorBtnX();
    drawButtonFrame(bx, by, MON_BTN_W, MON_BTN_H, true);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString("Monitor", bx + MON_BTN_W / 2, by + MON_BTN_H / 2);
    tft.setTextDatum(TL_DATUM);
}

void handleLandingTap(int x, int y) {
    if (y >= footerY()) {
        int by = footerY() + (FOOTER_H - MON_BTN_H) / 2;
        if (pointInRect(x, y, 8, by, SIDE_BTN_W, MON_BTN_H)) {          // tap L
            if (landingSideRight) { landingSideRight = false; switchScreen(ScreenId::LANDING_PLACEHOLDER); }
            return;
        }
        if (pointInRect(x, y, 8 + SIDE_BTN_W + 6, by, SIDE_BTN_W, MON_BTN_H)) {  // tap R
            if (!landingSideRight) { landingSideRight = true; switchScreen(ScreenId::LANDING_PLACEHOLDER); }
            return;
        }
        if (pointInRect(x, y, monitorBtnX(), by, MON_BTN_W, MON_BTN_H)) {
            switchScreen(ScreenId::MONITOR);
        }
        return;
    }
    // Tapping a populated hex tile opens that pad's tuning panel.
    int tile = hexGridHitTest(x, y);
    if (tile >= 0) {
        const HexTile& t = activeTiles[tile];
        if (t.populated && t.sensorIndex >= 0) {
            openPadPanel(t.sensorIndex);
        }
    }
}

// --- MONITOR ----------------------------------------------------------------
// The monitor only repaints when something it displays actually changed
// (link status, a pad's PADVAL, or the event log) -- no fixed full-screen
// refresh timer, so a static screen doesn't blink. Repainting does NOT
// affect the sensor: it only costs time on this UI Teensy, and the UART is
// read at the top of every loop() regardless.
unsigned long lastQueryMs = 0;

// Snapshots of what the last drawMonitor() painted.
int lastLinkStage = -1;                 // 0 waiting, 1 online, 2 offline
unsigned long lastPadUpdateSeen[NUM_PADS] = { 0 };
int lastLogCount = 0;
int lastLogHead = 0;

int linkStage() {
    if (drumState.lastContactMs == 0) return 0;
    return (millis() - drumState.lastContactMs < LINK_TIMEOUT_MS) ? 1 : 2;
}

bool monitorNeedsRepaint() {
    if (lastLinkStage != linkStage()) return true;
    for (int i = 0; i < NUM_PADS; i++) {
        if (drumState.pads[i].lastUpdated != lastPadUpdateSeen[i]) return true;
    }
    if (drumState.eventLog.count != lastLogCount ||
        drumState.eventLog.head != lastLogHead) return true;
    return false;
}

void monitorStampSnapshots() {
    lastLinkStage = linkStage();
    for (int i = 0; i < NUM_PADS; i++) lastPadUpdateSeen[i] = drumState.pads[i].lastUpdated;
    lastLogCount = drumState.eventLog.count;
    lastLogHead = drumState.eventLog.head;
}

const char* linkStatusText(uint16_t& color) {
    if (drumState.lastContactMs == 0) { color = TFT_YELLOW;  return "waiting"; }
    if (millis() - drumState.lastContactMs < LINK_TIMEOUT_MS) { color = TFT_GREEN; return "ONLINE"; }
    color = TFT_RED;
    return "OFFLINE";
}

void drawMonitor() {
    tft.fillScreen(TFT_BLACK);

    // Header: "<-- Back" button + title + live link status.
    drawButtonFrame(6, 6, 60, MON_BTN_H, true);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString("<--", 36, 6 + MON_BTN_H / 2);
    tft.setTextDatum(TL_DATUM);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Monitor", 76, 8);

    uint16_t stColor;
    const char* stText = linkStatusText(stColor);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("link:", tft.width() - 120, 13);
    tft.setTextColor(stColor, TFT_BLACK);
    tft.drawString(stText, tft.width() - 86, 13);

    // Per-pad readback (values arrive via GET_PAD -> PADVAL -> drumState).
    int y = 42;
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    tft.drawString(" pad   note   thr    ceil   curve", 6, y);
    y += 13;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char buf[48];
    for (int i = 0; i < NUM_PADS; i++) {
        const PadState& p = drumState.pads[i];
        if (p.lastUpdated == 0) {
            snprintf(buf, sizeof buf, " %d     --     --     --     --   (no data)", i + 1);
        } else {
            snprintf(buf, sizeof buf, " %d    %3d    %3d    %4d   %4.2f",
                     i + 1, p.note, p.threshold, p.ceiling, (double)p.curveExp);
        }
        tft.drawString(buf, 6, y);
        y += 13;
    }

    // Event log (HIT / ACK / ERR ...).
    y += 4;
    tft.drawFastHLine(0, y - 2, tft.width(), TFT_DARKGREY);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Events:", 6, y + 2);
    y += 15;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    int n = drumState.eventLog.count;
    for (int r = 0; r < n; r++) {
        tft.drawString(drumState.eventLog.get(r), 10, y);
        y += 12;
    }
    tft.setTextDatum(TL_DATUM);
}

// While MONITOR is open, periodically ask the drum Teensy for fresh per-pad
// values. GET_* queries do not use the single pending-ACK slot, so firing
// several back-to-back is safe.
void maybeQueryPads() {
    if (lastQueryMs == 0 || millis() - lastQueryMs >= QUERY_INTERVAL_MS) {
        lastQueryMs = millis();
        for (int i = 0; i < NUM_PADS; i++) {
            sendCommand("GET_PAD," + String(i));
        }
    }
}

// ----------------------------------------------------------------------------
// PAD DETAIL -- per-pad tuning panel (Phase 2)
// ----------------------------------------------------------------------------
// Opened by tapping a populated hex on the landing. Shows the pad's live
// values (from GET_PAD/PADVAL) and sends SET_NOTE / SET_THRESH /
// SET_CEILING_BASELINE for the +/- buttons. Only one SET_*/CAL_START may be
// in flight (single pending-ACK slot), so sends are skipped while
// getLastCommandResult() == PENDING and the status line reflects the result.

constexpr int PP_ROW_Y0 = 68;     // first editable row y
constexpr int PP_ROW_H  = 44;     // row pitch
constexpr int PP_VAL_XR = 186;    // value text right edge (datum MR) -- clear of buttons
constexpr int PP_BTN_X  = 198;    // minus button x
constexpr int PP_BTN_W  = 32;

int padPanelSensorIndex = -1;      // which pad (0..NUM_PADS-1) the panel edits
unsigned long ppLastQueryMs = 0;
unsigned long ppPadUpdateSeen = 0;
int ppCmdResultSeen = -1;

void openPadPanel(int sensorIndex) {
    padPanelSensorIndex = sensorIndex;
    switchScreen(ScreenId::PAD_DETAIL);
}

int ppValueAt(int r) {
    const PadState& p = drumState.pads[padPanelSensorIndex];
    return (r == 0) ? p.note : (r == 1) ? p.threshold : p.ceiling;
}

int ppStepAt(int r) {
    return (r == 0) ? 1 : (r == 1) ? 5 : 50;
}

int ppClamp(int r, int v) {
    int lo = (r == 0) ? 0 : 1;       // note 0..127; threshold/ceiling >= 1
    int hi = (r == 0) ? 127 : 4095;  // 12-bit ADC ceiling
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

const char* ppCmdAt(int r) {
    return (r == 0) ? "SET_NOTE" : (r == 1) ? "SET_THRESH" : "SET_CEILING_BASELINE";
}

const char* ppLabelAt(int r) {
    return (r == 0) ? "Note" : (r == 1) ? "Thresh" : "Ceiling";
}

void ppSendField(int r) {
    if (padPanelSensorIndex < 0) return;
    if (getLastCommandResult() == CommandResult::PENDING) return;  // one at a time
    int v = ppValueAt(r);
    sendCommand(String(ppCmdAt(r)) + "," + String(padPanelSensorIndex) + "," + String(v));
}

void drawPadPanel() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    // Header: "<-- Back" + "Pad N".
    drawButtonFrame(6, 6, 60, MON_BTN_H, true);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2);
    tft.drawString("<--", 36, 6 + MON_BTN_H / 2);
    tft.setTextDatum(TL_DATUM);

    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    char title[24];
    snprintf(title, sizeof title, "Pad %d", padPanelSensorIndex + 1);
    tft.drawString(title, 76, 8);

    // Freshness + editable-row labels.
    const PadState& p = drumState.pads[padPanelSensorIndex];
    tft.setTextSize(1);
    tft.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    if (p.lastUpdated == 0) tft.drawString("waiting for sensor data...", 76, 30);
    else                    tft.drawString("live (GET_PAD every 2s)", 76, 30);

    char buf[24];
    for (int r = 0; r < 3; r++) {
        int ry = PP_ROW_Y0 + r * PP_ROW_H;
        tft.setTextSize(2);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(ppLabelAt(r), 12, ry + 12);

        // Value right-aligned in the band between the label and the [-] [+]
        // buttons, so 4-digit ADC values (e.g. 0823) never run under either.
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        if (p.lastUpdated == 0) tft.drawString("--", PP_VAL_XR, ry + 12);
        else { snprintf(buf, sizeof buf, "%d", ppValueAt(r)); tft.drawString(buf, PP_VAL_XR, ry + 12); }

        // [-] [+] buttons
        drawButtonFrame(PP_BTN_X, ry, PP_BTN_W, PP_ROW_H - 12, true);
        tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
        tft.setTextSize(3);
        tft.drawString("-", PP_BTN_X + PP_BTN_W / 2, ry + (PP_ROW_H - 12) / 2);
        drawButtonFrame(PP_BTN_X + PP_BTN_W + 6, ry, PP_BTN_W, PP_ROW_H - 12, true);
        tft.drawString("+", PP_BTN_X + PP_BTN_W + 6 + PP_BTN_W / 2, ry + (PP_ROW_H - 12) / 2);
        tft.setTextDatum(TL_DATUM);
    }

    // Status line: last SET_* result.
    int sy = PP_ROW_Y0 + 3 * PP_ROW_H + 8;
    tft.setTextSize(1);
    CommandResult res = getLastCommandResult();
    if (res == CommandResult::PENDING) {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("sending... (waiting for ACK)", 12, sy);
    } else if (res == CommandResult::SUCCESS) {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("last change: applied", 12, sy);
    } else if (res == CommandResult::FAILED) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        String fail = String("ERR: ") + getLastFailureReason();
        tft.drawString(fail.c_str(), 12, sy);
    } else if (res == CommandResult::TIMED_OUT) {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("last change: timed out", 12, sy);
    } else {
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        tft.drawString("tap +/- to adjust this pad", 12, sy);
    }
    tft.setTextDatum(TL_DATUM);

    // Reference ranges for the +/- rows (handy while tuning).
    int ry2 = sy + 24;
    tft.setTextSize(2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Note     +/-1   0 - 127", 12, ry2);        ry2 += 18;
    tft.drawString("Thresh   +/-5   1 - 4095", 12, ry2);       ry2 += 18;
    tft.drawString("Ceiling  +/-50  1 - 4095", 12, ry2);
    tft.setTextDatum(TL_DATUM);
}

bool padPanelNeedsRepaint() {
    const PadState& p = drumState.pads[padPanelSensorIndex];
    if (p.lastUpdated != ppPadUpdateSeen) return true;
    if ((int)getLastCommandResult() != ppCmdResultSeen) return true;
    return false;
}

void padPanelStamp() {
    ppPadUpdateSeen = drumState.pads[padPanelSensorIndex].lastUpdated;
    ppCmdResultSeen = (int)getLastCommandResult();
}

void handlePadPanelTap(int x, int y) {
    // Back button.
    if (pointInRect(x, y, 6, 6, 60, MON_BTN_H)) {
        switchScreen(ScreenId::LANDING_PLACEHOLDER);
        return;
    }
    if (padPanelSensorIndex < 0) return;
    if (drumState.pads[padPanelSensorIndex].lastUpdated == 0) return; // no data yet

    for (int r = 0; r < 3; r++) {
        int ry = PP_ROW_Y0 + r * PP_ROW_H;
        bool minus = pointInRect(x, y, PP_BTN_X, ry, PP_BTN_W, PP_ROW_H - 12);
        bool plus  = pointInRect(x, y, PP_BTN_X + PP_BTN_W + 6, ry, PP_BTN_W, PP_ROW_H - 12);
        if (minus || plus) {
            int delta = plus ? ppStepAt(r) : -ppStepAt(r);
            PadState& p = drumState.pads[padPanelSensorIndex];
            int nv = ppClamp(r, ppValueAt(r) + delta);
            bool changed = (nv != ppValueAt(r));
            if (r == 0) p.note = nv; else if (r == 1) p.threshold = nv; else p.ceiling = nv;
            if (changed) {
                ppSendField(r);       // one SET_* in flight; skips if pending
                padPanelStamp();
                drawPadPanel();       // immediate visual feedback
            }
            return;
        }
    }
}

// Release-tap dispatch for the active screen (called by the touch layer).
void screensHandleRelease(int x, int y) {
    switch (currentScreen) {
        case ScreenId::LANDING_PLACEHOLDER: handleLandingTap(x, y); break;
        case ScreenId::PAD_DETAIL: handlePadPanelTap(x, y); break;
        case ScreenId::MONITOR:
            if (pointInRect(x, y, 6, 6, 60, MON_BTN_H)) {
                switchScreen(ScreenId::LANDING_PLACEHOLDER);
            }
            break;
        default: break;
    }
}

void updateSplash() {
    if (millis() - screenEnteredAt >= SPLASH_MIN_DURATION_MS) {
        switchScreen(ScreenId::LANDING_PLACEHOLDER);
    }
}

void screensInit(TFT_eSPI& display) {
    (void)display;  // drawing targets the global TFT_eSPI instance `tft` in this sketch
    switchScreen(ScreenId::SPLASH);
}

void switchScreen(ScreenId id) {
    currentScreen = id;
    screenEnteredAt = millis();

    switch (id) {
        case ScreenId::SPLASH: drawSplash(); break;
        case ScreenId::LANDING_PLACEHOLDER: drawLandingPlaceholder(); break;
        case ScreenId::MONITOR:
            drawMonitor();
            monitorStampSnapshots();
            lastQueryMs = 0;   // fire the first GET_PAD batch immediately
            break;
        case ScreenId::PAD_DETAIL:
            drawPadPanel();
            padPanelStamp();
            ppLastQueryMs = 0; // fetch this pad's values immediately
            break;
    }
}

void updateScreen() {
    switch (currentScreen) {
        case ScreenId::SPLASH: updateSplash(); break;
        case ScreenId::LANDING_PLACEHOLDER: break; // static; redrawn on entry
        case ScreenId::MONITOR:
            maybeQueryPads();
            if (monitorNeedsRepaint()) {
                monitorStampSnapshots();
                drawMonitor();
            }
            break;
        case ScreenId::PAD_DETAIL:
            if (padPanelSensorIndex >= 0) {
                // Refresh this pad's live values while the panel is open.
                if (ppLastQueryMs == 0 || millis() - ppLastQueryMs >= QUERY_INTERVAL_MS) {
                    ppLastQueryMs = millis();
                    sendCommand("GET_PAD," + String(padPanelSensorIndex));
                }
                if (padPanelNeedsRepaint()) {
                    padPanelStamp();
                    drawPadPanel();
                }
            }
            break;
    }
}


// ============================================================================
// SECTION: XPT2046 touch  (restored from the phase-1 hardware bring-up code —
// that code lived in the original repo's initial commit, src/main.cpp, and was
// later overwritten by throwaway UART-test screens)
// ============================================================================
//
// The XPT2046 shares the display's SPI bus and only needs its own CS line
// (pin 6, see tft_setup.h). Touch is polled manually here — no touch library:
// each read takes a burst of TOUCH_SAMPLES raw conversions and returns the
// median, which is robust to resistive-panel jitter and to a single spurious
// glitch. TFT_eSPI's own built-in touch support (compiled in because TOUCH_CS
// is defined in tft_setup.h) is available but NOT used by this code.
//
// Calibration: these constants were found during bring-up by touching the four
// physical screen corners and reading raw X/Y from Serial Monitor. If the
// display is ever swapped, or coordinates look inverted/off, re-derive them —
// they do not transfer to different hardware (see Claude.md in the original
// repo).

// Oversampling: 7 samples * ~500us settling between them adds ~3ms per read —
// well under perceptible touch lag.
#define TOUCH_SAMPLES 7
#define TOUCH_SETTLE_US 500

// Raw XPT2046 ADC readings at the physical screen corners.
#define CALIBRATION_X_MIN 1840
#define CALIBRATION_X_MAX 31480
#define CALIBRATION_Y_MIN 31104
#define CALIBRATION_Y_MAX 1824

// Returns true (with median-filtered raw coordinates) while the panel is being
// touched; returns false when released (idle reads are ~0).
bool readTouchData(uint16_t &x, uint16_t &y) {
    uint16_t xSamples[TOUCH_SAMPLES];
    uint16_t ySamples[TOUCH_SAMPLES];

    digitalWrite(TOUCH_CS, LOW);
    SPI.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));

    for (uint8_t i = 0; i < TOUCH_SAMPLES; i++) {
        SPI.transfer(0x90);              // command: read Y
        ySamples[i] = SPI.transfer16(0x0000);
        SPI.transfer(0xD0);              // command: read X
        xSamples[i] = SPI.transfer16(0x0000);
        if (i < TOUCH_SAMPLES - 1) delayMicroseconds(TOUCH_SETTLE_US);
    }

    SPI.endTransaction();
    digitalWrite(TOUCH_CS, HIGH);

    x = medianOfSamples(xSamples, TOUCH_SAMPLES);
    y = medianOfSamples(ySamples, TOUCH_SAMPLES);

    if (x > 100 && y > 100) {
        return true;
    }
    return false;
}

uint16_t medianOfSamples(uint16_t *samples, uint8_t count) {
    // Insertion sort — count is small (single-digit), so this is cheap.
    for (uint8_t i = 1; i < count; i++) {
        uint16_t key = samples[i];
        int8_t j = i - 1;
        while (j >= 0 && samples[j] > key) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = key;
    }
    return samples[count / 2];
}

// --- Touch -> screen glue ---------------------------------------------------
// Polled from loop() at a ~15ms cadence. Tracks a press and, on release,
// hands the mapped coordinates to the active screen (screensHandleRelease)
// so each screen decides what the tap means. Prints taps to USB Serial for
// development feedback.
bool touchWasPressed = false;
int lastPressX = -1, lastPressY = -1;
unsigned long lastTouchPollMs = 0;

void handleTouch() {
    const unsigned long TOUCH_POLL_MS = 15;
    if (millis() - lastTouchPollMs < TOUCH_POLL_MS) return;
    lastTouchPollMs = millis();

    uint16_t rawX, rawY;
    if (readTouchData(rawX, rawY)) {
        uint16_t mappedX = (uint16_t)map(rawX, CALIBRATION_X_MIN, CALIBRATION_X_MAX, 0, tft.width());
        uint16_t mappedY = (uint16_t)map(rawY, CALIBRATION_Y_MIN, CALIBRATION_Y_MAX, 0, tft.height());
        mappedX = constrain(mappedX, 0, (uint16_t)(tft.width() - 1));
        mappedY = constrain(mappedY, 0, (uint16_t)(tft.height() - 1));

        lastPressX = mappedX;
        lastPressY = mappedY;
        touchWasPressed = true;
    } else if (touchWasPressed) {
        // Finger lifted: this is a tap at the last press position.
        touchWasPressed = false;
        Serial.print("tap ");
        Serial.print(lastPressX); Serial.print(','); Serial.println(lastPressY);
        screensHandleRelease(lastPressX, lastPressY);
        lastPressX = lastPressY = -1;
    }
}


// ============================================================================
// SECTION: setup() / loop() (from main.cpp)
// ============================================================================
void setup() {
    Serial.begin(9600);
    while (!Serial && millis() < 2000); // don't hang forever if no monitor is attached

    uartProtocolInit();

    tft.init();
    tft.setRotation(0);
    screensInit(tft);

    // Touch controller CS (XPT2046) — shares the display SPI bus, own CS.
    pinMode(TOUCH_CS, OUTPUT);
    digitalWrite(TOUCH_CS, HIGH); // deselected by default
}

void loop() {
    uartProtocolUpdate();
    handleTouch();   // XPT2046 poll (~15ms cadence) + tap handling
    updateScreen();

    // Test-command passthrough: forward lines typed into the USB Serial
    // Monitor to the drum Teensy over Serial1, for manual protocol testing
    // (e.g. CAL_START,PIEZO,SINGLE,2) ahead of real screens sending these.
    static String cmdLine;

    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\n') {
            cmdLine.trim(); // drop trailing \r and stray whitespace

            if (cmdLine == "DUMP") {
                printDrumStateDebug();
            } else if (cmdLine.length() > 0) {
                sendCommand(cmdLine);

                Serial.print("Serial1 >> ");
                Serial.println(cmdLine);
            }

            cmdLine = "";
        } else {
            cmdLine += c;
            if (cmdLine.length() > UART_LINE_MAX) cmdLine = ""; // discard garbage/noise line
        }
    }
}
