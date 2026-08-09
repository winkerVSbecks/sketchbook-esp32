// ============================================================================
// arc-tile grid, shake to regenerate — native SDL + ESP32-S3-LCD-1.47B
// ============================================================================
//   pio run -e shake_native -t exec     (click the window to "shake")
//   pio run -e shake_esp32  -t upload
//
// Keep in sync with the Arduino IDE copy:
//   cp src/arc_tiles_shake.cpp ~/Documents/Arduino/arc_tiles_shake/sketch.h
// ============================================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>

static const int W = 172;
static const int H = 320;

// ---------------------------------------------------------------------------
// Platform shims
// ---------------------------------------------------------------------------
#if defined(ARDUINO)

  #include <Arduino.h>
  #include <Wire.h>

#else

  #include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
  #include <cstdio>
  #include <cstdlib>

  using lgfx::millis;      // LovyanGFX provides these for SDL (sdl/common.hpp)
  using lgfx::delay;

  static uint32_t esp_random() {
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
  }

  struct SerialShim {
    void begin(int) {}
    template <typename... A> void printf(const char *f, A... a) { std::printf(f, a...); }
    void println(const char *s = "") { std::printf("%s\n", s); }
  };
  static SerialShim Serial;

#endif

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
#if defined(ARDUINO)

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 40; c.pin_mosi = 45; c.pin_miso = -1; c.pin_dc = 41;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 42; c.pin_rst = 39; c.pin_busy = -1;
      c.memory_width = 240; c.memory_height = 320;
      c.panel_width  = W;   c.panel_height  = H;
      c.offset_x = 34; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 46; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

#else

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
      _panel.setWindowTitle("arc tiles 172x320");
    }
    setPanel(&_panel);
  }
};

#endif

static LGFX        lcd;
static LGFX_Sprite cv(&lcd);

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const int      RES_X   = 3;
static const int      RES_Y   = 6;
static const int      NCELLS  = RES_X * RES_Y;
static const uint32_t HOLD_MS = 8000;   // only used if the IMU is missing

// Inset the grid by this many pixels on every side. The margin needs no
// drawing of its own — fillScreen already lays down bg, and the grid is
// simply laid out inside the smaller rect.
static const int      MARGIN  = 8;

// Shake gesture. At rest the accelerometer reads ~1.0g (gravity); a deliberate
// shake spikes well past 2g. Lower SHAKE_G for a hair trigger, raise it if the
// piece regenerates when you just pick the board up. The cooldown stops one
// wrist-flick — which is several peaks — from firing a dozen times.
static const float    SHAKE_G           = 2.9f;
static const uint32_t SHAKE_COOLDOWN_MS = 600;

// How many foreground colours a composition may use. 1 = a two-tone piece:
// one background, one ink. Raise it (up to 16) to go back to multi-colour —
// MIN_DELTA_E below only does any work when this is > 1.
static const int      MAX_COLORS = 1;

// Colour separation thresholds.
//   BG_CONTRAST  — WCAG ratio vs black; decides which entries can be background
//   FG_CONTRAST  — WCAG ratio vs the chosen background (legibility: correct use)
//   MIN_DELTA_E  — oklab distance between two foreground colours (see below)
static const float BG_CONTRAST = 4.0f;
static const float FG_CONTRAST = 3.0f;
static const float MIN_DELTA_E = 0.20f;   // tune me: 0.10 permissive, 0.35 strict

// ---------------------------------------------------------------------------
// Palettes (colors/found.ts)
// ---------------------------------------------------------------------------
static const uint32_t CARMEN[] = {
  0xFDFCF3, 0x002500, 0xCEFF00, 0x2A42FF, 0x2B0404,
  0xECE5F0, 0xAB2A00, 0xC15F3D, 0xEB562F,
};
static const uint32_t BLESS[] = {
  0xFFFFFF, 0xFBF9F3, 0xA8F0E6, 0xE5D5FF, 0xFFDDDD,
  0x000000, 0xFFA500, 0x8F0202, 0x042411,
};

// ---------------------------------------------------------------------------
// PRNG — mulberry32
// ---------------------------------------------------------------------------
static uint32_t rngState = 1;

