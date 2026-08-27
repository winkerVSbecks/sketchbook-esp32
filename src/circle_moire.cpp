// ============================================================================
// circle moire — concentric stripe rings under a knob-driven playhead
// ============================================================================
//   pio run -e moire_rk_native -t exec     SDL window (drag the mouse = turn)
//   pio run -e moire_rk        -t upload   the board
//   pio run -e moire_rk_shot               headless PNG capture, then run it
//
// Port of sketchbook/circle-moire.js. The JS stacks 19 filled circles, largest
// first, radii 5..95 (x scale), alternating between two vertical-stripe
// patterns offset from each other by half a stripe period — so what it
// actually draws is 19 concentric rings, each one stripe-period wide, whose
// stripes are phase-inverted against their neighbours'. The animation slides
// both patterns sideways by exactly one period per loop, which is what makes
// the wrap seamless; here the knob is the clock — one revolution slides the
// stripes one period. The JS's translate(w/2, h/2) before filling anchors the
// pattern space at the canvas centre, and the port keeps that anchoring.
//
// Per the user's spec the outer circle fills the screen: R = 120 on the round
// 240x240 glass, so the outermost ring's rim sits exactly at the bezel. The
// JS's outer circle stops at 88% of its canvas; that inset is the one
// deliberate divergence.
//
// What the port reduced (all reported against the JS):
//   - The 19 circles overdraw each other entirely: the composition is 19
//     annuli, each 5*scale wide. Ring j (counting from the centre, 1-based)
//     shows pattern (j odd ? 1 : 0) — the whole piece is a per-pixel function
//     of (distance from centre, x), which is how it renders here: no circles,
//     no patterns, one pass over the framebuffer.
//   - createPattern is rebuilt twice per frame in the JS (two throwaway
//     canvases per render); here the per-frame work is one 240-entry column
//     table of stripe coverage.
//   - render() calls clearRect and then fillRect over the same full canvas
//     with the same effective result — the clearRect is dead.
//   - begin({}) is empty, the pattern canvas's 1000px height on a 1080px
//     canvas just makes the pattern tile vertically (invisible: the stripes
//     are y-uniform), and every circle's cx/cy is 0. All dead weight, not
//     ported.
//   - There is NO randomness in this piece: fixed palette, fixed geometry.
//     So unlike zhi_knob/isolines there is nothing for a tap (or RESET) to
//     vary — touch.h is deliberately not included, and the knob is the only
//     input. Nothing runs on a clock — see CLAUDE.md, "The two buttons".
//
// Rendering: ring parity and outer-rim coverage depend only on geometry, so
// setup() bakes them per-pixel — parity blended 1px across each ring boundary
// and the rim, i.e. analytic AA on every curve — into a W*H*2 mask in PSRAM
// (115KB, walked strictly row-sequential each frame, which is the access
// shape PSRAM's cache serves well; see skeleton_line.cpp for the precedent).
// Stripe edges get the same treatment in x via exact pixel-interval coverage
// in the per-frame column table. Every pixel then blends bg->fg through one
// 256-entry LUT, byte-swapped for direct sprite writes (see isolines.cpp).
// ============================================================================

#define SKETCH_TITLE  "circle moire"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/color.h"
#include "shared/encoder.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const uint32_t BG = 0x3333FF;   // clrs.bg
static const uint32_t FG = 0xFFFFFF;   // clrs.foreground

static const int   RINGS = 19;                    // circles.length
static const float R     = (float)W * 0.5f;       // outer circle fills the glass
static const float RING  = R / (float)RINGS;      // 5 * scale in the JS
static const float PERIOD = 2.0f * RING;          // stripe period, 10 * scale

