// ============================================================================
// napoleon — Napoleon's theorem as a fidget: drag the vertices, the proof holds
// ============================================================================
//   pio run -e nap_a18_native -t exec     SDL window (mouse-while-held = finger)
//   pio run -e nap_a18        -t upload   the board
//   pio run -e nap_a18_shot               headless PNG capture, then run it
//
// Port of napoleon-theorem.js (canvas-sketch, 1080x1080, 24s loop). Three
// points make a triangle; on each side an equilateral triangle is erected
// outward (apex placed via the circumcenter), and the centroids of those
// three always form an equilateral triangle — Napoleon's theorem. The
// original wandered the points with loop noise; here the noise is replaced
// by the finger: each vertex wears a purple handle you drag, and the whole
// construction follows live. A quick tap on empty glass puts the points
// back at the original's starting layout. Deterministic — no randomness
// anywhere, so there is nothing for a reseed to vary (the circle_moire
// situation). Touch-first, so a18 only.
//
// Two behaviours preserved deliberately:
//   - The outer triangles stroke in the centroid triangle's green. The JS
//     assigns each triangle's own point array to strokeStyle (invalid, so
//     canvas keeps the previous style), which from frame 2 on is the last
//     valid stroke — #01FF70. That steady state is what the piece has
//     always looked like, so it is what the port draws.
//   - Collinear points degenerate the construction (the circumcenter races
//     to infinity; the JS flicks NaN triangles off canvas). Here the last
//     valid construction holds while the main triangle keeps tracking the
//     finger, and the derived geometry snaps back once the points leave
//     the degenerate line — strictly better than drawing NaN.
//
// One divergence: the 1080-canvas handles are 27px, which scales to 9px —
// well under a millimetre on this 330ppi glass. They are the drag
// affordance, so they render at HANDLE_R instead; the grab target GRAB_R
// is finger-sized and independent of the visual radius.
// ============================================================================

#define SKETCH_TITLE  "napoleon"
#define SKETCH_FRAMES 12

#include "shared/platform.h"
#include "shared/color.h"
#include "shared/touch.h"

// ---------------------------------------------------------------------------
// Config — the original's 1080-space mapped uniformly, centered vertically
// ---------------------------------------------------------------------------
static const float SCALE = (float)W / 1080.0f;
static const float OFF_X = ((float)W - 1080.0f * SCALE) * 0.5f;   // 0
static const float OFF_Y = ((float)H - 1080.0f * SCALE) * 0.5f;   // the tall panel's spare rows

static const uint32_t COL_BG     = 0xFFFFFF;
static const uint32_t COL_INK    = 0x333333;   // the main triangle
static const uint32_t COL_NAP    = 0x01FF70;   // outer + centroid triangles (see header)
static const uint32_t COL_HANDLE = 0xA463F2;   // rgba(164, 99, 242, 0.55)
static const uint8_t  HANDLE_A8  = 140;        // 0.55 * 255

static const float LINE_R   = 3.0f * SCALE;    // JS lineWidth 6 → wedge radius
// Width-proportional rather than fixed pixels: the touch boards' glasses are
// all about 30mm wide, so a fraction of W is a fraction of a fingertip on
// every one of them. Tuned by hand on the a18 (24px visual, 100px grab).
static const float HANDLE_R = 70.0f * SCALE;   // visual; the JS's 27 lands invisible (header)
static const float GRAB_R   = 295.0f * SCALE;  // ~8mm — the real touch target

// The original's starting points, 1080-space.
static const float INIT_PTS[3][2] = { { 270.0f, 540.0f }, { 405.0f, 270.0f }, { 810.0f, 540.0f } };

// A press that resolves within TAP_MAX_MS having grabbed nothing and moved
// less than TAP_MAX_MOVE is the reset gesture (paint.cpp's classifier).
static const uint32_t TAP_MAX_MS   = 250;
static const float    TAP_MAX_MOVE = 8.0f;

// ---------------------------------------------------------------------------
// Geometry — the JS functions, floats throughout
// ---------------------------------------------------------------------------
struct Tri { float x[3], y[3]; };

static float pts[3][2];                        // u, v, w in panel coordinates
static Tri   triA, triB, triC, triCen;         // last valid derived construction
static bool  derValid = false;

static inline float distf(float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0, dy = y1 - y0;
  return sqrtf(dx * dx + dy * dy);
}

// sign(p1, p2, p3) — which side of line p2→p3 the point p1 falls on.
static inline float orient(float p1x, float p1y, float p2x, float p2y, float p3x, float p3y) {
  return (p1x - p3x) * (p2y - p3y) - (p2x - p3x) * (p1y - p3y);
}

