// ============================================================================
// platform.h — target and board scaffolding shared by every sketch in this repo
// ============================================================================
// Supplies the three things that are identical across sketches and easy to get
// subtly wrong: the SDL/Arduino shims, the panel configuration (per board, via
// boards/*.h), and the full-frame sprite everything draws into.
//
// A sketch includes this first, optionally after defining:
//
//   SKETCH_TITLE    SDL window title                          (default "sketch")
//   SKETCH_FRAMES   frames to capture in headless mode        (default 1)
//
// It then gets `W`, `H`, `lcd`, `cv`, `panelBegin()`, `present()`, and a main
// entry point — so the sketch itself only has to define setup() and loop().
//
// Three build shapes come out of the same source:
//
//   pio run -e <name>          Arduino/ESP32 or SDL, depending on the env
//   pio run -e <name>_shot     headless: renders, writes PNGs, exits
//
// The headless target exists because an SDL window can't be inspected by
// anything but a human. It allocates the sprite, skips lcd.init() entirely,
// and turns present() into a PNG write — so a port can be checked by looking
// at the frames it produces.
// ============================================================================
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include <stdint.h>

#ifndef SKETCH_TITLE
  #define SKETCH_TITLE "sketch"
#endif
#ifndef SKETCH_FRAMES
  #define SKETCH_FRAMES 1
#endif

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------
// The board header supplies W/H, the device LGFX panel class, FB_IN_PSRAM,
// and the board's pins (PIN_BOOT, PIN_IMU_*). Selected by a -DBOARD_* build
// flag from platformio.ini; an unflagged build gets the 1.47B, so plain
// includes and IDE indexers keep working.
#if defined(BOARD_AMOLED_18)
  #include "boards/amoled_18.h"
#elif defined(BOARD_TOUCH_LCD_2)
  #include "boards/touch_lcd_2.h"
#elif defined(BOARD_ROTARY_128)
  #include "boards/rotary_128.h"
#else
  #include "boards/lcd_147b.h"
#endif

// ---------------------------------------------------------------------------
// Platform shims
// ---------------------------------------------------------------------------
#if defined(ARDUINO)

  #include <Arduino.h>
  #include <esp_heap_caps.h>
  #include <Preferences.h>

#else

  #include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <ctime>

  static uint32_t esp_random() {
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
  }

  struct SerialShim {
    void begin(int) {}
    template <typename... A> void printf(const char *f, A... a) { std::printf(f, a...); }
    void println(const char *s = "") { std::printf("%s\n", s); }
  };
  static SerialShim Serial;

  #if defined(SKETCH_HEADLESS)
    // A virtual clock. Headless runs must not spend real seconds waiting out a
    // sketch's HOLD_MS, and frame captures should be reproducible, so time only
    // moves when the sketch asks it to (plus a fixed tick per loop, so a sketch
    // that never calls delay() still advances).
    static uint32_t _vclock = 0;
    static inline uint32_t millis() { return _vclock; }
    static inline void     delay(uint32_t ms) { _vclock += ms; }
  #else
    using lgfx::millis;    // LovyanGFX provides these for SDL (sdl/common.hpp)
    using lgfx::delay;
  #endif

#endif

// Real elapsed microseconds, for measuring how long a frame took.
//
// Deliberately separate from millis(): the headless build replaces that with a
// virtual clock, so it cannot time anything. Sketches should derive a playhead
// from millis() (so captures step deterministically) and time themselves with
// wallMicros().
#if defined(ARDUINO)
  static inline uint32_t wallMicros() { return micros(); }
#elif defined(SKETCH_HEADLESS)
  #include <time.h>
  static inline uint32_t wallMicros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull);
  }
#else
  static inline uint32_t wallMicros() { return lgfx::micros(); }
#endif

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
// The device build's LGFX class comes from the board header above. The SDL
// window is the same for every board — a W x H canvas at 2x — so its class
// lives here.
#if !defined(ARDUINO)

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_sdl _panel;
public:
  LGFX(void) {
    { auto c = _panel.config();
      c.memory_width = W; c.panel_width  = W;
      c.memory_height = H; c.panel_height = H;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.bus_shared = false;
      _panel.config(c);
      _panel.setScaling(2, 2);
      _panel.setWindowTitle(SKETCH_TITLE);
    }
    setPanel(&_panel);
  }
};

#endif

static LGFX        lcd;
static LGFX_Sprite cv(&lcd);

