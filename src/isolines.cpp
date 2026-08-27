// ============================================================================
// isolines — noise-field contours under a knob-driven playhead
// ============================================================================
//   pio run -e iso_rk_native -t exec     SDL window (drag the mouse = turn)
//   pio run -e iso_rk        -t upload   the board
//   pio run -e iso_rk_shot               headless PNG capture, then run it
//
// Port of sketchbook/isolines/isolines.js restyled per chromatic-noise.js:
// a simplex-noise field sliced into 12 bands, the band boundaries stroked in
// four passes — red, green, blue, then white — each pass sampling the field
// at a slightly earlier time, so the RGB passes fringe out from under the
// white one like chromatic aberration. The JS animates a loop; here the knob
// is the clock — one full revolution is one loop. Each pass's z coordinate
// is sin(pi * playhead - offset) * 0.5, which returns to its start at both
// ends of a turn, so the wrap at each full revolution is seamless in either
// direction.
//
// Unlike the JS (which clips the polylines to a 4% margin box), the field is
// full-bleed: the round bezel is the frame and the contours run to the
// glass. The label is isolines.js's, moved to a centred plate per spec —
// chromatic-noise.js itself has none.
//
// What the port reduced (all reported against the JS):
//   - The JS strokes MarchingSquaresJS.isoBands polygons. A band's outline is
//     the contour at its lower level, the contour at its upper level, and the
//     data-rect border where the band touches it — and consecutive bands share
//     levels, so every interior level is stroked twice and the border strokes
//     are then masked by drawShell's black rect (lineWidth 14 over their 12).
//     Marching-squares isolines at the 11 interior levels produce the same
//     pixels once, with nothing to mask — and with the shell gone entirely
//     (full-bleed, above), the black rect has no trim job left either.
//   - drawIsolines computes `tileSize` (from width, not size) and a UV-to-
//     pixel point `t`, and uses neither. Dead code, not ported.
//   - drawLabel computes a beat() opacity whose use is commented out — the
//     label draws at full white. Ported as it behaves, not as it intends.
//   - The label's angle is |noise1D(playhead)|, a 0..1 noise sample wearing a
//     degree sign — not an angle. It also jumps at the loop wrap in the JS
//     (noise1D(1) != noise1D(0)); the knob inherits that hiccup faithfully.
//
// The label moves per the user's spec: centred on the panel (the JS puts it
// bottom-left, which the round bezel would crop), on a black plate with a
// white border, so it reads over whatever the contours are doing beneath it.
//
// A tap on the glass draws a whole new field (Random.permuteNoise() again, in
// effect). Like zhi_knob, generate() does not touch the playhead — the knob
// didn't move, so the piece shouldn't. Nothing runs on a clock — see
// CLAUDE.md, "The two buttons". SDL has no reseed stand-in (the mouse is the
// knob): relaunch the binary.
// ============================================================================

#define SKETCH_TITLE  "isolines"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/noise.h"
#include "shared/color.h"
#include "shared/touch.h"
#include "shared/encoder.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// Stroke width rides the canvas scale: chromatic-noise.js draws lineWidth 24
// on a 1080-high canvas, so 240 / 1080 puts it at ~5.3px here — the chunky
// stroke is the style.
static const float LINE_R = 12.0f * (float)H / 1080.0f;  // drawWideLine radius

// The field: gridSize in the JS — sampling resolution, not composition.
// 64 samples over the 240px panel is 3.75px per cell under 5.3px strokes;
// the crossing points are interpolated, so curves stay smooth, and both the
// noise sweep and the segment count scale with this. (The JS runs 256 cells
// over 1920px — 7.5px cells under 24px lines — so this is proportionally
// finer than the original.)
static const int GRID = 64;

// chromatic-noise.js samples at x / (gridSize * 0.75): 1/0.75 noise units
// across the panel.
static const float NOISE_SPAN = 1.0f / 0.75f;

// The four passes, in draw order — white last so it sits on top and the RGB
// fringes peek out where the time offsets have moved the field. The JS's
// `off = 0.005`, as phase offsets into the sin.
static const float TIME_OFF = 0.005f * (float)M_PI;

// linspace(12): thresholds i/12. Level 0 has nothing below it to bound, so
// the drawn levels are 1..11 — exactly the bands' interior boundaries.
static const int LEVELS = 12;

