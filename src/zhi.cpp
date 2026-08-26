// ============================================================================
// zhi (ਜ਼ਹੀ) — five stacked grids of breathing rectangles, scrubbed by finger
// ============================================================================
//   pio run -e zhi_t2_native -t exec     SDL window (drag the mouse to scrub)
//   pio run -e zhi_t2        -t upload   the board
//   pio run -e zhi_t2_shot               headless PNG capture, then run the binary
//
// Five grids of increasing subdivision are stacked over a light ground. Every
// cell holds one solid rect that shrinks and grows about the cell's centre; the
// phase of that breath is the cell's distance from the top-left corner, signed
// per layer, so each grid opens and closes as a diagonal wave. Colour is a 2x2
// scheme per layer, indexed by column and row parity — which is what reads as a
// checkerboard once half the cells have shrunk away.
//
// The original is an fxhash piece on a 3s loop. Here the loop is a gesture:
// the finger's y position is the playhead, one loop over the panel height,
// and it holds where you lift. A tap draws a whole new piece. Nothing
// runs on a clock — see CLAUDE.md, "The two buttons".
// ============================================================================

#define SKETCH_TITLE  "zhi"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/touch.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// The JS grids are [cols, rows] tuned for a 2048x1152 landscape canvas:
// [5,5] [10,5] [20,5] [30,10] [40,20]. Transposed here so the fine subdivision
// runs along this panel's long axis and the coarse layers still read as bands
// rather than as a single row of slabs.
static const int LAYERS = 5;
static const int GRID[LAYERS][2] = {
  {  5,  5 },
  {  5, 10 },
  {  5, 20 },
  { 10, 30 },
  { 20, 40 },
};

// Odd columns drop their first and last row, so a layer is
// ceil(cols/2)*rows + floor(cols/2)*(rows-2). For the grids above that is
// 21 + 46 + 96 + 290 + 780 = 1233.
static const int MAX_NODES = 1300;

// Direction sets, one sign per layer. Named in the original as fxhash features.
static const int8_t DIRECTIONS[4][LAYERS] = {
  {  1, -1,  1, -1,  1 },   // Cross Over A
  { -1,  1, -1,  1, -1 },   // Cross Over B
  { -1, -1, -1, -1, -1 },   // Uniform left
  {  1,  1,  1,  1,  1 },   // Uniform right
};
static const char *DIRECTION_NAMES[4] = {
  "Cross Over A", "Cross Over B", "Uniform left", "Uniform Right",
};

// Gesture classification. A press is a tap until it moves TAP_SLOP px or
// outlives TAP_MS; after that it is a scrub for the rest of its life.
static const uint32_t TAP_MS   = 250;
static const int      TAP_SLOP = 8;

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

// Random.pick(colors). The `bg` argument the JS passes to its `clr()` is
// ignored there — the helper takes no parameters — so it is dead code, not a
// contrast test that got lost. Ported as written.
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
  const int   dirSet    = rngIndex(4);

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
    createNodes(GRID[i][0], GRID[i][1], DIRECTIONS[dirSet][i]);

  Serial.printf("centerHue %.2f  hueCycle %.2f  directions %s\n",
                centerHue, hueCycle, DIRECTION_NAMES[dirSet]);
  Serial.printf("background hsl(%.2f, %.2f%%, %.2f%%)  %d colours  %d nodes\n",
                bgHsl.h, clampf(bgHsl.s, 0.0f, 1.0f) * 100.0f,
                clampf(bgHsl.l, 0.0f, 1.0f) * 100.0f, nColors, nNodes);

  playhead      = 0.0f;
  drawnPlayhead = -1.0f;
}

// ---------------------------------------------------------------------------
// Gesture
// ---------------------------------------------------------------------------
// One finger does both jobs, so a press has to be classified before it is
// allowed to move anything: a tap that scrubbed on the way past would fight the
// reseed it was asking for. A press starts undecided and stays that way until
// it moves TAP_SLOP px or outlives TAP_MS, whichever comes first — from then on
// it is a scrub, and only a press that reaches its release still undecided
// reseeds.
//
// Returns true if the gesture asked for a new composition.
static bool pollTouch() {
  static bool     down    = false;
  static bool     scrub   = false;
  static uint32_t startAt = 0;
  static int      startX = 0, startY = 0;
  int tx = 0, ty = 0;
  const bool now = touchPoint(&tx, &ty);

  if (now && !down) {
    down    = true;
    scrub   = false;
    startAt = millis();
    startX  = tx;
    startY  = ty;
    return false;
  }

  if (now) {
    if (!scrub) {
      const int dx = tx - startX, dy = ty - startY;
      if (dx * dx + dy * dy >= TAP_SLOP * TAP_SLOP || millis() - startAt >= TAP_MS) scrub = true;
    }
    // Absolute: the finger's y position IS the playhead, one loop over the
    // panel height, so the wave always sits where the finger is — relative
    // tracking read as disconnected from the touch. |cos(pi*p - d)| has
    // period 1 in p, so the top and bottom edges meet the same frame.
    if (scrub) playhead = H > 1 ? clampf((float)ty / (float)(H - 1), 0.0f, 1.0f) : 0.0f;
    return false;
  }

  if (down) {
    down = false;
    return !scrub;                       // a short, still press is a tap
  }
  return false;
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

  generate(newSeed());

  const uint32_t t0 = wallMicros();
  renderAll(playhead);
  const uint32_t drawUs = wallMicros() - t0;
  present();
  drawnPlayhead = playhead;
  Serial.printf("first frame drawn in %luus\n", (unsigned long)drawUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // touchPoint() is always false with no window, so a capture would otherwise
  // be SKETCH_FRAMES copies of playhead 0. Sweep it instead: setup() already
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
  if (pollTouch()) {
    generate(newSeed());
    renderAll(playhead);
    present();
    drawnPlayhead = playhead;
    return;
  }

  // Idle frames must not push: a full 240x320 blit is ~31ms of SPI on this
  // board, and the composition holds between gestures.
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