// The JS rounds the result to 0.01px; that was invisible and is skipped.
static bool circumCenter(float ax, float ay, float bx, float by, float cx, float cy,
                         float *ox, float *oy) {
  const float d = (ax - cx) * (by - cy) - (bx - cx) * (ay - cy);
  if (fabsf(d) < 1e-3f) return false;          // collinear: keep the last construction
  const float ka = ((ax - cx) * (ax + cx) + (ay - cy) * (ay + cy)) * 0.5f;
  const float kb = ((bx - cx) * (bx + cx) + (by - cy) * (by + cy)) * 0.5f;
  *ox = (ka * (by - cy) - kb * (ay - cy)) / d;
  *oy = (kb * (ax - cx) - ka * (bx - cx)) / d;
  return isfinite(*ox) && isfinite(*oy);
}

// apex(u, v, cc): the equilateral apex over base uv, pushed away from the
// circumcenter — m → 0 when uv is a right triangle's hypotenuse (cc lands on
// the midpoint), which the JS answered with NaN; here it keeps the last frame.
static bool apexPoint(float ux, float uy, float vx, float vy, float ccx, float ccy,
                      float *ox, float *oy) {
  const float mpx = (vx + ux) * 0.5f, mpy = (vy + uy) * 0.5f;
  const float dir = orient(ccx, ccy, ux, uy, vx, vy) > 0.0f ? 1.0f : -1.0f;
  const float cmx = (mpx - ccx) * dir, cmy = (mpy - ccy) * dir;
  const float h = sqrtf(3.0f) * 0.5f * distf(ux, uy, vx, vy);
  const float m = sqrtf(cmx * cmx + cmy * cmy);
  if (m < 1e-4f) return false;
  *ox = cmx * (h + dir * m) / m + ccx;
  *oy = cmy * (h + dir * m) / m + ccy;
  return isfinite(*ox) && isfinite(*oy);
}

// eqTriangle(u, v, cc) = [u, apex, v]. (The JS passes a fourth argument the
// function never declares — dead, dropped here.)
static bool eqTriangle(const float *u, const float *v, float ccx, float ccy, Tri *out) {
  out->x[0] = u[0]; out->y[0] = u[1];
  out->x[2] = v[0]; out->y[2] = v[1];
  return apexPoint(u[0], u[1], v[0], v[1], ccx, ccy, &out->x[1], &out->y[1]);
}

static Tri centroidOf(const Tri &t) {
  Tri c;
  c.x[0] = (t.x[0] + t.x[1] + t.x[2]) / 3.0f;
  c.y[0] = (t.y[0] + t.y[1] + t.y[2]) / 3.0f;
  return c;
}

// getState(): recompute the construction from the current points, committing
// only if every derived value is finite — otherwise the last valid geometry
// stands and the main triangle alone follows the finger.
static void computeDerived() {
  float ccx, ccy;
  if (!circumCenter(pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1],
                    &ccx, &ccy)) return;

  Tri a, b, c;
  if (!eqTriangle(pts[0], pts[1], ccx, ccy, &a)) return;
  if (!eqTriangle(pts[1], pts[2], ccx, ccy, &b)) return;
  if (!eqTriangle(pts[2], pts[0], ccx, ccy, &c)) return;

  triA = a; triB = b; triC = c;
  const Tri ca = centroidOf(a), cb = centroidOf(b), cc = centroidOf(c);
  triCen.x[0] = ca.x[0]; triCen.y[0] = ca.y[0];
  triCen.x[1] = cb.x[0]; triCen.y[1] = cb.y[0];
  triCen.x[2] = cc.x[0]; triCen.y[2] = cc.y[0];
  derValid = true;
}