static const uint16_t BG    = to565(0x131217);
static const uint16_t FG    = to565(0xFFFFFF);
static const uint16_t BLACK = to565(0x000000);

struct Pass { float timeOff; uint32_t color; };   // colour as 0xRRGGBB
static const Pass PASSES[4] = {
  { 3.0f * TIME_OFF, 0xFF0000 },
  { 2.0f * TIME_OFF, 0x00FF00 },
  { 1.0f * TIME_OFF, 0x0000FF },
  { 0.0f,            0xFFFFFF },
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Two field evaluations bracket the four passes: the red pass's time and the
// white pass's differ by 3 * TIME_OFF in phase — at most ~0.02 noise units in
// z — so the green and blue fields are linear interpolations of the outer two
// (sub-pixel contour error) instead of two more full noise sweeps.
static float fieldA[GRID][GRID];   // at the red pass's time (earliest)
static float fieldB[GRID][GRID];   // at the white pass's time (latest)

static float playhead      = 0.0f;
static float drawnPlayhead = -1.0f;   // last value actually pushed to the panel

// ---------------------------------------------------------------------------
// The field
// ---------------------------------------------------------------------------
// simplex.noise3D(x/scale, y/scale, time * 0.5) mapped from [-1,1] to [0,1].
static void computeField(float dst[GRID][GRID], float time) {
  const float z   = time * 0.5f;
  const float inv = NOISE_SPAN / (float)GRID;
  for (int y = 0; y < GRID; y++)
    for (int x = 0; x < GRID; x++)
      dst[y][x] = (simplexNoise3D((float)x * inv, (float)y * inv, z)
                   + 1.0f) * 0.5f;
}

// ---------------------------------------------------------------------------
// Marching squares
// ---------------------------------------------------------------------------
// Grid index -> pixel: the full panel edge to edge. One axis serves both
// because the panel is square.
static const float DATA_X0 = 0.0f;
static const float DATA_ST = (float)(W - 1) / (float)(GRID - 1);

// The current pass's colour in the two forms the filler needs: plain 565 for
// rim blending, and byte-swapped 565 for direct framebuffer writes (a 16bpp
// LGFX_Sprite stores swap565_t with stride == width — see termfont.h).
static uint16_t stroke565 = 0xFFFF;
static uint16_t strokeRaw = 0xFFFF;

// The sprite's buffer, resolved once in setup() — panelBegin() allocates it
// exactly once, so the pointer is stable.
static uint16_t *fbPix = nullptr;

static inline void setStroke(uint32_t rgb) {
  stroke565 = to565(rgb);
  strokeRaw = (uint16_t)((stroke565 >> 8) | (stroke565 << 8));
}

// dst and fg are plain 565; a is coverage in 0..32. The R|B pair rides one
// multiply: B*32 tops out below bit 10 and R*32 starts at bit 16, so the two
// fields can't collide before the shift.
SKETCH_INLINE uint16_t blend565q(uint16_t dst, uint16_t fg, uint32_t a) {
  const uint32_t ia  = 32u - a;
  const uint32_t rb  = (((fg & 0xF81Fu) * a + (dst & 0xF81Fu) * ia) >> 5) & 0xF81Fu;
  const uint32_t g   = (((fg & 0x07E0u) * a + (dst & 0x07E0u) * ia) >> 5) & 0x07E0u;
  return (uint16_t)(rb | g);
}

// One rim pixel: d²-derived coverage (no sqrt — the falloff difference over
// one pixel is invisible), integer 565 blend, and pixels already holding
// this pass's colour skipped outright — adjacent segments overlap at every
// shared crossing point.
SKETCH_INLINE void rimPx(uint16_t *row, int px, uint32_t a) {
  if (a == 0) return;
  const uint16_t cur = row[px];
  if (cur == strokeRaw) return;
  if (a >= 32) { row[px] = strokeRaw; return; }
  const uint16_t dst = (uint16_t)((cur >> 8) | (cur << 8));
  const uint16_t out = blend565q(dst, stroke565, a);
  row[px] = (uint16_t)((out >> 8) | (out << 8));
}

// floorf/ceilf are libm calls on this toolchain — hundreds of cycles — and
// the profile put them at most of the frame (they run per cell and per row).
// Truncating casts are one FPU instruction each.
SKETCH_INLINE int fastFloor(float x) { const int i = (int)x; return i - (x < (float)i); }
SKETCH_INLINE int fastCeil (float x) { const int i = (int)x; return i + (x > (float)i); }

// The stroke runs in 1/16-px fixed point: the in-order LX7 stalls 4-5 cycles
// on every dependent float op, and the inner loops below are pure dependency
// chains — as integers they run at a cycle or two per pixel. Positions carry
// x16 (px * 16), so squared quantities carry x256. Worst cases stay inside
// int32: a segment is at most a cell diagonal (~5.4px), so the body's cross
// product is < 37k x256 and its square < 1.4e9.
static const float STROKE_ROUT   = LINE_R + 0.5f;
static const float STROKE_RIN    = LINE_R - 0.5f;
static const int32_t ROUT2_256   = (int32_t)(STROKE_ROUT * STROKE_ROUT * 256.0f);
static const int32_t RIN2_256    = (int32_t)(STROKE_RIN  * STROKE_RIN  * 256.0f);
// Cap rim alpha: a = (ROUT2_256 - d2) * AMUL_CAP >> 16, lands in 0..32.
static const int32_t AMUL_CAP    = (32 << 16) / (ROUT2_256 - RIN2_256);

// One row across a round cap: d² to the centre is a quadratic in x, and the
// step is 16 subunits, so it forward-differences to two adds per pixel.
SKETCH_INLINE void capRow(uint16_t *row, int lo, int hi, int32_t ex0, int32_t ey2) {
  int32_t d2 = ex0 * ex0 + ey2;
  int32_t dd = 32 * ex0 + 256;
  for (int px = lo; px <= hi; px++) {
    if (d2 < ROUT2_256) {
      if (d2 <= RIN2_256) row[px] = strokeRaw;
      else rimPx(row, px, (uint32_t)(((ROUT2_256 - d2) * AMUL_CAP) >> 16));
    }
    d2 += dd; dd += 512;
  }
}

// One row across the body: the cross product with the segment is linear in
// x (step 16·dy), so the squared perpendicular distance is one mul per
// pixel, compared against thresholds pre-scaled by len² — no division.
SKETCH_INLINE void bodyRow(uint16_t *row, int lo, int hi, int32_t cross,
                           int32_t dy16, int32_t rOut2L, int32_t rIn2L,
                           int32_t aDiv) {
  // A row outside the stroke's slab costs nothing: cross is linear, so if
  // both ends sit on one side beyond the outer radius, nothing hits.
  const int32_t crossEnd = cross + (int32_t)(hi - lo) * 16 * dy16;
  if (cross * cross >= rOut2L && crossEnd * crossEnd >= rOut2L &&
      ((cross > 0) == (crossEnd > 0)))
    return;
  const int32_t step = 16 * dy16;
  for (int px = lo; px <= hi; px++, cross += step) {
    const int32_t c2 = cross * cross;
    if (c2 >= rOut2L) continue;
    if (c2 <= rIn2L) { row[px] = strokeRaw; continue; }
    uint32_t a = (uint32_t)((rOut2L - c2) / aDiv);
    rimPx(row, px, a > 32 ? 32 : a);
  }
}

// A filled capsule (the round-capped, round-joined stroke segment), written
// straight into the sprite. LovyanGFX's drawWideLine measured 369ms a frame
// here — thousands of 2-4px segments, each paying a per-pixel float
// projection chain. Instead each row is split at the t=0 / t=1 lines into
// cap and body zones and each zone walks on integer forward differences. A
// pixel on a zone seam may be classified either way; the two d² formulas
// agree on the seam itself, so the misclassification only perturbs a rim
// alpha invisibly.
static void SKETCH_HOT capsule(float axf, float ayf, float bxf, float byf) {
  int x0 = fastFloor((axf < bxf ? axf : bxf) - STROKE_ROUT);
  int x1 = fastCeil ((axf > bxf ? axf : bxf) + STROKE_ROUT);
  int y0 = fastFloor((ayf < byf ? ayf : byf) - STROKE_ROUT);
  int y1 = fastCeil ((ayf > byf ? ayf : byf) + STROKE_ROUT);
  if (x0 < 0) x0 = 0;  if (x1 > W - 1) x1 = W - 1;
  if (y0 < 0) y0 = 0;  if (y1 > H - 1) y1 = H - 1;

  // Endpoints in x16 units (coordinates are positive, so the cast truncates
  // correctly); everything below is integer.
  const int32_t ax = (int32_t)(axf * 16.0f + 0.5f);
  const int32_t ay = (int32_t)(ayf * 16.0f + 0.5f);
  const int32_t bx = (int32_t)(bxf * 16.0f + 0.5f);
  const int32_t by = (int32_t)(byf * 16.0f + 0.5f);

  const int32_t dx = bx - ax, dy = by - ay;
  const int32_t len2 = dx * dx + dy * dy;              // x256
  // Body thresholds carry x65536 (px^4), matching cross²: ROUT2_256 is x256
  // and len2 is x256, so their plain product is already in cross²'s units.
  const int32_t rOut2L = (int32_t)((int64_t)ROUT2_256 * len2);
  const int32_t rIn2L  = (int32_t)((int64_t)RIN2_256  * len2);
  const int32_t aDiv   = (rOut2L - rIn2L) / 32 + 1;    // rim alpha divisor

  // The per-row zone boundaries are solved in float — two muls a row, not
  // worth the integer-division sign gymnastics — while the per-pixel loops
  // above run entirely on ints.
  const float dxF = bxf - axf, dyF = byf - ayf;
  const float len2F  = dxF * dxF + dyF * dyF;
  const float invDxF = dx != 0 ? 1.0f / dxF : 0.0f;

  for (int py = y0; py <= y1; py++) {
    uint16_t *row = fbPix + (size_t)py * (size_t)W;
    const int32_t fy   = py * 16 + 8 - ay;             // row centre, rel. A
    const int32_t fyB  = fy - dy;
    const int32_t ey2A = fy * fy, ey2B = fyB * fyB;
    const int32_t fx0  = x0 * 16 + 8 - ax;             // first px, rel. A

    if (len2 == 0) {                                   // degenerate: a dot
      if (ey2A < ROUT2_256) capRow(row, x0, x1, fx0, ey2A);
      continue;
    }

    if (dx == 0) {
      // t is constant along the row: the whole row is one zone.
      const int32_t fydy = fy * dy;
      if      (fydy < 0)     { if (ey2A < ROUT2_256) capRow(row, x0, x1, fx0, ey2A); }
      else if (fydy > len2)  { if (ey2B < ROUT2_256) capRow(row, x0, x1, fx0 + ax - bx, ey2B); }
      else bodyRow(row, x0, x1, fx0 * dy, dy, rOut2L, rIn2L, aDiv);
      continue;
    }

    // fx (px units, relative to A) where the t=0 and t=1 lines cross this
    // row; A's cap is on the low-x side when dx > 0, B's when dx < 0.
    const float fyF  = (float)py + 0.5f - ayf;
    const float fxT0 = -(fyF * dyF) * invDxF;
    const float fxT1 = (len2F - fyF * dyF) * invDxF;
    const float loFx = dx > 0 ? fxT0 : fxT1;
    const float hiFx = dx > 0 ? fxT1 : fxT0;

    int bodyLo = fastCeil (loFx + axf - 0.5f);
    int bodyHi = fastFloor(hiFx + axf - 0.5f);
    if (bodyLo < x0) bodyLo = x0;  if (bodyHi > x1) bodyHi = x1;

    const int32_t loEy2 = dx > 0 ? ey2A : ey2B;
    const int32_t hiEy2 = dx > 0 ? ey2B : ey2A;
    const int32_t loCx  = dx > 0 ? 0 : dx;             // cap centre, rel. A
    const int32_t hiCx  = dx > 0 ? dx : 0;

    if (bodyLo > x0 && loEy2 < ROUT2_256)
      capRow(row, x0, (bodyLo - 1 < x1 ? bodyLo - 1 : x1), fx0 - loCx, loEy2);
    if (bodyHi >= bodyLo)
      bodyRow(row, bodyLo, bodyHi,
              (fx0 + (bodyLo - x0) * 16) * dy - fy * dx, dy,
              rOut2L, rIn2L, aDiv);
    if (bodyHi < x1 && hiEy2 < ROUT2_256) {
      const int capLo = bodyHi + 1 > x0 ? bodyHi + 1 : x0;
      capRow(row, capLo, x1, fx0 + (capLo - x0) * 16 - hiCx, hiEy2);
    }
  }
}

static inline void seg(float ax, float ay, float bx, float by) {
  capsule(DATA_X0 + ax * DATA_ST, DATA_X0 + ay * DATA_ST,
          DATA_X0 + bx * DATA_ST, DATA_X0 + by * DATA_ST);
}

// One cell at one level. Corners: f00 top-left, f10 top-right, f01 bottom-left,
// f11 bottom-right (field[y][x] indexing, y down — same as the JS data array).
// Crossing edges always have their corners on opposite sides of L, so the
// interpolation denominator is never zero.
static void SKETCH_HOT marchCell(int x, int y, float f00, float f10, float f01, float f11,
                      float L) {
  const int c = ((f00 > L) << 3) | ((f10 > L) << 2) |
                ((f11 > L) << 1) |  (f01 > L);
  if (c == 0 || c == 15) return;

  const float fx = (float)x, fy = (float)y;
  // Crossing points on the four edges, in grid coordinates.
  const float tx  = fx + (L - f00) / (f10 - f00);   // top:    f00 -> f10
  const float bx_ = fx + (L - f01) / (f11 - f01);   // bottom: f01 -> f11
  const float ly  = fy + (L - f00) / (f01 - f00);   // left:   f00 -> f01
  const float ry  = fy + (L - f10) / (f11 - f10);   // right:  f10 -> f11

  switch (c) {
    case 1:  case 14: seg(fx, ly, bx_, fy + 1.0f);        break;  // left-bottom
    case 2:  case 13: seg(bx_, fy + 1.0f, fx + 1.0f, ry); break;  // bottom-right
    case 3:  case 12: seg(fx, ly, fx + 1.0f, ry);         break;  // left-right
    case 4:  case 11: seg(tx, fy, fx + 1.0f, ry);         break;  // top-right
    case 6:  case 9:  seg(tx, fy, bx_, fy + 1.0f);        break;  // top-bottom
    case 7:  case 8:  seg(tx, fy, fx, ly);                break;  // top-left
    case 5:                       // TR + BL inside — saddle, centre decides
      if ((f00 + f10 + f01 + f11) * 0.25f > L) {
        seg(tx, fy, fx, ly);  seg(bx_, fy + 1.0f, fx + 1.0f, ry);
      } else {
        seg(tx, fy, fx + 1.0f, ry);  seg(fx, ly, bx_, fy + 1.0f);
      }
      break;
    case 10:                      // TL + BR inside — the mirror saddle
      if ((f00 + f10 + f01 + f11) * 0.25f > L) {
        seg(tx, fy, fx + 1.0f, ry);  seg(fx, ly, bx_, fy + 1.0f);
      } else {
        seg(tx, fy, fx, ly);  seg(bx_, fy + 1.0f, fx + 1.0f, ry);
      }
      break;
  }
}

// Cell-outer, level-inner: a smooth field means most cells straddle at most
// one of the 11 levels, so bounding the level loop by the cell's own min/max
// does the work of the trivial-reject once instead of eleven times. `m`
// mixes fieldA toward fieldB — 0 is the red pass's field, 1 the white's.
// The row range makes the coWork split below possible.
static void SKETCH_HOT drawIsolinesRange(int yLo, int yHi, float m) {
  for (int y = yLo; y < yHi; y++) {
    for (int x = 0; x < GRID - 1; x++) {
      const float f00 = fieldA[y][x]     + (fieldB[y][x]     - fieldA[y][x])     * m;
      const float f10 = fieldA[y][x + 1] + (fieldB[y][x + 1] - fieldA[y][x + 1]) * m;
      const float f01 = fieldA[y + 1][x] + (fieldB[y + 1][x] - fieldA[y + 1][x]) * m;
      const float f11 = fieldA[y + 1][x + 1] +
                        (fieldB[y + 1][x + 1] - fieldA[y + 1][x + 1]) * m;

      float lo = f00 < f10 ? f00 : f10;  if (f01 < lo) lo = f01;  if (f11 < lo) lo = f11;
      float hi = f00 > f10 ? f00 : f10;  if (f01 > hi) hi = f01;  if (f11 > hi) hi = f11;

      // Generous by one level either side; marchCell rejects the trivial cases.
      int i0 = fastFloor(lo * (float)LEVELS);  if (i0 < 1)          i0 = 1;
      int i1 = fastCeil (hi * (float)LEVELS);  if (i1 > LEVELS - 1) i1 = LEVELS - 1;

      for (int i = i0; i <= i1; i++)
        marchCell(x, y, f00, f10, f01, f11, (float)i / (float)LEVELS);
    }
  }
}

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------
// The specimen label, centred per spec: black plate, white border, white
// 6x8 text. The angle is the JS's |noise1D(playhead)| — noise wearing a
// degree sign — with the degree mark drawn as a 3px ring rather than trusted
// to the font's code page.
static void drawLabel(float ph) {
  const float ang = fabsf(simplexNoise3D(ph, 0.0f, 0.0f));

  char text[32];
  snprintf(text, sizeof(text), "C O N T O U R   %.2f", ang);

  cv.setTextSize(1);
  const int tw   = cv.textWidth(text);
  const int degW = 5;                  // the hand-drawn degree ring + gap
  const int padX = 9, padY = 7;
  const int bw = tw + degW + 2 * padX;
  const int bh = 8 + 2 * padY;
  const int bx = (W - bw) / 2;
  const int by = (H - bh) / 2;

  cv.fillRect(bx, by, bw, bh, BLACK);
  cv.drawRect(bx, by, bw, bh, FG);
  cv.drawRect(bx + 1, by + 1, bw - 2, bh - 2, FG);

  cv.setTextColor(FG);
  cv.setCursor(bx + padX, by + padY);
  cv.print(text);
  cv.drawCircle(bx + padX + tw + 3, by + padY + 1, 1, FG);
}

// ---------------------------------------------------------------------------
static uint32_t fieldUs = 0, marchUs = 0;

// The coWork halves: field A and field B are computed one per core, and each
// stroke pass splits its cell rows down the middle. The halves' strokes
// overlap only in the few rows around the seam, in the same colour, so a
// racing write there is harmless. Parameters travel through statics because
// coWork takes a bare function; the notify handshake orders the writes.
static float coTimeA, coTimeB, coMix;

static void fieldHalf(int half) {
  if (half == 0) computeField(fieldA, coTimeA);
  else           computeField(fieldB, coTimeB);
}

static void marchHalf(int half) {
  const int mid = (GRID - 1) / 2;
  if (half == 0) drawIsolinesRange(0, mid, coMix);
  else           drawIsolinesRange(mid, GRID - 1, coMix);
}

static void renderAll(float ph) {
  cv.fillScreen(BG);

  // Two real fields bracket the passes; the middle two interpolate. Near
  // playhead 0.5 the offsets converge (cos(pi/2) = 0) and the denominator
  // vanishes — but so do the fields' differences, so mix 0 is exact there.
  const float phase  = ph * (float)M_PI;
  const float tRed   = sinf(phase - PASSES[0].timeOff);
  const float tWhite = sinf(phase);                     // PASSES[3] offset is 0

  uint32_t t0 = wallMicros();
  coTimeA = tRed;
  coTimeB = tWhite;
  coWork(fieldHalf);
  fieldUs = wallMicros() - t0;

  const float denom = tWhite - tRed;
  t0 = wallMicros();
  for (int p = 0; p < 4; p++) {
    const float tp = sinf(phase - PASSES[p].timeOff);
    coMix = fabsf(denom) < 1e-6f ? 0.0f : (tp - tRed) / denom;
    setStroke(PASSES[p].color);
    coWork(marchHalf);
  }
  marchUs = wallMicros() - t0;

  drawLabel(ph);
}

// The only randomness in this piece is the noise permutation — the JS's
// Random.permuteNoise(). noiseSeed() burns its 255 draws off the fresh
// stream, exactly as Random.setSeed does.
static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);
  noiseSeed();

  // Deliberately no playhead reset — the knob didn't move. See zhi_knob.cpp.
  drawnPlayhead = -1.0f;
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
  fbPix = (uint16_t *)cv.getBuffer();
  touchBegin();
  encoderBegin();

  generate(newSeed());
  playhead = knobPlayhead();

  renderAll(playhead);
  const uint32_t t0 = wallMicros();
  present();
  const uint32_t pushUs = wallMicros() - t0;
  drawnPlayhead = playhead;
  Serial.printf("first frame: field %luus  march %luus  push %luus\n",
                (unsigned long)fieldUs, (unsigned long)marchUs,
                (unsigned long)pushUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // encoderRev() is pinned at 0 with no knob; sweep the playhead instead so
  // one capture run shows the whole loop. setup() wrote frame 0, so the loop
  // covers 1/(N-1) .. 1.
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
      Serial.printf("playhead %.3f  field %luus  march %luus\n",
                    playhead, (unsigned long)fieldUs, (unsigned long)marchUs);
    }
  } else {
    delay(8);
  }
#endif
}