static void  rngSeed(uint32_t s) { rngState = s ? s : 1; }
static float rngNext() {
  rngState += 0x6D2B79F5u;
  uint32_t t = rngState;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return (float)((t ^ (t >> 14)) >> 8) / 16777216.0f;
}
static int rngIndex(int n) { int i = (int)(rngNext() * n); return i >= n ? n - 1 : i; }

static void rngShuffle(uint32_t *a, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = rngIndex(i + 1);
    uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
  }
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
static inline float srgbLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float relLuminance(uint32_t rgb) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// WCAG contrast: a luminance ratio. Right tool for "is this legible against
// that background", wrong tool for "are these two colours distinguishable" —
// it can't see hue, so #2A42FF (blue) vs #AB2A00 (rust) scores about 1.0.
static float wcagContrast(uint32_t a, uint32_t b) {
  float la = relLuminance(a), lb = relLuminance(b);
  float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
  return (hi + 0.05f) / (lo + 0.05f);
}

// Perceptual distance in oklab — this is what separates the foreground colours.
static void toOklab(uint32_t rgb, float &L, float &A, float &B) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
  float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
  float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
  L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}
// Lowercase locals deliberately: the Arduino core's binary.h #defines B0, B1,
// B01, B10 ... as binary literals, so `float B1` fails to compile on device
// under core 2.x (which is what PlatformIO's espressif32 platform ships).
// Core 3.x dropped them, so the Arduino IDE hides this — don't "tidy" these
// back to uppercase.
static float deltaEok(uint32_t c1, uint32_t c2) {
  float l1, a1, b1, l2, a2, b2;
  toOklab(c1, l1, a1, b1);
  toOklab(c2, l2, a2, b2);
  float dL = l1 - l2, dA = a1 - a2, dB = b1 - b2;
  return sqrtf(dL * dL + dA * dA + dB * dB);
}