static void resetPoints() {
  for (int i = 0; i < 3; i++) {
    pts[i][0] = INIT_PTS[i][0] * SCALE + OFF_X;
    pts[i][1] = INIT_PTS[i][1] * SCALE + OFF_Y;
  }
  computeDerived();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void strokeTri(const Tri &t, uint32_t color) {
  for (int i = 0; i < 3; i++) {
    const int j = (i + 1) % 3;
    cv.drawWideLine((int32_t)lroundf(t.x[i]), (int32_t)lroundf(t.y[i]),
                    (int32_t)lroundf(t.x[j]), (int32_t)lroundf(t.y[j]),
                    LINE_R, color);
  }
}

// The 55%-alpha handle: analytic disc coverage, blended into whatever is
// already under it (the strokes pass through the vertices) by fillRectAlpha.
// Full-coverage interior rows batch into one span call per row.
static void drawHandle(float cx, float cy) {
  int x0 = (int)floorf(cx - HANDLE_R) - 1, x1 = (int)ceilf(cx + HANDLE_R) + 1;
  int y0 = (int)floorf(cy - HANDLE_R) - 1, y1 = (int)ceilf(cy + HANDLE_R) + 1;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;

  for (int yy = y0; yy <= y1; yy++) {
    const float dy = ((float)yy + 0.5f) - cy;
    int span = -1;
    for (int xx = x0; xx <= x1; xx++) {
      const float dx    = ((float)xx + 0.5f) - cx;
      const float cover = 0.5f + (HANDLE_R - sqrtf(dx * dx + dy * dy));
      if (cover >= 1.0f) {
        if (span < 0) span = xx;
        continue;
      }
      if (span >= 0) { cv.fillRectAlpha(span, yy, xx - span, 1, HANDLE_A8, COL_HANDLE); span = -1; }
      if (cover > 0.0f)
        cv.fillRectAlpha(xx, yy, 1, 1, (uint8_t)(cover * (float)HANDLE_A8), COL_HANDLE);
    }
    if (span >= 0) cv.fillRectAlpha(span, yy, x1 - span + 1, 1, HANDLE_A8, COL_HANDLE);
  }
}

// The JS draw order: outer triangles, main, centroid triangle, handles last.
static void render() {
  cv.fillScreen(to565(COL_BG));

  if (derValid) {
    strokeTri(triA, COL_NAP);
    strokeTri(triB, COL_NAP);
    strokeTri(triC, COL_NAP);
  }

  Tri main;
  for (int i = 0; i < 3; i++) { main.x[i] = pts[i][0]; main.y[i] = pts[i][1]; }
  strokeTri(main, COL_INK);

  if (derValid) strokeTri(triCen, COL_NAP);

  for (int i = 0; i < 3; i++) drawHandle(pts[i][0], pts[i][1]);
}

// ---------------------------------------------------------------------------
// Gesture state — file-scope so a switcher re-entry through setup() would
// reset it rather than carry a stale grab across sketches.
// ---------------------------------------------------------------------------
static bool     wasDown  = false;
static int      grabbed  = -1;      // which point the finger holds, -1 = none
static float    grabDx, grabDy;     // handle center minus press point, kept while dragging
static float    pressX, pressY;
static uint32_t pressAt  = 0;
static bool     moved    = false;

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }
  touchBegin();

  wasDown = false;
  grabbed = -1;
  resetPoints();

  Serial.printf("\nnapoleon: drag a handle to move a vertex, tap empty glass to reset\n");
  Serial.printf("points (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)  scale %.3f\n",
                pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1], SCALE);

  const uint32_t t0 = wallMicros();
  render();
  const uint32_t drawUs = wallMicros() - t0;
  present();
  Serial.printf("first frame drawn in %luus\n", (unsigned long)drawUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // No finger: orbit two vertices around their starts so the capture shows
  // the construction tracking movement. Pure function of the frame index,
  // so runs reproduce.
  static int frame = 1;
  const float t = (float)frame / (float)(SKETCH_FRAMES > 1 ? SKETCH_FRAMES : 1);
  const float ang = t * 2.0f * (float)M_PI;
  pts[2][0] = (INIT_PTS[2][0] + 120.0f * cosf(ang)) * SCALE + OFF_X;
  pts[2][1] = (INIT_PTS[2][1] + 120.0f * sinf(ang)) * SCALE + OFF_Y;
  pts[1][0] = (INIT_PTS[1][0] + 90.0f * cosf(-ang)) * SCALE + OFF_X;
  pts[1][1] = (INIT_PTS[1][1] + 90.0f * sinf(-ang)) * SCALE + OFF_Y;
  frame++;
  computeDerived();
  render();
  present();
  delay(16);
#else
  int tx, ty;
  const bool down = touchPoint(&tx, &ty);
  bool changed = false;

  if (down) {
    const float fx = (float)tx, fy = (float)ty;

    if (!wasDown) {
      pressX = fx; pressY = fy;
      pressAt = millis();
      moved   = false;
      grabbed = -1;
      float best = GRAB_R;
      for (int i = 0; i < 3; i++) {
        const float d = distf(fx, fy, pts[i][0], pts[i][1]);
        if (d <= best) { best = d; grabbed = i; }
      }
      if (grabbed >= 0) { grabDx = pts[grabbed][0] - fx; grabDy = pts[grabbed][1] - fy; }
    }

    if (distf(fx, fy, pressX, pressY) >= TAP_MAX_MOVE) moved = true;

    if (grabbed >= 0) {
      float nx = fx + grabDx, ny = fy + grabDy;
      if (nx < HANDLE_R) nx = HANDLE_R;
      if (ny < HANDLE_R) ny = HANDLE_R;
      if (nx > (float)W - HANDLE_R) nx = (float)W - HANDLE_R;
      if (ny > (float)H - HANDLE_R) ny = (float)H - HANDLE_R;
      if (nx != pts[grabbed][0] || ny != pts[grabbed][1]) {
        pts[grabbed][0] = nx;
        pts[grabbed][1] = ny;
        computeDerived();
        changed = true;
      }
    }
  } else if (wasDown) {
    if (grabbed < 0 && !moved && millis() - pressAt < TAP_MAX_MS) {
      resetPoints();
      changed = true;
    }
    grabbed = -1;
  }
  wasDown = down;

  if (changed) {
    const uint32_t t0 = wallMicros();
    render();
    const uint32_t drawUs = wallMicros() - t0;
    present();

    static uint32_t reportAt = 0;
    if (millis() - reportAt >= 1000) {
      reportAt = millis();
      Serial.printf("pts (%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)  draw %luus\n",
                    pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1],
                    (unsigned long)drawUs);
    }
  } else {
    delay(5);
  }
#endif
}