static const float CX = (float)W * 0.5f;
static const float CY = (float)H * 0.5f;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Per pixel, baked once: [0] weight toward pattern 1 (ring parity, AA'd across
// ring boundaries), [1] coverage inside the outer circle (AA'd at the rim).
static uint8_t *mask = nullptr;

// bg -> fg blend ramp, pre-swapped for raw 16bpp sprite writes.
static uint16_t lut[256];

// Per frame: fg coverage of pattern 0 at each pixel column.
static uint8_t colCov[W];

static float playhead      = 0.0f;
static float drawnPlayhead = -1.0f;   // last value actually pushed to the panel

// ---------------------------------------------------------------------------
// Geometry bake
// ---------------------------------------------------------------------------
// Ring j (1-based, j = ceil(d / RING)) wears pattern (j & 1): the JS's circle
// at radius 5*scale*j has fill = j odd ? 1 : 0, and the smallest circle
// covering a pixel is the one that shows. AA: blend parity linearly over the
// 1px straddling each boundary; boundary 19 is the rim, which the coverage
// byte owns, so parity stops transitioning there (rings past 18 read as 19).
static void bakeMask() {
  uint8_t *m = mask;
  for (int py = 0; py < H; py++) {
    const float dy = ((float)py + 0.5f) - CY;
    for (int px = 0; px < W; px++, m += 2) {
      const float dx = ((float)px + 0.5f) - CX;
      const float d  = sqrtf(dx * dx + dy * dy);

      const int kb = (int)lroundf(d / RING);   // nearest ring boundary
      float w;
      if (kb < 1 || kb > RINGS - 1) {
        w = 1.0f;                              // ring 1 and ring 19: pattern 1
      } else {
        const float a = clampf(d - (float)kb * RING + 0.5f, 0.0f, 1.0f);
        w = (kb & 1) ? 1.0f - a : a;           // inner ring kb -> outer kb+1
      }
      m[0] = (uint8_t)(w * 255.0f + 0.5f);
      m[1] = (uint8_t)(clampf(R - d + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
}

// ---------------------------------------------------------------------------
// Stripes
// ---------------------------------------------------------------------------
// Pattern 0's fg lives on [RING, 2*RING) mod PERIOD of (x - offset) in
// centre-anchored coordinates; pattern 1 is its exact inverse. stripeF is the
// integral of that indicator, so a pixel's coverage is one difference — exact
// AA on the stripe edges.
static inline float stripeF(float t) {
  const float per = floorf(t / PERIOD);
  const float f   = t - per * PERIOD;
  return per * RING + clampf(f - RING, 0.0f, RING);
}

static void computeColumns(float offset) {
  // Shift far positive so stripeF never sees a negative argument; 32 periods
  // clears the worst case (|x - CX| + |offset| < 2.5 periods here).
  const float shift = 32.0f * PERIOD;
  for (int x = 0; x < W; x++) {
    const float p = ((float)x + 0.5f) - CX - offset + shift;
    colCov[x] = (uint8_t)((stripeF(p + 0.5f) - stripeF(p - 0.5f)) * 255.0f + 0.5f);
  }
}

// ---------------------------------------------------------------------------
static uint32_t frameUs = 0;

static void renderAll(float ph) {
  // playhead * -10 * scale in the JS: one loop = one period, wrap seamless.
  computeColumns(ph * -PERIOD);

  const uint32_t t0 = wallMicros();
  uint16_t *fb = (uint16_t *)cv.getBuffer();
  const uint8_t *m = mask;
  for (int py = 0; py < H; py++) {
    uint16_t *row = fb + (size_t)py * (size_t)W;
    for (int px = 0; px < W; px++, m += 2) {
      const uint32_t w  = m[0];
      const uint32_t c0 = colCov[px];
      // fg coverage: mix the two (mutually inverse) patterns by ring parity,
      // then gate by the outer-circle coverage.
      const uint32_t mix = (c0 * (255u - w) + (255u - c0) * w + 127u) / 255u;
      row[px] = lut[(mix * m[1] + 127u) / 255u];
    }
  }
  frameUs = wallMicros() - t0;
}

// ---------------------------------------------------------------------------
// The knob — see zhi_knob.cpp: the fractional part of the cumulative
// revolution count IS the playhead, both directions, wrap seamless.
// ---------------------------------------------------------------------------
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
  encoderBegin();

  mask = (uint8_t *)psramAlloc((size_t)W * (size_t)H * 2u);
  if (!mask) {
    Serial.println("FATAL: mask allocation failed");
    while (true) delay(1000);
  }
  bakeMask();

  for (int i = 0; i < 256; i++) {
    const uint16_t c = blend565(BG, FG, (float)i / 255.0f);
    lut[i] = (uint16_t)((c >> 8) | (c << 8));
  }

  Serial.printf("\n%s: R %.1f  ring %.2fpx  period %.2fpx\n",
                SKETCH_TITLE, R, RING, PERIOD);

  playhead = knobPlayhead();
  renderAll(playhead);
  present();
  drawnPlayhead = playhead;
  Serial.printf("first frame: %luus\n", (unsigned long)frameUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // encoderRev() is pinned at 0 with no knob; sweep the playhead instead so
  // one capture run shows the whole loop. setup() wrote frame 0, so the loop
  // covers 1/(N-1) .. 1 — and because the loop is exact, the last frame must
  // be byte-identical to the first: that's the wrap check.
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

  // Idle frames must not push: the composition holds wherever the knob rests.
  // The encoder count is quantised, so exact comparison is the change test.
  if (playhead != drawnPlayhead) {
    renderAll(playhead);
    present();
    drawnPlayhead = playhead;

    static uint32_t reportAt = 0;
    if (millis() - reportAt >= 1000) {
      reportAt = millis();
      Serial.printf("playhead %.3f  frame %luus\n",
                    playhead, (unsigned long)frameUs);
    }
  } else {
    delay(8);
  }
#endif
}
