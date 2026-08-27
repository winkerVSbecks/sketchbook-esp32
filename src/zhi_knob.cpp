// ============================================================================
// zhi (ਜ਼ਹੀ), knob edition — the breathing grids, driven by a rotary bezel
// ============================================================================
//   pio run -e zhi_rk_native -t exec     SDL window (drag the mouse = turn)
//   pio run -e zhi_rk        -t upload   the board
//   pio run -e zhi_rk_shot               headless PNG capture, then run it
//
// The same piece as zhi.cpp — five stacked grids of rectangles breathing as a
// diagonal wave, colours from the fettepalette lamé ramp — retuned for a
// square round panel and a different instrument. On the touch boards the
// finger's y position IS the playhead; here the knob is: one full mechanical
// revolution is one loop of the breath. |cos(pi*p - d)| has period 1 in p, so
// the playhead is simply the fractional part of encoderRev() and the wrap at
// each full turn is seamless — the knob turns forever, in both directions,
// and the wave follows.
//
// A tap on the glass draws a whole new piece. Unlike zhi.cpp there is no
// tap-vs-scrub classification to do: the glass has one job, because scrubbing
// belongs to the knob. And unlike zhi.cpp, generate() does not reset the
// playhead — the physical knob doesn't move when the composition changes, and
// a playhead that jumped away from the knob position would put the two out of
// argument until the next turn. Nothing runs on a clock — see CLAUDE.md,
// "The two buttons".
//
// SDL has no reseed stand-in (the mouse is the knob): relaunch the binary,
// same as every RESET stand-in in this repo.
// ============================================================================

#define SKETCH_TITLE  "zhi knob"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/touch.h"
#include "shared/encoder.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// The JS grids [5,5] [10,5] [20,5] [30,10] [40,20] were tuned for a 2048x1152
// landscape canvas; zhi.cpp transposed them for a portrait panel. On a square
// panel the originals go back in untransposed — the aspect progression of the
// cells (square coarse layers, wide fine ones) is the piece's own.
static const int LAYERS = 5;
static const int GRID[LAYERS][2] = {
  {  5,  5 },
  { 10,  5 },
  { 20,  5 },
  { 30, 10 },
  { 40, 20 },
};

// Odd columns drop their first and last row (see createNodes), so these grids
// come to 21 + 40 + 80 + 270 + 760 = 1171.
static const int MAX_NODES = 1300;

// See zhi.cpp: every layer sweeps away from the origin as the playhead grows,
// so the wave moves the way the knob turns and never fights it.
static const int DIR = 1;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Node {
  float    x, y, w, h;    // cell bounds in pixels
  float    delay;         // phase offset, in radians
  uint16_t color;
};

static Node nodes[MAX_NODES];
static int  nNodes = 0;

static uint16_t bgColor;

static float playhead = 0.0f;
static float drawnPlayhead = -1.0f;    // last value actually pushed to the panel

// ---------------------------------------------------------------------------
// fettepalette — generateRandomColorRamp, the 'lamé' arm
// ---------------------------------------------------------------------------
// Ported from the sketch's util/color.js, itself a minified copy of
// meodai/fettepalette. Only the 'lamé' curve is reachable from this piece's
// parameters, so the other four arms are left out.
//
// The ramp is generated in HSV, converted to HSL, and the JS hands the result
// to a `hsl(...)` CSS string — which silently clamps the out-of-range values
// the tint and shade offsets produce. hslToRgb() below clamps for the same
// reason: light entries reach saturation -0.25 and lightness 1.2.

// A point on the Lamé curve at step i of `total`, giving HSV saturation and
// value. minSaturationLight/maxSaturationLight are [0,0]/[1,1] here, so the
// remap they perform reduces to the clamp.
static void pointOnCurveLame(float i, float total, float accent, float &sat, float &val) {
  const float quarter = (float)M_PI / 2.0f;
  const float m       = (i / total) * quarter;
  const float b       = 2.0f / (2.0f + 20.0f * accent);
  const float cu = cosf(m), su = sinf(m);
  sat = clampf(copysignf(powf(fabsf(cu), b), cu), 0.0f, 1.0f);
  val = clampf(copysignf(powf(fabsf(su), b), su), 0.0f, 1.0f);
}

// HSV -> HSL, keeping the hue untouched.
static void hsv2hsl(float s, float v, float &sOut, float &lOut) {
  const float l = v - (v * s) / 2.0f;
  const float n = l < 1.0f - l ? l : 1.0f - l;
  sOut = n > 0.0f ? (v - l) / n : 0.0f;
  lOut = l;
}

