// ============================================================================
// chill wave — a paper-on-ink squiggle whose ends ride the wave, knob-driven
// ============================================================================
//   pio run -e chill_rk_native -t exec     SDL window (drag the mouse = turn)
//   pio run -e chill_rk        -t upload   the board
//   pio run -e chill_rk_shot               headless PNG capture, then run it
//
// Port of sketchbook/chill-wave.js. The JS builds a 15-half-period bezier wave
// (the m = 0.512286... half-sine constant), strokes it with a dash pattern
// [7L, 2L, 0, 7L] whose offset slides by -2L*playhead while the whole path
// translates left by 2a*playhead — and those two motions cancel in screen
// space: what you see is a 7-half-period wave segment whose END CAPS SIT AT
// FIXED x while the wave slides through them. The crests travel, the round
// caps bob one full cycle per loop, and the loop wraps because one loop is
// exactly one period of travel. Here the knob is the clock, geared 4x: one
// revolution = four loops, both directions, wrap seamless (see knobPlayhead).
//
// What the port reduced (all reported against the JS):
//   - The zero-length dash entry draws a round dot 2L past the wave's end —
//     at FIXED screen x = 220 (the path translation cancels there too) — and
//     the masking fillRect starts at w*0.7 = 182: the dot is always under the
//     mask, and the wave ends at x = 140, short of it. So the dot and the
//     mask exactly annihilate; neither is ported.
//   - clearRect immediately under fillRect over the same canvas: dead.
//   - w = 260 never sizes the wave (the visible span is steps*a = 280); its
//     only live use was positioning that dead mask.
//   - `-(1 - a) * m` in the control points is a suspected typo for
//     `-(1 - m) * a` (19.98 vs 19.51 at a = 40 — indistinguishable). Ported
//     as written. One consequence kept too: segment 0's first control differs
//     from the reflected controls of segments 1..14 (20.49 vs 20.02), and the
//     dash unit L is measured on segment 0 alone, so the wrap is sub-pixel
//     off exact — in the JS as well. Don't expect the last capture frame to
//     be byte-identical to the first.
//
// Colors are clrs(): background = a random riso ink, the wave = the paper
// tone the ink was contrast-filtered against (>= 3:1 WCAG, black excluded) —
// ink and paper deliberately swapped, per the JS. That's the piece's one
// random draw, so a tap redraws it; the geometry never changes. Nothing runs
// on a clock — see CLAUDE.md, "The two buttons". generate() does not reset
// the playhead, for zhi_knob's reason: the physical knob doesn't move when
// the colors change.
//
// Rendering: the wave is x-monotone, so the visible window is extracted from
// one baked polyline by arc length and stroked per-pixel as a distance field —
// point-distance at the ends IS the round cap, segment search pruned to a
// sliding x-window. The band is ~180x40px; everything else is one fillScreen.
//
// The JS scales the wave to 280/1080 of its canvas (~26%). On a 1.28" round
// glass that read as a speck, so the one deliberate divergence: the span is
// SPAN_FRAC of the panel width. Everything else keeps the JS's proportions
// (stroke, amplitude, and period all scale together).
// ============================================================================

#define SKETCH_TITLE  "chill wave"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/touch.h"
#include "shared/encoder.h"

// ---------------------------------------------------------------------------
// Config — wave space is the JS's coordinate frame (a = 40), scaled at the end
// ---------------------------------------------------------------------------
static const float A      = 40.0f;                  // half-period: x-span and peak-to-trough
static const float M      = 0.512286623256592433f;  // the JS's magic constant, verbatim
static const int   STEPS  = 7;                      // visible half-periods
static const int   SHIFT  = 2;                      // half-periods of travel per loop
static const float LINE_W = 12.0f;                  // stroke width, wave space

static const float SPAN_FRAC = 0.75f;               // wave span / panel width (the divergence)
static const float SCALE = ((float)W * SPAN_FRAC) / ((float)STEPS * A);
static const float CX = (float)W * 0.5f;
static const float CY = (float)H * 0.5f;            // JS: wave midline at canvas centre
static const float RADIUS = LINE_W * 0.5f * SCALE;  // stroke radius, pixels