// ---------------------------------------------------------------------------
// Headless capture — a self-contained PNG writer
// ---------------------------------------------------------------------------
// Uses stored (uncompressed) deflate blocks, so there's no zlib dependency and
// no image tooling in the loop: the file lands on disk ready to open.
#if !defined(ARDUINO) && defined(SKETCH_HEADLESS)

static const char *_shotDir = ".";
static int         _frameNo = 0;

static uint32_t _crcTable[256];
static bool     _crcReady = false;

static uint32_t _crc32(const uint8_t *p, size_t n, uint32_t crc) {
  if (!_crcReady) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
      _crcTable[i] = c;
    }
    _crcReady = true;
  }
  for (size_t i = 0; i < n; i++) crc = _crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

static void _be32(uint8_t *d, uint32_t v) { d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = (uint8_t)v; }

static void _pngChunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t hdr[8];
  _be32(hdr, n);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t crc = _crc32((const uint8_t *)type, 4, 0xFFFFFFFFu);
  crc = _crc32(data, n, crc) ^ 0xFFFFFFFFu;
  uint8_t tail[4];
  _be32(tail, crc);
  fwrite(tail, 1, 4, f);
}

static const size_t _RAW_N = (size_t)H * (1 + 3 * W);
static uint8_t      _rawBuf[_RAW_N];
// zlib header + a 5-byte stored-block header per 64KB chunk + the adler32.
static uint8_t      _zBuf[_RAW_N + (_RAW_N / 65535 + 1) * 5 + 64];

static bool writeFramePng(const char *path) {
  RGBColor row[W];
  size_t o = 0;
  for (int y = 0; y < H; y++) {
    _rawBuf[o++] = 0;                                  // PNG filter type: none
    cv.readRectRGB(0, y, W, 1, row);
    for (int x = 0; x < W; x++) {
      _rawBuf[o++] = row[x].r;
      _rawBuf[o++] = row[x].g;
      _rawBuf[o++] = row[x].b;
    }
  }

  FILE *f = fopen(path, "wb");
  if (!f) return false;

  static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);

  uint8_t ihdr[13];
  _be32(ihdr, (uint32_t)W);
  _be32(ihdr + 4, (uint32_t)H);
  ihdr[8] = 8;      // bit depth
  ihdr[9] = 2;      // colour type: truecolour
  ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  _pngChunk(f, "IHDR", ihdr, 13);

  size_t zn = 0;
  _zBuf[zn++] = 0x78; _zBuf[zn++] = 0x01;              // zlib header
  size_t left = o, off = 0;
  do {
    uint16_t n = left > 65535 ? 65535 : (uint16_t)left;
    uint16_t nlen = (uint16_t)~n;
    _zBuf[zn++] = (n == left) ? 1 : 0;                 // BFINAL, BTYPE=stored
    _zBuf[zn++] = n & 0xFF;    _zBuf[zn++] = n >> 8;
    _zBuf[zn++] = nlen & 0xFF; _zBuf[zn++] = nlen >> 8;
    memcpy(_zBuf + zn, _rawBuf + off, n);
    zn += n; off += n; left -= n;
  } while (left);

  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < o; i++) { a = (a + _rawBuf[i]) % 65521; b = (b + a) % 65521; }
  _be32(_zBuf + zn, (b << 16) | a);
  zn += 4;

  _pngChunk(f, "IDAT", _zBuf, (uint32_t)zn);
  _pngChunk(f, "IEND", nullptr, 0);
  fclose(f);
  return true;
}

#endif  // headless

// ---------------------------------------------------------------------------
// The two buttons
// ---------------------------------------------------------------------------
// The board's controls divide cleanly: BOOT changes *which* sketch is running,
// RESET changes *what that sketch drew*. No sketch regenerates on a timer, so
// between presses a composition holds for as long as you leave it.
//
// BOOT is GPIO0, which idles high through its pull-up and shorts to ground when
// pressed — so a press is a falling edge. buttonPressed() reports one event per
// press, not one per poll, so holding the button does not machine-gun.
//
// RESET has no entry point here and cannot have one: it is wired to the chip's
// EN line, so software never sees the press — the S3 simply reboots and every
// sketch's setup() draws a fresh composition from newSeed(). That reboot is
// also why the switcher's place in the roster lives in flash (persistGet
// below); RTC memory is powered from the same domain EN drops.
//
// On desktop there is no BOOT pin, so a click in the bottom strip of the window
// stands in. That strip is not the whole window on purpose: imu.h already reads
// a click *anywhere* as a shake, and a binary hosting both needs the two
// gestures to stay tellable apart. There is no RESET stand-in — relaunch the
// binary, which reseeds from the clock (see main() at the foot of this file).
static const uint32_t BUTTON_DEBOUNCE_MS = 200;
static const int      BUTTON_SDL_STRIP_H = 48;

