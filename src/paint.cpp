// ============================================================================
// paint — finger-painting for the Touch-LCD-2
// ============================================================================
//   pio run -e paint_t2_native -t exec      SDL window (mouse-while-held = finger)
//   pio run -e paint_t2        -t upload    the board
//   pio run -e paint_t2_shot                headless PNG capture (scripted gesture)
//
// Drag paints a round brush stroke into the persistent canvas; a quick tap
// (short hold, little movement) cycles the brush to the next palette color
// instead of leaving a mark. The two gestures share the same press: nothing
// is committed until either the timer or the movement threshold decides
// which one it was, and a drag only starts painting from the moment it
// crosses that threshold — walking the stroke back to the original press
// point so a fast flick doesn't leave a gap.
//
// RESET reboots into a fresh palette and a blank canvas, for free — setup()
// always draws bg + swatch from a new seed. Shaking the board wipes the
// drawing Etch A Sketch style, keeping the palette and the current crayon —
// device only: on SDL the clicks are the finger, so shake has no stand-in
// there (relaunch instead), and headless never fires it.
// ============================================================================

#define SKETCH_TITLE  "paint"
#define SKETCH_FRAMES 90

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/touch.h"
#include "shared/imu.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const float BRUSH_RADIUS = 6.0f;
static const float BRUSH_STEP   = BRUSH_RADIUS * 0.5f;   // interpolation spacing

// A press that resolves within TAP_MAX_MS, having moved less than
// TAP_MAX_MOVE from where it started, is a tap; anything else is a drag.
static const uint32_t TAP_MAX_MS   = 250;
static const float    TAP_MAX_MOVE = 8.0f;

// Shake wipes the drawing (bg + swatch repainted, palette and crayon kept).
// Same threshold and cooldown as the shake sketches; small hands shake hard.
static const float    SHAKE_G           = 2.9f;
static const uint32_t SHAKE_COOLDOWN_MS = 600;

// Brush colors are palette entries that read against the background — a thin
// stroke needs less contrast than a filled area, so this sits below the
// arc-tile sketches' BG_CONTRAST/FG_CONTRAST. Separation between brush colors
// uses oklab distance, not WCAG, for the same reason CLAUDE.md gives: WCAG
// can't see hue and would let two same-luminance colors knock each other out.
static const float BRUSH_CONTRAST    = 2.2f;
static const float BRUSH_MIN_DELTA_E = 0.12f;
static const int   MAX_BRUSH_COLORS  = 16;
// The found palettes are small (4-6 entries) and the contrast filter can gut
// them to two inks, which is no fun to paint with — so after keeping whatever
// palette entries read, the set is topped up from an oklch hue wheel until a
// full painting palette is on hand.
static const int   TARGET_BRUSH_COLORS = 12;

// Inset enough to clear a rounded glass corner: at 20px the AMOLED-1.8's
// bezel curve clips the ring. 32 sits fully inside on every board so far.
static const int SWATCH_R  = 9;
static const int SWATCH_CX = W - 32;
static const int SWATCH_CY = 32;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static uint32_t bg;
static uint32_t brushColors[MAX_BRUSH_COLORS];
static int      nBrushColors;
static int      colorIndex;
static uint32_t brushColor;
static uint32_t outlineColor;   // swatch ring, so it reads regardless of brushColor vs bg