// ---------------------------------------------------------------------------
// clrs() — riso-colors and paper-colors, as the JS's clrs.js uses them
// ---------------------------------------------------------------------------
static const uint32_t RISO[] = {
  0x000000, 0x914E72, 0x0078BF, 0x00A95C, 0x3255A4, 0xF15060, 0x3D5588,
  0x765BA7, 0x00838A, 0xBB8B41, 0x407060, 0xFF665E, 0x925F52, 0xFFE800,
  0xD2515E, 0xFF6C2F, 0xFF48B0, 0x88898A, 0xAC936E, 0xE45D50, 0xFF7477,
  0x62A8E5, 0x4982CF, 0x0074A2, 0x235BA8, 0x484D7A, 0x435060, 0xD5E4C0,
  0xA5AAA8, 0x70747C, 0x5F8289, 0x375E77, 0x5E695E, 0x00AA93, 0x19975D,
  0x397E58, 0x516E5A, 0x4A635D, 0x68724D, 0x62C2B1, 0x67B346, 0x009DA5,
  0x169B62, 0x237E74, 0x2F6165, 0x9D7AD2, 0xAA60BF, 0x845991, 0x775D7A,
  0x6C5D80, 0xF65058, 0xD2515E, 0xD1517A, 0x9E4C6E, 0xD1517A, 0xA75154,
  0xE3ED55, 0xFFB511, 0xFFAE3B, 0xF6A04D, 0xEE7F4B, 0xFF6F4C, 0xB49F29,
  0xBA8032, 0xBD6439, 0x8E595A, 0xF2CDCF, 0xF984CA, 0xE6B5C9, 0xBD8CA6,
  0x914E72, 0x928D88, 0xFFFFFF, 0x5EC8E5, 0x82D8D5, 0xFFE900, 0xFF4C65,
  0x44D62C,
};
static const int N_RISO = (int)(sizeof(RISO) / sizeof(RISO[0]));

static const uint32_t PAPER[] = {
  0xF2C5D2, 0xE5BB57, 0x9C96CD, 0x76B995, 0x70A7C5, 0xF8E6D1,
  0xDFBCAB, 0xF1E7E1, 0xEFEDF6, 0xE1DCE9, 0xCDCAD5, 0xF2F2F2,
};
static const int N_PAPER = (int)(sizeof(PAPER) / sizeof(PAPER[0]));

static const float MIN_CONTRAST = 3.0f;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// The path, baked once (geometry has no randomness): 15 cubic segments,
// SUB samples each, in wave space at playhead 0. Playhead only translates it.
static const int SUB    = 24;
static const int N_SEGS = STEPS * 2 + 1;
static const int N_FULL = N_SEGS * SUB + 1;
static float fx[N_FULL], fy[N_FULL];   // sample points
static float slen[N_FULL];             // cumulative arc length
static float L0 = 0.0f;                // dash unit: segment 0's length, as getTotalLength

// The visible window, rebuilt per frame, screen coordinates. x-monotone.
static float vx[N_FULL], vy[N_FULL];
static int   nV = 0;

static uint16_t bg565, lut[256];       // bg -> fg coverage ramp
static uint32_t bgRGB, fgRGB;

static float playhead      = 0.0f;
static float drawnPlayhead = -1.0f;    // last value actually pushed to the panel

// ---------------------------------------------------------------------------
// The path bake
// ---------------------------------------------------------------------------
// Segment 0 is the JS's 'c' (first control a*m out, horizontal); segments 1..14
// are 's' commands, first control = reflection of the previous second control.
// All second controls are ((a-1)*m, +-a) — the suspected typo, kept.
static void bakeWave() {
  float px = -0.5f * (float)STEPS * A;   // the 'M', minus the playhead term
  float py = A * 0.5f;
  float pc2x = 0.0f, pc2y = 0.0f;
  int   n = 0;

  fx[0] = px; fy[0] = py;
  for (int k = 0; k < N_SEGS; k++) {
    const float dir = (k & 1) ? A : -A;  // first half-period rises
    const float c1x = (k == 0) ? px + A * M : 2.0f * px - pc2x;
    const float c1y = (k == 0) ? py         : 2.0f * py - pc2y;
    const float c2x = px + (A - 1.0f) * M;
    const float c2y = py + dir;
    const float p3x = px + A;
    const float p3y = py + dir;

    for (int j = 1; j <= SUB; j++) {
      const float t = (float)j / (float)SUB, u = 1.0f - t;
      const float b0 = u * u * u, b1 = 3.0f * u * u * t, b2 = 3.0f * u * t * t, b3 = t * t * t;
      fx[n + j] = b0 * px + b1 * c1x + b2 * c2x + b3 * p3x;
      fy[n + j] = b0 * py + b1 * c1y + b2 * c2y + b3 * p3y;
    }
    pc2x = c2x; pc2y = c2y;
    px = p3x; py = p3y;
    n += SUB;
  }

  slen[0] = 0.0f;
  for (int i = 1; i < N_FULL; i++) {
    const float dx = fx[i] - fx[i - 1], dy = fy[i] - fy[i - 1];
    slen[i] = slen[i - 1] + sqrtf(dx * dx + dy * dy);
  }
  L0 = slen[SUB];
}