static inline uint16_t to565(uint32_t rgb) {
  return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
static inline uint16_t blend565(uint32_t a, uint32_t b, float t) {
  float ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  float br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  uint8_t r  = (uint8_t)(ar + (br - ar) * t + 0.5f);
  uint8_t g  = (uint8_t)(ag + (bg - ag) * t + 0.5f);
  uint8_t bl = (uint8_t)(ab + (bb - ab) * t + 0.5f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

// ---------------------------------------------------------------------------
// Cell types — each "-arc" cell is a quarter disc centred on one corner
// ---------------------------------------------------------------------------
enum CellType : uint8_t { T_FULL = 0, T_TL = 1, T_TR = 2, T_BL = 3, T_BR = 4 };

// mirrors[type][edge] as a 5-bit mask. Edges: 0=top 1=right 2=bottom 3=left.
static const uint8_t MIRRORS[5][4] = {
  /* 0123    */ { 0x19, 0x0B, 0x07, 0x15 },
  /* 013-arc */ { 0x09, 0x00, 0x00, 0x05 },
  /* 012-arc */ { 0x11, 0x03, 0x00, 0x00 },
  /* 023-arc */ { 0x00, 0x00, 0x03, 0x11 },
  /* 123-arc */ { 0x00, 0x09, 0x05, 0x00 },
};

struct GridCell {
  uint8_t  x, y;
  bool     occupied;
  uint32_t color;
  CellType type;
  int16_t  areaId;
};

static GridCell grid[NCELLS];
static uint32_t bg;
static uint32_t colors[MAX_COLORS];
static int      nColors;

static inline int xyToIndex(int x, int y) { return y * RES_X + x; }

// ---------------------------------------------------------------------------
// Palette selection
// ---------------------------------------------------------------------------
static void buildPalette() {
  uint32_t base[9];
  const uint32_t *src = rngNext() < 0.5f ? CARMEN : BLESS;
  for (int i = 0; i < 9; i++) base[i] = src[i];
  rngShuffle(base, 9);

  uint32_t bgOpts[9]; int nbg = 0;
  bool isBgOpt[9] = { false };
  for (int i = 0; i < 9; i++) {
    if (wcagContrast(base[i], 0x000000) >= BG_CONTRAST) {
      bgOpts[nbg++] = base[i];
      isBgOpt[i] = true;
    }
  }
  if (nbg == 0) bgOpts[nbg++] = 0xFFFFFF;

  rngShuffle(bgOpts, nbg);
  bg = bgOpts[nbg - 1];                        // shuffle().pop()

  // Foreground set. Two independent jobs, two different metrics:
  //   vs background — WCAG, because that IS a legibility question
  //   vs each other — oklab distance, because that's a "can I tell these
  //                   apart" question, and WCAG can't see hue at all.
  // The original used WCAG >= 4.0 for both, which is why bless always
  // collapsed to one colour: its three dark entries (#000000, #8F0202,
  // #042411) sit at ratios of ~1.7-2.2 to each other, so whichever the
  // shuffle put first knocked out the other two every time.
  nColors = 0;
  for (int i = 0; i < 9 && nColors < MAX_COLORS; i++) {
    if (isBgOpt[i]) continue;
    if (wcagContrast(base[i], bg) < FG_CONTRAST) continue;

    bool ok = true;
    for (int j = 0; j < nColors; j++) {
      if (deltaEok(base[i], colors[j]) < MIN_DELTA_E) { ok = false; break; }
    }
    if (ok) colors[nColors++] = base[i];
  }

  // Fallbacks — the JS has none and would paint `undefined`.
  if (nColors == 0) {
    for (int i = 0; i < 9 && nColors < MAX_COLORS; i++) {
      if (!isBgOpt[i] && wcagContrast(base[i], bg) >= FG_CONTRAST) colors[nColors++] = base[i];
    }
  }
  if (nColors == 0) colors[nColors++] = (relLuminance(bg) > 0.3f) ? 0x000000 : 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// Grid generation
// ---------------------------------------------------------------------------
static void resetGrid() {
  for (int y = 0; y < RES_Y; y++)
    for (int x = 0; x < RES_X; x++) {
      GridCell &c = grid[xyToIndex(x, y)];
      c.x = x; c.y = y; c.occupied = false; c.color = 0; c.type = T_FULL; c.areaId = -1;
    }
}

static const int8_t DIRS[4][2] = { {0,1}, {1,0}, {0,-1}, {-1,0} };  // down right up left

static void createArea(int areaId) {
  int freeCells[NCELLS], nfree = 0;
  for (int i = 0; i < NCELLS; i++) if (!grid[i].occupied) freeCells[nfree++] = i;
  if (nfree == 0) return;

  uint32_t  color   = colors[rngIndex(nColors)];
  GridCell *current = &grid[freeCells[rngIndex(nfree)]];
  current->occupied = true;
  current->color    = color;
  current->areaId   = areaId;

  int count = 1;
  while (true) {
    int opts[4], nopts = 0;
    for (int d = 0; d < 4; d++) {
      int nx = current->x + DIRS[d][0];
      int ny = current->y + DIRS[d][1];
      if (nx < 0 || nx >= RES_X || ny < 0 || ny >= RES_Y) continue;
      if (grid[xyToIndex(nx, ny)].occupied) continue;
      opts[nopts++] = d;
    }
    if (nopts == 0) break;
    if (count > 10)  break;

    int d  = opts[rngIndex(nopts)];
    int dx = DIRS[d][0], dy = DIRS[d][1];
    GridCell *next = &grid[xyToIndex(current->x + dx, current->y + dy)];
    next->occupied = true;
    next->color    = color;
    next->areaId   = areaId;

    int edge = (dx ==  1) ? 1 : (dx == -1) ? 3 : (dy == 1) ? 2 : 0;
    uint8_t mask = MIRRORS[current->type][edge];
    if (mask == 0) break;

    CellType allowed[5]; int nallowed = 0;
    for (int t = 0; t < 5; t++) if (mask & (1u << t)) allowed[nallowed++] = (CellType)t;

    next->type = allowed[rngIndex(nallowed)];
    current    = next;
    count++;
  }
}

static void fillGridWithAreas() {
  int attempts = 0;
  while (attempts < 100) {
    int unoccupied = 0;
    for (int i = 0; i < NCELLS; i++) if (!grid[i].occupied) unoccupied++;
    if (unoccupied == 0) break;
    createArea(attempts);
    attempts++;
  }
  Serial.printf("Filled grid with areas in %d attempts\n", attempts);
}

static void reduce() {
  for (int i = 0; i < NCELLS; i++) {
    GridCell &cell = grid[i];

    uint32_t nb[4]; int nnb = 0;
    for (int d = 0; d < 4; d++) {
      int nx = cell.x + DIRS[d][0], ny = cell.y + DIRS[d][1];
      if (nx < 0 || nx >= RES_X || ny < 0 || ny >= RES_Y) continue;
      nb[nnb++] = grid[xyToIndex(nx, ny)].color;
    }

    bool shares = false;
    for (int k = 0; k < nnb; k++) if (nb[k] == cell.color) { shares = true; break; }
    if (shares || nnb == 0) continue;

    uint32_t best = cell.color; int bestCount = 0;
    for (int k = 0; k < nnb; k++) {
      int c = 0;
      for (int m = 0; m < nnb; m++) if (nb[m] == nb[k]) c++;
      if (c > bestCount) { bestCount = c; best = nb[k]; }
    }
    cell.color = best;
  }
}

// ---------------------------------------------------------------------------
// Rendering — analytic coverage AA
// ---------------------------------------------------------------------------
static void drawCell(const GridCell &c, int x0, int x1, int y0, int y1) {
  const int w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return;

  if (c.type == T_FULL) {
    cv.fillRect(x0, y0, w, h, to565(c.color));
    return;
  }

  const float rx = (float)w, ry = (float)h;
  const float cx = (c.type == T_TL || c.type == T_BL) ? (float)x0 : (float)x1;
  const float cy = (c.type == T_TL || c.type == T_TR) ? (float)y0 : (float)y1;
  const float invRx2 = 1.0f / (rx * rx), invRy2 = 1.0f / (ry * ry);

  for (int py = y0; py < y1; py++) {
    const float dy = (py + 0.5f) - cy;
    const float ny = dy / ry, ny2 = ny * ny;

    for (int px = x0; px < x1; px++) {
      const float dx = (px + 0.5f) - cx;
      const float nx = dx / rx;
      const float q  = sqrtf(nx * nx + ny2);

      float cover;
      if (q < 0.0001f) {
        cover = 1.0f;
      } else {
        const float gradLen = sqrtf(nx * nx * invRx2 + ny2 * invRy2) / q;
        const float dist    = (q - 1.0f) / gradLen;
        cover = 0.5f - dist;
        if (cover <= 0.0f) continue;
        if (cover >  1.0f) cover = 1.0f;
      }
      cv.drawPixel(px, py, cover >= 1.0f ? to565(c.color) : blend565(bg, c.color, cover));
    }
  }
}

static void render() {
  cv.fillScreen(to565(bg));

  const int innerW = W - 2 * MARGIN;
  const int innerH = H - 2 * MARGIN;

  int colEdge[RES_X + 1], rowEdge[RES_Y + 1];
  for (int i = 0; i <= RES_X; i++) colEdge[i] = MARGIN + (int)lroundf(i * (float)innerW / RES_X);
  for (int j = 0; j <= RES_Y; j++) rowEdge[j] = MARGIN + (int)lroundf(j * (float)innerH / RES_Y);

  for (int i = 0; i < NCELLS; i++) {
    const GridCell &c = grid[i];
    drawCell(c, colEdge[c.x], colEdge[c.x + 1], rowEdge[c.y], rowEdge[c.y + 1]);
  }

  cv.pushSprite(0, 0);
}

static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  buildPalette();
  Serial.printf("bg #%06lX / %d colors:", (unsigned long)bg, nColors);
  for (int i = 0; i < nColors; i++) Serial.printf(" #%06lX", (unsigned long)colors[i]);
  Serial.println();

  resetGrid();
  fillGridWithAreas();
  reduce();

  uint32_t t0 = millis();
  render();
  Serial.printf("rendered in %lums\n", (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------------------
// Shake input
//
// Device: the QMI8658 6-axis IMU over I2C. The wiki's pinout tables cover only
// the LCD, RGB LED and TF card, but ESP32-S3-LCD-1.47B-Demo.zip pins the bus
// down: I2C_SDA_PIN 48 / I2C_SCL_PIN 47, agreed on by both the Arduino
// (I2C_Driver.h) and ESP-IDF (I2C_Driver.h) demos.
//
// Do NOT take these from the ESP32 core's waveshare_esp32_s3_lcd_147 variant —
// its SDA=8/SCL=9 are generic boilerplate, not this board's wiring.
//
// Desktop: click the window.
// ---------------------------------------------------------------------------
static bool imuOk = false;

#if defined(ARDUINO)

static const int PIN_IMU_SDA = 48;
static const int PIN_IMU_SCL = 47;

static const uint8_t QMI_WHO_AM_I = 0x00;   // reads 0x05
static const uint8_t QMI_CTRL1    = 0x02;
static const uint8_t QMI_CTRL2    = 0x03;
static const uint8_t QMI_CTRL7    = 0x08;
static const uint8_t QMI_AX_L     = 0x35;

static const float ACCEL_LSB_PER_G = 4096.0f;   // at +/-8g full scale

static uint8_t imuAddr = 0;

static bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool imuRead(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)imuAddr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool imuBegin() {
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, 400000);

  const uint8_t candidates[2] = { 0x6B, 0x6A };   // SA0 pulled high / low
  for (int i = 0; i < 2; i++) {
    imuAddr = candidates[i];
    uint8_t who = 0;
    if (imuRead(QMI_WHO_AM_I, &who, 1) && who == 0x05) {
      imuWrite(QMI_CTRL1, 0x40);   // auto-increment register address, little endian
      imuWrite(QMI_CTRL2, 0x25);   // accel +/-8g, 250Hz output rate
      imuWrite(QMI_CTRL7, 0x01);   // accel on, gyro off
      delay(20);
      Serial.printf("QMI8658 at 0x%02X\n", imuAddr);
      return true;
    }
  }

  // Nothing answered at either address. Scan the bus so the log says whether
  // the part is at an unexpected address or the pins are wrong entirely.
  imuAddr = 0;
  Serial.printf("QMI8658 not found - scanning SDA=%d SCL=%d: ", PIN_IMU_SDA, PIN_IMU_SCL);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("0x%02X ", a); found++; }
  }
  Serial.println(found ? "" : "(bus empty)");
  Serial.println("falling back to the timer");
  return false;
}

static bool shakeDetected() {
  if (!imuAddr) return false;

  uint8_t raw[6];
  if (!imuRead(QMI_AX_L, raw, 6)) return false;

  const float ax = (int16_t)(raw[0] | (raw[1] << 8)) / ACCEL_LSB_PER_G;
  const float ay = (int16_t)(raw[2] | (raw[3] << 8)) / ACCEL_LSB_PER_G;
  const float az = (int16_t)(raw[4] | (raw[5] << 8)) / ACCEL_LSB_PER_G;
  const float g  = sqrtf(ax * ax + ay * ay + az * az);

  if (g <= SHAKE_G) return false;
  Serial.printf("shake %.2fg\n", g);
  return true;
}

#else

static bool imuBegin() { return true; }

// Mouse-down edge, so a held button doesn't machine-gun new seeds.
static bool shakeDetected() {
  static bool wasDown = false;
  int32_t tx, ty;
  const bool down  = lcd.getTouch(&tx, &ty);
  const bool fired = down && !wasDown;
  wasDown = down;
  return fired;
}

#endif

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(170);
  lcd.fillScreen(TFT_BLACK);

  cv.setPsram(false);
  cv.setColorDepth(16);
  if (!cv.createSprite(W, H)) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  imuOk = imuBegin();

  generate(esp_random());
}

void loop() {
  static uint32_t lastFire = 0;
  const uint32_t now = millis();

  if (now - lastFire >= SHAKE_COOLDOWN_MS && shakeDetected()) {
    lastFire = now;
    generate(esp_random());
  }

  // With no IMU there's no way to ask for a new composition, so keep the old
  // timer as a safety net rather than freezing on a single frame forever.
  if (!imuOk && now - lastFire >= HOLD_MS) {
    lastFire = now;
    generate(esp_random());
  }

  delay(5);
}

// ---------------------------------------------------------------------------
// SDL entry point
// ---------------------------------------------------------------------------
#if !defined(ARDUINO)
#if defined(SDL_h_)

__attribute__((weak))
int user_func(bool *running) {
  setup();
  do { loop(); } while (*running);
  return 0;
}

int main(int, char **) {
  return lgfx::Panel_sdl::main(user_func);
}

#endif
#endif