static void buildPalette() {
  const Palette pal = randomPalette();
  bg = pal.c[rngIndex(pal.n)];

  nBrushColors = 0;
  for (int i = 0; i < pal.n && nBrushColors < MAX_BRUSH_COLORS; i++) {
    const uint32_t cand = pal.c[i];
    if (cand == bg) continue;
    if (wcagContrast(cand, bg) < BRUSH_CONTRAST) continue;

    bool ok = true;
    for (int j = 0; j < nBrushColors; j++)
      if (deltaEok(cand, brushColors[j]) < BRUSH_MIN_DELTA_E) { ok = false; break; }
    if (ok) brushColors[nBrushColors++] = cand;
  }
  // Top up to a full painting palette from an oklch hue wheel: evenly spaced
  // hues from a random start, passing the same contrast and separation bars
  // as the palette entries did. Lightness passes walk away from the
  // background on its far side first, then cross over — a mid-luminance
  // background (a saturated orange, say) can be unreachable from one side at
  // 2.2:1, so both directions get a turn rather than trusting a light/dark
  // coin flip.
  const bool  bgLight  = relLuminance(bg) > 0.4f;
  const float h0       = rngRange(0.0f, 360.0f);
  const float inkL[4]  = { bgLight ? 0.42f : 0.76f, bgLight ? 0.30f : 0.86f,
                           bgLight ? 0.76f : 0.42f, bgLight ? 0.86f : 0.30f };
  for (int pass = 0; pass < 4 && nBrushColors < TARGET_BRUSH_COLORS; pass++) {
    for (int k = 0; k < 24 && nBrushColors < TARGET_BRUSH_COLORS; k++) {
      const uint32_t cand = oklchToRgb(inkL[pass], 0.14f, fmodf(h0 + (float)k * 15.0f, 360.0f));
      if (wcagContrast(cand, bg) < BRUSH_CONTRAST) continue;
      bool ok = true;
      for (int j = 0; j < nBrushColors; j++)
        if (deltaEok(cand, brushColors[j]) < BRUSH_MIN_DELTA_E) { ok = false; break; }
      if (ok) brushColors[nBrushColors++] = cand;
    }
  }
  // Even the wheel can't fail this hard, but keep the floor: whichever of
  // black/white reads best against the background (CLAUDE.md's `bless` story).
  if (nBrushColors == 0) brushColors[nBrushColors++] = bgLight ? 0x000000 : 0xFFFFFF;

  outlineColor = relLuminance(bg) > 0.4f ? 0x000000 : 0xFFFFFF;
  colorIndex   = 0;
  brushColor   = brushColors[colorIndex];

  Serial.printf("bg #%06lX  %d brush colors:", (unsigned long)bg, nBrushColors);
  for (int i = 0; i < nBrushColors; i++) Serial.printf(" #%06lX", (unsigned long)brushColors[i]);
  Serial.println();
}

static void drawSwatch() {
  cv.fillSmoothCircle(SWATCH_CX, SWATCH_CY, SWATCH_R + 2, to565(outlineColor));
  cv.fillSmoothCircle(SWATCH_CX, SWATCH_CY, SWATCH_R, to565(brushColor));
}

// ---------------------------------------------------------------------------
// Brush
// ---------------------------------------------------------------------------
static inline void stampBrush(float x, float y) {
  cv.fillSmoothCircle((int)lroundf(x), (int)lroundf(y), (int)BRUSH_RADIUS, to565(brushColor));
}

