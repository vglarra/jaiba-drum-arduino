// ============================================================================
// tft_setup.h — TFT_eSPI compile-time configuration for UIFirmware_Arduino.ino
//
// TFT_eSPI is configured entirely with preprocessor defines at compile time.
// This file is the single source of truth for that configuration. It is the
// exact equivalent of the `build_flags` section of the original UI project's
// platformio.ini (every define below maps 1:1 to one -D flag there).
//
// HOW THE CONFIG REACHES THE COMPILER — two supported routes:
//
//   ROUTE 1 (Arduino IDE — guaranteed to work): open the installed TFT_eSPI
//   library and replace the contents of its `User_Setup.h` with the contents
//   of THIS file (keep only the top-of-file comment). TFT_eSPI always reads
//   User_Setup.h when no other setup is loaded, so the whole library is then
//   compiled with exactly this configuration. Typical User_Setup.h locations:
//     Windows: %LOCALAPPDATA%\Arduino15\packages\... (or Documents\Arduino\libraries\TFT_eSPI\User_Setup.h)
//     macOS:   ~/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
//     Linux:   ~/Arduino/libraries/TFT_eSPI/User_Setup.h
//   (Library Manager installs TFT_eSPI under the sketchbook's libraries folder.)
//
//   ROUTE 2 (auto-detected sketch setup): TFT_eSPI.h checks
//       #if __has_include(<tft_setup.h>)  ->  #include <tft_setup.h>
//   and uses this file INSTEAD of User_Setup.h — but only when the build puts
//   the sketch folder on the include search path. PlatformIO and some Arduino
//   IDE versions do this; the arduino-cli/IDE-2.x Teensy toolchain used to
//   verify this conversion does NOT, so do not rely on Route 2 alone. Keeping
//   this file in the sketch folder is still useful: it documents the config
//   and is the exact text to paste for Route 1.
//
// HARDWARE (display/touch combo board, silkscreen "X320 V1.1", 3.3V logic):
//   Display ST7796 (320x480, SPI): SCK=13, MOSI=11, MISO=12, CS=10, DC=8, RST=9
//   Touch XPT2046:                  shares SCK/MOSI/MISO, CS=6, IRQ=7 (unused)
// ============================================================================

#ifndef TFT_SETUP_H
#define TFT_SETUP_H

// ---- Display driver --------------------------------------------------------
#define ST7796_DRIVER 1
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// ---- Display SPI pins (Teensy 4.1) ----------------------------------------
#define TFT_MISO 12
#define TFT_MOSI 11
#define TFT_SCLK 13
#define TFT_CS   10
#define TFT_DC   8
#define TFT_RST  9

// ---- Touch controller (XPT2046) — shares the display SPI bus, own CS ------
// Defining TOUCH_CS also compiles TFT_eSPI's built-in touch support; this
// sketch polls the XPT2046 manually (see the touch section in the .ino), so
// the built-in touch API is unused but harmless.
#define TOUCH_CS 6

// ---- Fonts (each LOAD_FONT adds to flash size) -----------------------------
#define LOAD_GLCD   1
#define LOAD_FONT2  1
#define LOAD_FONT4  1
#define LOAD_FONT6  1
#define LOAD_FONT7  1
#define LOAD_FONT8  1
#define LOAD_GFXFF  1
#define SMOOTH_FONT 1

// ---- SPI speeds ------------------------------------------------------------
// Display can run fast; the XPT2046 is limited to ~2.5MHz.
#define SPI_FREQUENCY       40000000
#define SPI_TOUCH_FREQUENCY 2500000

#endif // TFT_SETUP_H