static uint32_t hslToRgb(float h, float s, float l) {
  s = clampf(s, 0.0f, 1.0f);
  l = clampf(l, 0.0f, 1.0f);
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;

  const float c  = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
  const float hp = h / 60.0f;
  const float x  = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  const float m  = l - c / 2.0f;

  float r = 0.0f, g = 0.0f, b = 0.0f;
  switch ((int)hp) {
    case 0:  r = c; g = x;        break;
    case 1:  r = x; g = c;        break;
    case 2:         g = c; b = x; break;
    case 3:         g = x; b = c; break;
    case 4:  r = x;        b = c; break;
    default: r = c;        b = x; break;
  }
  return rgbPack(r + m, g + m, b + m);
}

// The ramp: 9 light tints, then 9 base colours, then 9 shades — the JS's
// `all: [...light, ...base, ...dark]`, in that order, because the background is
// picked out of the first third by index.
static const int RAMP_TOTAL = 9;
static const int RAMP_N     = RAMP_TOTAL * 3;

struct Hsl { float h, s, l; };
static Hsl ramp[RAMP_N];

static void generateRamp(float centerHue, float hueCycle) {
  const float total = (float)RAMP_TOTAL + 1.0f;
  const float curveAccent = 0.2f, offsetTint = 0.251f, offsetShade = 0.01f;
  const float offsetCurveModTint = 0.03f, offsetCurveModShade = 0.03f;
  // tintShadeHueShift is 0 in this piece, so tints and shades keep the base
  // hue. Left as a named constant rather than folded away: it is the one
  // parameter that visibly separates the three thirds when it isn't 0.
  const float tintShadeHueShift = 0.0f;

  for (int i = 1; i <= RAMP_TOTAL; i++) {
    const float hue = fmodf(360.0f + (-180.0f * hueCycle +
                                      (centerHue + (float)i * (360.0f / total) * hueCycle)),
                            360.0f);

    float sat, val, s, l;

    pointOnCurveLame((float)i, total, curveAccent, sat, val);
    hsv2hsl(sat, val, s, l);
    ramp[RAMP_TOTAL + i - 1] = Hsl{ hue, s, l };                       // base

    pointOnCurveLame((float)i, total, curveAccent + offsetCurveModTint, sat, val);
    hsv2hsl(sat, val, s, l);
    ramp[i - 1] = Hsl{ fmodf(hue + 360.0f * tintShadeHueShift, 360.0f),
                       s - offsetTint, l + offsetTint };                // light

    pointOnCurveLame((float)i, total, curveAccent - offsetCurveModShade, sat, val);
    hsv2hsl(sat, val, s, l);
    ramp[RAMP_TOTAL * 2 + i - 1] = Hsl{ fmodf(360.0f + (hue - 360.0f * tintShadeHueShift), 360.0f),
                                        s - offsetShade, l - offsetShade };  // dark
  }
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
// The JS builds its drawing set as `clrs.all.map(hsl).filter(c => c !== bg)` —
// a string compare against the formatted `hsl(h, s%, l%)`, with everything
// rounded to two decimals first. Two ramp entries that differ below that
// resolution would both drop out, so the comparison is reproduced at the same
// resolution rather than by index.
static uint16_t colors[RAMP_N];
static int      nColors = 0;

static inline bool sameFormatted(const Hsl &a, const Hsl &b) {
  return lroundf(a.h * 100.0f) == lroundf(b.h * 100.0f) &&
         lroundf(clampf(a.s, 0.0f, 1.0f) * 10000.0f) == lroundf(clampf(b.s, 0.0f, 1.0f) * 10000.0f) &&
         lroundf(clampf(a.l, 0.0f, 1.0f) * 10000.0f) == lroundf(clampf(b.l, 0.0f, 1.0f) * 10000.0f);
}

// Random.pick(colors). See zhi.cpp for why this takes no arguments.
static inline uint16_t clr() { return colors[rngIndex(nColors)]; }

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------
// Even columns carry every row; odd columns drop the first and the last. The
// grid is therefore nearly solid, not sparse — the checkerboard the piece reads
// as comes out of the 2x2 colour scheme plus the phase offset, not out of the
// layout.
static void createNodes(int cols, int rows, int dir) {
  const float sizeX = (float)W / (float)cols;
  const float sizeY = (float)H / (float)rows;
  const float diag  = sqrtf((float)W * (float)W + (float)H * (float)H);

  const uint16_t palette[2][2] = { { clr(), clr() }, { clr(), clr() } };

  for (int x = 0; x < cols; x++) {
    const bool even   = (x % 2) == 0;
    const int  offset = even ? 0 : 1;
    const int  max    = even ? rows : rows - 1;

    for (int y = offset; y < max; y++) {
      if (nNodes >= MAX_NODES) return;
      Node &n = nodes[nNodes++];
      n.x = (float)x * sizeX;
      n.y = (float)y * sizeY;
      n.w = sizeX;
      n.h = sizeY;
      n.color = palette[even ? 0 : 1][(y % 2) == 0 ? 0 : 1];
      // mapRange(hypot(px, py), 0, hypot(W, H), 0, PI * dir)
      n.delay = sqrtf(n.x * n.x + n.y * n.y) / diag * (float)M_PI * (float)dir;
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// eases.expoInOut.
static inline float expoInOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t < 0.5f ? 0.5f * exp2f(20.0f * t - 10.0f)
                  : 1.0f - 0.5f * exp2f(10.0f - 20.0f * t);
}

static void renderAll(float ph) {
  cv.fillScreen(bgColor);

  const float phase = (float)M_PI * ph;
  for (int i = 0; i < nNodes; i++) {
    const Node &n = nodes[i];
    const float t = expoInOut(fabsf(cosf(phase - n.delay)));
    const float w = n.w * (1.0f - t);
    const float h = n.h * (1.0f - t);
    const int rw = (int)lroundf(w);
    const int rh = (int)lroundf(h);
    if (rw <= 0 || rh <= 0) continue;
    cv.fillRect((int)lroundf(n.x + (n.w - w) * 0.5f),
                (int)lroundf(n.y + (n.h - h) * 0.5f),
                rw, rh, n.color);
  }
}

// ---------------------------------------------------------------------------
static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  // Same draw order as the JS module scope, so a feature set reads the same way
  // even though the streams differ (mulberry32 here, ARC4 there).
  const float centerHue = rngRange(240.0f, 300.0f);
  const float hueCycle  = rngRange(0.5f, 1.0f);

  generateRamp(centerHue, hueCycle);

  const int bgIdx = rngIndex(RAMP_TOTAL);           // Random.pick(clrs.light)
  const Hsl &bgHsl = ramp[bgIdx];
  bgColor = to565(hslToRgb(bgHsl.h, bgHsl.s, bgHsl.l));

  nColors = 0;
  for (int i = 0; i < RAMP_N; i++) {
    if (sameFormatted(ramp[i], bgHsl)) continue;
    colors[nColors++] = to565(hslToRgb(ramp[i].h, ramp[i].s, ramp[i].l));
  }

  nNodes = 0;
  for (int i = 0; i < LAYERS; i++)
    createNodes(GRID[i][0], GRID[i][1], DIR);

  Serial.printf("centerHue %.2f  hueCycle %.2f\n", centerHue, hueCycle);
  Serial.printf("background hsl(%.2f, %.2f%%, %.2f%%)  %d colours  %d nodes\n",
                bgHsl.h, clampf(bgHsl.s, 0.0f, 1.0f) * 100.0f,
                clampf(bgHsl.l, 0.0f, 1.0f) * 100.0f, nColors, nNodes);

  // Deliberately no playhead reset here — see the file header.
  drawnPlayhead = -1.0f;
}

// ---------------------------------------------------------------------------
// The knob
// ---------------------------------------------------------------------------
// Absolute, not relative: the fractional part of the cumulative revolution
// count IS the playhead, so the wave always sits where the knob points, the
// way zhi.cpp's wave sits under the finger. floorf handles both directions
// (fmodf would hand back a negative fraction for anticlockwise turns).
static inline float knobPlayhead() {
  const float rev = encoderRev();
  return rev - floorf(rev);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }
  touchBegin();
  encoderBegin();

  generate(newSeed());
  playhead = knobPlayhead();

  const uint32_t t0 = wallMicros();
  renderAll(playhead);
  const uint32_t drawUs = wallMicros() - t0;
  present();
  drawnPlayhead = playhead;
  Serial.printf("first frame drawn in %luus\n", (unsigned long)drawUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // encoderRev() is pinned at 0 with no knob, so a capture would otherwise be
  // SKETCH_FRAMES copies of playhead 0. Sweep it instead: setup() already
  // wrote frame 0, so the loop covers 1/(N-1) .. 1 and one run shows the whole
  // breath.
  static int frame = 1;
  playhead = SKETCH_FRAMES > 1
               ? clampf((float)frame / (float)(SKETCH_FRAMES - 1), 0.0f, 1.0f)
               : 0.0f;
  frame++;
  renderAll(playhead);
  present();
  delay(16);
#else
  playhead = knobPlayhead();

  if (tapDetected()) {
    generate(newSeed());
    renderAll(playhead);
    present();
    drawnPlayhead = playhead;
    return;
  }

  // Idle frames must not push: the composition holds wherever the knob rests.
  // The encoder count is quantised, so exact comparison is the change test —
  // no epsilon needed.
  if (playhead != drawnPlayhead) {
    const uint32_t t0 = wallMicros();
    renderAll(playhead);
    const uint32_t drawUs = wallMicros() - t0;
    present();
    drawnPlayhead = playhead;

    static uint32_t reportAt = 0;
    if (millis() - reportAt >= 1000) {
      reportAt = millis();
      Serial.printf("playhead %.3f  draw %luus\n", playhead, (unsigned long)drawUs);
    }
  } else {
    delay(8);
  }
#endif
}