// ---------------------------------------------------------------------------
// The visible window
// ---------------------------------------------------------------------------
// Dash phase s - 2*L0*ph is visible on [0, 7*L0): window [2*L0*ph, 2*L0*ph +
// 7*L0] of the path, which itself sits 2*A*ph left of its bake position.
// Endpoints are interpolated exactly — they are the caps.
static inline float toScreenX(float x, float ph) { return CX + (x - (float)SHIFT * A * ph) * SCALE; }
static inline float toScreenY(float y)           { return CY + y * SCALE; }

static void buildVisible(float ph) {
  const float s0 = (float)SHIFT * L0 * ph;
  const float s1 = s0 + (float)STEPS * L0;

  nV = 0;
  int i = 0;
  while (i < N_FULL - 1 && slen[i + 1] <= s0) i++;

  { // start cap, interpolated into sample interval i
    const float t = (slen[i + 1] > slen[i]) ? (s0 - slen[i]) / (slen[i + 1] - slen[i]) : 0.0f;
    vx[nV] = toScreenX(fx[i] + (fx[i + 1] - fx[i]) * t, ph);
    vy[nV] = toScreenY(fy[i] + (fy[i + 1] - fy[i]) * t);
    nV++;
  }
  while (i + 1 < N_FULL && slen[i + 1] < s1) {
    i++;
    vx[nV] = toScreenX(fx[i], ph);
    vy[nV] = toScreenY(fy[i]);
    nV++;
  }
  { // end cap
    const int j = (i + 1 < N_FULL) ? i : N_FULL - 2;
    const float t = (slen[j + 1] > slen[j]) ? (s1 - slen[j]) / (slen[j + 1] - slen[j]) : 1.0f;
    vx[nV] = toScreenX(fx[j] + (fx[j + 1] - fx[j]) * t, ph);
    vy[nV] = toScreenY(fy[j] + (fy[j + 1] - fy[j]) * t);
    nV++;
  }
}

// ---------------------------------------------------------------------------
// The stroke — distance field over the polyline
// ---------------------------------------------------------------------------
static inline float segDist2(float qx, float qy, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 0.0f ? ((qx - ax) * dx + (qy - ay) * dy) / len2 : 0.0f;
  t = clampf(t, 0.0f, 1.0f);
  const float ex = ax + t * dx - qx, ey = ay + t * dy - qy;
  return ex * ex + ey * ey;
}

static uint32_t frameUs = 0;