// Interpolates from (x0,y0) to (x1,y1) at ~BRUSH_STEP spacing, so a fast drag
// leaves a continuous stroke instead of dotted stamps.
static void strokeTo(float x0, float y0, float x1, float y1) {
  const float dx   = x1 - x0, dy = y1 - y0;
  const float dist = sqrtf(dx * dx + dy * dy);
  int steps = (int)(dist / BRUSH_STEP);
  if (steps < 1) steps = 1;
  for (int i = 1; i <= steps; i++) {
    const float t = (float)i / (float)steps;
    stampBrush(x0 + dx * t, y0 + dy * t);
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
// touchPoint() reads real hardware, or the mouse-while-held on SDL — but
// always false headless, with no window to click. A capture instead plays a
// scripted gesture through this same call site: a couple of curved strokes
// (a spiral, an S-curve) sampled at finger-drag spacing, with a tap between
// and around them so color cycling shows up too. One call per loop(), so the
// step counter advances at the sketch's own cadence.
#if defined(SKETCH_HEADLESS)

static const int TAP1_START     = 0;
static const int TAP1_LEN       = 4;                               // held ~4 loops, well under TAP_MAX_MS
static const int TAP1_RELEASE   = TAP1_START + TAP1_LEN;

static const int SPIRAL_START   = TAP1_RELEASE + 1;
static const int SPIRAL_LEN     = 40;
static const int SPIRAL_RELEASE = SPIRAL_START + SPIRAL_LEN;

static const int TAP2_START     = SPIRAL_RELEASE + 1;
static const int TAP2_LEN       = 4;
static const int TAP2_RELEASE   = TAP2_START + TAP2_LEN;

static const int SCURVE_START   = TAP2_RELEASE + 1;
static const int SCURVE_LEN     = 40;
static const int SCURVE_RELEASE = SCURVE_START + SCURVE_LEN;

static bool scriptTouch(int *x, int *y) {
  static int step = 0;
  const int i = step++;

  if (i >= TAP1_START && i < TAP1_RELEASE)  { *x = 60; *y = 60; return true; }
  if (i == TAP1_RELEASE) return false;

  if (i >= SPIRAL_START && i < SPIRAL_RELEASE) {
    const float t   = (float)(i - SPIRAL_START) / (float)(SPIRAL_LEN - 1);
    const float ang = t * 4.0f * (float)M_PI;               // two turns
    const float r   = 10.0f + t * 70.0f;
    *x = (int)lroundf(120.0f + r * cosf(ang));
    *y = (int)lroundf(150.0f + r * sinf(ang));
    return true;
  }
  if (i == SPIRAL_RELEASE) return false;

  if (i >= TAP2_START && i < TAP2_RELEASE)  { *x = 180; *y = 250; return true; }
  if (i == TAP2_RELEASE) return false;

  if (i >= SCURVE_START && i < SCURVE_RELEASE) {
    const float t = (float)(i - SCURVE_START) / (float)(SCURVE_LEN - 1);
    *x = (int)lroundf(20.0f + t * 200.0f);
    *y = (int)lroundf(160.0f + 80.0f * sinf(t * 2.0f * (float)M_PI));
    return true;
  }
  return false;                                              // done; finger stays up
}

#endif

static inline bool readTouch(int *x, int *y) {
#if defined(SKETCH_HEADLESS)
  return scriptTouch(x, y);
#else
  return touchPoint(x, y);
#endif
}

// ---------------------------------------------------------------------------
// Gesture state
// ---------------------------------------------------------------------------
static bool     touchDown = false;
static bool     dragging  = false;   // has this press committed to painting?
static float    pressX, pressY;      // where the current press started
static float    lastX, lastY;        // last known finger position while down
static uint32_t pressTime;

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  if (!touchBegin()) Serial.println("no touch controller - this sketch needs one");
  imuBegin();   // shake-to-clear; without an IMU the sketch just keeps drawing

  rngSeed(newSeed());
  buildPalette();

  cv.fillScreen(to565(bg));
  drawSwatch();
  present();
}

void loop() {
  static uint32_t lastShakeAt = 0;
  if (millis() - lastShakeAt >= SHAKE_COOLDOWN_MS && imuShakeDetected(SHAKE_G)) {
    lastShakeAt = millis();
    cv.fillScreen(to565(bg));
    drawSwatch();
    present();
  }

  int tx, ty;
  const bool touched = readTouch(&tx, &ty);
  bool changed = false;

  if (touched) {
    const float fx = (float)tx, fy = (float)ty;

    if (!touchDown) {
      touchDown = true;
      dragging  = false;
      pressX    = fx; pressY = fy;
      pressTime = millis();
    } else if (!dragging) {
      const float ddx = fx - pressX, ddy = fy - pressY;
      if (sqrtf(ddx * ddx + ddy * ddy) >= TAP_MAX_MOVE) {
        // Committed to a drag: paint from the original press point so the
        // stroke has no gap before this first qualifying sample.
        dragging = true;
        strokeTo(pressX, pressY, fx, fy);
        changed = true;
      }
    } else {
      strokeTo(lastX, lastY, fx, fy);
      changed = true;
    }

    lastX = fx; lastY = fy;
  } else if (touchDown) {
    touchDown = false;
    if (!dragging && (millis() - pressTime) < TAP_MAX_MS) {
      colorIndex = (colorIndex + 1) % nBrushColors;
      brushColor = brushColors[colorIndex];
      drawSwatch();
      changed = true;
    }
    dragging = false;
  }

  if (changed) present();

  delay(5);
}