static inline void buttonBegin() {
#if defined(ARDUINO)
  pinMode(PIN_BOOT, INPUT_PULLUP);
#endif
}

static inline bool buttonPressed() {
#if defined(SKETCH_HEADLESS)
  return false;                         // no pin and no window to click
#else
  static bool     was  = false;
  static uint32_t last = 0;

  #if defined(ARDUINO)
    const bool down = digitalRead(PIN_BOOT) == LOW;
  #else
    // A press counts only if it *started* in the strip. A touch-driven sketch
    // (paint) can legitimately drag a stroke through the strip, and without
    // the latch that wander would produce a rising edge here and read as a
    // BOOT press mid-stroke. Click users are unaffected: a click starts where
    // it lands.
    static bool rawWas = false, fromStrip = false;
    int32_t tx, ty;
    const bool rawDown = lcd.getTouch(&tx, &ty);
    if (rawDown && !rawWas) fromStrip = ty >= H - BUTTON_SDL_STRIP_H;
    rawWas = rawDown;
    const bool down = rawDown && fromStrip;
  #endif

  const bool edge = down && !was;
  was = down;
  if (!edge) return false;

  const uint32_t now = millis();
  if (now - last < BUTTON_DEBOUNCE_MS) return false;
  last = now;
  return true;
#endif
}

// ---------------------------------------------------------------------------
// State that survives a reboot
// ---------------------------------------------------------------------------
// A few small integers kept in NVS. Flash is the only thing that survives the
// RESET button: it pulls EN, which is a power-on reset, and RTC memory — the
// cheap answer to "remember one number across a reboot" — is powered from the
// domain that drops with it.
//
// Cost is one small write per call, so only write on a button press, never per
// frame. NVS wear-levels, and a press is a human-rate event.
//
// Off-device both are no-ops and a read returns its default, so an SDL run
// always starts from a clean slate — which is what keeps headless captures
// reproducible.
static inline uint32_t persistGet(const char *key, uint32_t fallback) {
#if defined(ARDUINO)
  Preferences p;
  if (!p.begin("sketchbook", true)) return fallback;   // read-only; absent namespace fails
  const uint32_t v = p.getUInt(key, fallback);
  p.end();
  return v;
#else
  (void)key;
  return fallback;
#endif
}

static inline void persistPut(const char *key, uint32_t value) {
#if defined(ARDUINO)
  Preferences p;
  if (!p.begin("sketchbook", false)) return;
  p.putUInt(key, value);
  p.end();
#else
  (void)key; (void)value;
#endif
}

// ---------------------------------------------------------------------------
// Seeds
// ---------------------------------------------------------------------------
// A fresh seed for generate(). Every sketch calls this from setup(), so this is
// the whole of what the RESET button does: reboot, reseed, redraw.
//
// The one guarantee that matters is that a RESET does not hand back the
// composition you just pressed the button to be rid of — and esp_random() alone
// does not make it. With the RF subsystem off, the bootloader's entropy source
// is switched off again before the app starts, so the sequence one boot sees is
// not promised to differ from the last one's. A counter in flash is promised to
// differ, so the two are mixed: esp_random() supplies the variation *within* a
// boot, the counter supplies it *across* boots, and neither has to be trusted
// alone.
//
// Off-device this is plain esp_random() — rand() under the shim — which the
// headless main() seeds from argv so that a capture reproduces exactly.
static inline uint32_t newSeed() {
#if defined(ARDUINO)
  // Read once per boot, not once per call: switcher.cpp re-runs a sketch's
  // setup() on every BOOT press, and re-reading here would turn each press into
  // a second flash write for no added entropy.
  static uint32_t salt   = 0;
  static bool     loaded = false;
  if (!loaded) {
    loaded = true;
    salt   = persistGet("boots", 0) + 1;
    persistPut("boots", salt);
    salt *= 0x9E3779B9u;              // spread consecutive boot counts apart
  }
  return esp_random() ^ salt;
#else
  return esp_random();
#endif
}