static void renderAll(float ph) {
  const uint32_t t0 = wallMicros();
  buildVisible(ph);
  cv.fillScreen(bg565);

  const float RQ = RADIUS + 1.0f;        // coverage is 0 past RADIUS + 0.5

  int x0 = (int)floorf(vx[0] - RQ),      x1 = (int)ceilf(vx[nV - 1] + RQ);
  if (x0 < 0) x0 = 0;
  if (x1 > W - 1) x1 = W - 1;

  // Sliding window of segments whose x-interval can reach this column. The
  // polyline is x-monotone, so both indices only ever advance.
  int iA = 0, iB = 0;
  for (int pxi = x0; pxi <= x1; pxi++) {
    const float qx = (float)pxi + 0.5f;
    while (iA < nV - 2 && vx[iA + 1] <= qx - RQ) iA++;
    while (iB < nV - 2 && vx[iB + 1] <  qx + RQ) iB++;

    float yMin = vy[iA], yMax = vy[iA];
    for (int i = iA + 1; i <= iB + 1; i++) {
      if (vy[i] < yMin) yMin = vy[i];
      if (vy[i] > yMax) yMax = vy[i];
    }
    int py0 = (int)floorf(yMin - RQ), py1 = (int)ceilf(yMax + RQ);
    if (py0 < 0) py0 = 0;
    if (py1 > H - 1) py1 = H - 1;

    for (int pyi = py0; pyi <= py1; pyi++) {
      const float qy = (float)pyi + 0.5f;
      float d2 = 1e30f;
      for (int i = iA; i <= iB; i++) {
        const float d = segDist2(qx, qy, vx[i], vy[i], vx[i + 1], vy[i + 1]);
        if (d < d2) d2 = d;
      }
      const float cover = clampf(RADIUS + 0.5f - sqrtf(d2), 0.0f, 1.0f);
      if (cover > 0.0f)
        cv.drawPixel(pxi, pyi, lut[(int)(cover * 255.0f + 0.5f)]);
    }
  }
  frameUs = wallMicros() - t0;
}

// ---------------------------------------------------------------------------
// clrs() — background from paper, ink filtered to >= 3:1 against it, black
// excluded; this piece then paints the INK as the field and the PAPER as the
// wave. Same draw order as the JS (paper first, then the ink pick).
// ---------------------------------------------------------------------------
static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  const uint32_t paper = PAPER[rngIndex(N_PAPER)];

  uint8_t ink[N_RISO];
  int     nInk = 0, best = 0;
  float   bestC = 0.0f;
  for (int i = 0; i < N_RISO; i++) {
    if (RISO[i] == 0x000000) continue;
    const float c = wcagContrast(paper, RISO[i]);
    if (c > bestC) { bestC = c; best = i; }
    if (c >= MIN_CONTRAST) ink[nInk++] = (uint8_t)i;
  }
  // The JS would hand Random.pick an empty list and paint `undefined`; on
  // device, fall back to the highest-contrast ink instead. Unreached with
  // these tables (every paper is light), but see porting-patterns.md.
  bgRGB = nInk > 0 ? RISO[ink[rngIndex(nInk)]] : RISO[best];
  fgRGB = paper;

  bg565 = to565(bgRGB);
  for (int i = 0; i < 256; i++)
    lut[i] = blend565(bgRGB, fgRGB, (float)i / 255.0f);

  Serial.printf("ink #%06lX on paper #%06lX  (%d inks passed)\n",
                (unsigned long)bgRGB, (unsigned long)fgRGB, nInk);

  // Deliberately no playhead reset — see zhi_knob.cpp.
  drawnPlayhead = -1.0f;
}

// ---------------------------------------------------------------------------
// The knob — see zhi_knob.cpp: the fractional part of the cumulative
// revolution count IS the playhead, both directions, wrap seamless. Geared
// 4x here (the moire treatment): at 1x a whole revolution slid the crests
// one period — 51px — and the knob felt disconnected from the piece; 8x
// jumped a quarter loop per detent and felt too fast on hardware.
// ---------------------------------------------------------------------------
static const float KNOB_CYCLES_PER_REV = 4.0f;

static inline float knobPlayhead() {
  const float t = encoderRev() * KNOB_CYCLES_PER_REV;
  return t - floorf(t);
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

  bakeWave();
  generate(newSeed());

  Serial.printf("%s: span %.0fpx  stroke %.1fpx  L0 %.2f (x%.2f scale)\n",
                SKETCH_TITLE, (float)STEPS * A * SCALE, RADIUS * 2.0f, L0, SCALE);

  playhead = knobPlayhead();
  renderAll(playhead);
  present();
  drawnPlayhead = playhead;
  Serial.printf("first frame: %luus\n", (unsigned long)frameUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // encoderRev() is pinned at 0 with no knob; sweep the playhead so one run
  // shows the whole loop. setup() wrote frame 0, so this covers 1/(N-1) .. 1.
  // (Unlike moire, don't diff the last frame against the first: the wrap is
  // sub-pixel inexact by construction — see the header.)
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