// ---------------------------------------------------------------------------
// Bulk scratch memory
// ---------------------------------------------------------------------------
// For a big buffer that does *not* need to be in fast internal SRAM. On device
// this comes out of the 8MB octal PSRAM, leaving internal DRAM for the one
// allocation that genuinely needs the speed — the framebuffer.
//
// Only reach for this when the access pattern is span- or scan-shaped. PSRAM
// goes through the data cache, so sequential runs cost about one line fill per
// 32 bytes; genuinely random access pays the full round trip every time and
// belongs in a static array instead.
//
// Falls back to the internal heap when there is no PSRAM, so a caller only has
// to check for null. Off-device it is plain malloc.
static inline void *psramAlloc(size_t n) {
#if defined(ARDUINO)
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  return p ? p : malloc(n);
#else
  return malloc(n);
#endif
}

// ---------------------------------------------------------------------------
// Framebuffer lifecycle
// ---------------------------------------------------------------------------

// Bring up the panel and allocate the full-frame sprite (W*H*2 bytes). Where
// it lands is the board's call, via FB_IN_PSRAM: the 1.47B's 110KB frame fits
// in internal SRAM, which is what makes per-pixel work on it cheap; a frame
// too big to fit internally (the AMOLED-1.8's 322KB) goes to PSRAM instead.
// Returns false if the allocation failed — always check it; a silent null
// sprite draws nothing and looks like a logic bug.
//
// Idempotent. A sketch calls this from its own setup(), so a binary hosting
// several sketches (switcher.cpp) runs it again on every switch — re-initing
// the SPI bus and churning a 110KB alloc each time would be a slow way to
// fragment the heap. Re-entry keeps the existing sprite and only repaints the
// panel black, so the incoming sketch still starts on a clean screen.
static inline bool panelBegin(uint8_t brightness = 170) {
  static bool up = false;
#if defined(SKETCH_HEADLESS)
  (void)brightness;                     // no panel, no backlight, no SDL window
#else
  if (!up) {
#if defined(ARDUINO)
    boardPowerBegin();   // board headers define it; a PMU board turns its rails on here
#endif
    lcd.init();
    lcd.setRotation(0);
  }
  lcd.setBrightness(brightness);
  lcd.fillScreen(TFT_BLACK);
#endif
  if (up) return cv.getBuffer() != nullptr;
  up = true;
  cv.setPsram(FB_IN_PSRAM);
  cv.setColorDepth(16);
  return cv.createSprite(W, H) != nullptr;
}

// Blit the sprite to the panel — or, headless, capture it.
static inline void present() {
#if defined(SKETCH_HEADLESS)
  char path[512];
  snprintf(path, sizeof(path), "%s/frame_%03d.png", _shotDir, _frameNo);
  // Say so when the write fails. _frameNo counts frames *rendered*, so it has
  // to advance either way or the run would never reach its target and would
  // spin to the iteration cap instead — but that also means a run whose output
  // directory is unwritable otherwise ends silently, exit 0, with nothing on
  // disk and nothing on stderr. A capture that reports success while producing
  // no evidence is worse than one that crashes.
  if (writeFramePng(path)) fprintf(stderr, "wrote %s\n", path);
  else                     fprintf(stderr, "FAILED to write %s\n", path);
  _frameNo++;
#else
  cv.pushSprite(0, 0);
#endif
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
void setup();
void loop();

#if !defined(ARDUINO)

#if defined(SKETCH_HEADLESS)

//   .pio/build/<env>/program [outDir] [seed] [frames]
//
// All three are optional: "." , 1, and SKETCH_FRAMES respectively. Run the
// binary directly rather than through `pio run -t exec`, which has no way to
// forward arguments.
int main(int argc, char **argv) {
  if (argc > 1) _shotDir = argv[1];
  srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1u);
  const int want = argc > 3 ? atoi(argv[3]) : SKETCH_FRAMES;
  setup();
  for (int i = 0; i < 200000 && _frameNo < want; i++) {
    loop();
    _vclock += 16;
  }
  return 0;
}

#elif defined(SDL_h_)

__attribute__((weak))
int user_func(bool *running) {
  setup();
  do { loop(); } while (*running);
  return 0;
}

int main(int, char **) {
  // Desktop has no RESET button, so relaunching the binary is how you ask for a
  // new composition — which only works if the run is seeded. Nothing else on
  // this path calls srand(), so without this rand() would start from its
  // default 1 and every run would open on the same frame. The headless main()
  // above deliberately does the opposite and seeds from argv.
  srand((unsigned)time(nullptr));
  return lgfx::Panel_sdl::main(user_func);
}

#endif
#endif
