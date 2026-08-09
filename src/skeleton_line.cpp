// ============================================================================
// skeleton line — disks under a rubber band, joined by tendons
// ============================================================================
//   pio run -e skeleton_native -t exec      SDL window
//   pio run -e skeleton_esp32  -t upload    the board
//   pio run -e skeleton_shot                headless PNG capture, then run
//                                           .pio/build/skeleton_shot/program DIR SEED N
//
// Ported from sketchbook-ssam/src/sketches/rubber-band-stipple/skeleton-line.ts.
//
// What the canvas code actually draws
// -----------------------------------
// `rubberBandPath()` looks like arbitrary path work but it is the **convex hull
// of a set of disks** — outer tangent segments joined by arcs, i.e. shrink-wrap.
// It is called in exactly two shapes:
//
//   * around the whole hull set (stroked only, never filled)
//   * around a *pair* of disks — a tendon
//
// The pair case has a closed-form signed distance (iq's round cone), so a
// tendon needs no path machinery at all: fill is `sd < 0`, stroke is `|sd| < w`,
// both antialiased, one pass. The N-disk case is stroked only, so it reduces to
// stroking its arcs and tangent segments directly. Clipping each arc by angle
// and each segment by its perpendicular slab makes them tile exactly — the cuts
// land on the same line, because the segment is tangent where the arc ends — so
// nothing is drawn twice at a join.
//
// `hullIndices()` is a support-function sweep, and `ensurePositions()` calls it
// on disks that all carry `config.maxR`. Adding the same radius to every
// support value cannot change the argmax, so **that call is just the convex
// hull of the seed points** and `maxR` cancels out of it entirely.
//
// Two colours, one buffer
// -----------------------
// Every drawing op in the sketch is either "fill this region with bg" or
// "stroke this outline in ink at 50% alpha". So the frame is one 8-bit
// coverage plane: fills multiply it down toward 0, strokes composite up toward
// 1, and a 256-entry LUT resolves bg-to-ink at the end. That is what makes the
// overlapping-alpha semantics come out right, and it gets analytic AA for free.
//
// Keep in sync with the Arduino IDE copy:
//   cp src/skeleton_line.cpp ~/Documents/Arduino/skeleton_line/sketch.h
//   cp -R src/shared          ~/Documents/Arduino/skeleton_line/
// ============================================================================

#define SKETCH_TITLE  "skeleton line"
#define SKETCH_FRAMES 24

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/noise.h"
#include "shared/subtractive.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const int LOOP_MS      = 8000;    // ssam duration
static const int FRAME_MS     = 1000 / 24;

static const int COUNT        = 8;      // config.count
static const int CONN         = 2;       // config.connectionsPerInside
static const int LAYERS       = 4;       // config.layers
static const int RELAX_ITERS  = 24;      // config.relaxIterations
static const int HULL_SAMPLES = 360;     // config.hullSamples

// One uniform scale for every length in the piece.
//
// The JS canvas is 1080x1080 and this panel is 172x320 — a different aspect, so
// there is no scale that maps the frame onto the frame. What has to carry over
// is how much of the canvas the disks cover, and that is preserved by the
// area-preserving scale: sqrt(172*320 / 1080^2). Both come out at 35% covered,
// with 16 disks either way. Scaling by width instead (0.159) would leave the
// panel two thirds empty; scaling x and y separately would turn every disk into
// an ellipse and take the tangent geometry with it.
static constexpr float S = 0.2171599f;

static constexpr float MIN_R    = 20.0f  * S;   //  4.34   config.minR
static constexpr float MAX_R    = 160.0f * S;   // 34.75   config.maxR
static constexpr float DRIFT    = 27.0f  * S;   //  5.86   width * driftFactor
static constexpr float LAYER_SP = 14.0f  * S;   //  3.04   config.layerSpacing
static constexpr float HULL_SP  = 14.0f  * S;   //  3.04   config.hullLayerSpacing
static constexpr float CENTER_R = 7.0f   * S;   //  1.52   config.centerRingRadius
static constexpr float GAP_EPS  = 2.0f   * S;   //         the `+ 2` in the edge test
static constexpr float MARGIN   = MAX_R + DRIFT + 4.0f * S;

// The one length that is *not* to scale. config.strokeWidth is 2 on 1080, which
// lands at 0.43px here: at 50% alpha that peaks around a = 0.21, a wash that
// shimmers as the curve slides sub-pixel. Pinned to a whole pixel instead.
static constexpr float STROKE_W  = 1.0f;
static constexpr float STROKE_HW = STROKE_W * 0.5f;
static constexpr float STROKE_E  = STROKE_HW + 0.5f;   // half-width + AA fringe
static constexpr float STROKE_A  = 0.5f;               // hsl(from ink h s l / 0.5)

// noise(x, y) in the JS is noise4D(x/100, y/100, ..., frequency 0.25), so the
// field's correlation length is 400 canvas px. Scaled, that is 400*S panel px.
static constexpr float NOISE_K   = 1.0f / (400.0f * S);
static constexpr float NOISE_OFF = 500.0f / 400.0f;    // the JS's +500px offset

// Skipping the inner tendon fills is exact only while a layer's stroke stays
// clear of the next layer's region — see drawTendons().
static_assert(LAYER_SP > STROKE_E, "inner band fills stop being no-ops");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct Vec2   { float x, y; };
struct Circle { float x, y, r; };

static Vec2     positions[COUNT];          // seed layout, fixed per composition
static Circle   circles[COUNT];            // per-frame: drifted and relaxed
static int      nCircles;

static uint8_t  boundaryIdx[COUNT];        // convex hull of the seed points
static int      nBoundary;
static uint8_t  edgeA[COUNT * CONN];       // inside node
static uint8_t  edgeB[COUNT * CONN];       // its boundary node
static int      nEdges;

static int      hullOrder[COUNT + 2];      // per-frame hull, in traversal order
static int      nHull;

static uint32_t bgColor, inkColor;
static uint16_t inkLut[256];

static float    hullCos[HULL_SAMPLES], hullSin[HULL_SAMPLES];

// The frame, as coverage: 0 = pure background, 255 = pure ink.
//
// 55KB, and it lives in PSRAM: as a static it would take internal SRAM down to
// where the 110KB framebuffer no longer reliably finds a contiguous block —
// which is what keeps this sketch in the switcher alongside four others. The
// raster primitives walk it in row spans, so the cache carries it.
static const size_t AB_BYTES = (size_t)W * (size_t)H;
static uint8_t     *ab;

// ---------------------------------------------------------------------------
// Coverage plane
// ---------------------------------------------------------------------------
// `stroke` composites ink over whatever is there, `fill` paints opaque
// background back over it. Both take a pixel coverage in [0, 1].

static inline void inkAt(int idx, float cov) {
  if (cov <= 0.0f) return;
  if (cov > 1.0f) cov = 1.0f;
  const float a = (float)ab[idx] * (1.0f / 255.0f);
  ab[idx] = (uint8_t)((a + (1.0f - a) * STROKE_A * cov) * 255.0f + 0.5f);
}

static inline void bgAt(int idx, float cov) {
  if (cov <= 0.0f) return;
  if (cov >= 1.0f) { ab[idx] = 0; return; }
  ab[idx] = (uint8_t)((float)ab[idx] * (1.0f - cov) + 0.5f);
}

// ---------------------------------------------------------------------------
// Raster primitives
// ---------------------------------------------------------------------------

// x range of a convex quad at one scanline. Also used for stroke slabs.
static bool quadRowSpan(const float *qx, const float *qy, float yc, float &lo, float &hi) {
  bool any = false;
  for (int i = 0; i < 4; i++) {
    const int j = (i + 1) & 3;
    if ((qy[i] <= yc) == (qy[j] <= yc)) continue;
    const float t = (yc - qy[i]) / (qy[j] - qy[i]);
    const float x = qx[i] + t * (qx[j] - qx[i]);
    if (!any)        { lo = hi = x; any = true; }
    else if (x < lo) { lo = x; }
    else if (x > hi) { hi = x; }
  }
  return any;
}

// context.arc(...); context.fill() with an opaque background.
static void fillDisk(float cx, float cy, float r) {
  if (r <= 0.0f) return;
  const float ro = r + 0.5f, ri = r - 0.5f;
  const float ro2 = ro * ro, ri2 = ri > 0.0f ? ri * ri : -1.0f;

  int y0 = (int)floorf(cy - ro), y1 = (int)ceilf(cy + ro);
  if (y0 < 0) y0 = 0;
  if (y1 > H) y1 = H;

  for (int py = y0; py < y1; py++) {
    const float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    const float half = sqrtf(ro2 - dy2);
    int x0 = (int)floorf(cx - half), x1 = (int)ceilf(cx + half);
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;

    const int base = py * W;
    for (int px = x0; px < x1; px++) {
      const float dx = (float)px + 0.5f - cx;
      const float d2 = dx * dx + dy2;
      if (d2 <= ri2) { ab[base + px] = 0; continue; }   // interior, no AA needed
      bgAt(base + px, 0.5f + (r - sqrtf(d2)));
    }
  }
}

// An angular wedge, as two half-plane tests against the arc's end directions.
// `wide` flips it to a union for sweeps past a half turn. Cheaper than atan2
// per pixel, and there are a lot of pixels.
struct Wedge { float ax, ay, bx, by; bool wide; };

static inline bool inWedge(const Wedge &w, float dx, float dy) {
  const float ca = w.ax * dy - w.ay * dx;      // cross(start, p - c)
  const float cd = dx * w.by - dy * w.bx;      // cross(p - c, end)
  return w.wide ? (ca >= 0.0f || cd >= 0.0f) : (ca >= 0.0f && cd >= 0.0f);
}

// Stroke a circle, or the arc of one. Walks only the annulus the stroke can
// reach, two spans per row once the inner edge clears the scanline.
static void strokeCircleBand(float cx, float cy, float r, const Wedge *w) {
  if (r <= 0.0f) return;
  const float ro = r + STROKE_E, ri = r - STROKE_E;
  const float ro2 = ro * ro, ri2 = ri > 0.0f ? ri * ri : -1.0f;

  int y0 = (int)floorf(cy - ro), y1 = (int)ceilf(cy + ro);
  if (y0 < 0) y0 = 0;
  if (y1 > H) y1 = H;

  for (int py = y0; py < y1; py++) {
    const float dy = (float)py + 0.5f - cy, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    const float xo    = sqrtf(ro2 - dy2);
    const bool  split = dy2 < ri2;
    const float xi    = split ? sqrtf(ri2 - dy2) : 0.0f;
    const int   base  = py * W;

    for (int pass = 0; pass < (split ? 2 : 1); pass++) {
      const float xa = split ? (pass ? cx + xi : cx - xo) : cx - xo;
      const float xb = split ? (pass ? cx + xo : cx - xi) : cx + xo;
      int x0 = (int)floorf(xa), x1 = (int)ceilf(xb);
      if (x0 < 0) x0 = 0;
      if (x1 > W) x1 = W;

      for (int px = x0; px < x1; px++) {
        const float dx = (float)px + 0.5f - cx;
        if (w && !inWedge(*w, dx, dy)) continue;
        const float d = sqrtf(dx * dx + dy2);
        inkAt(base + px, STROKE_E - fabsf(d - r));
      }
    }
  }
}

// Stroke a line segment with square ends. The slab clip is deliberate: where a
// segment meets a tangent arc the cut lands on the same line the arc's angular
// clip uses, so the two tile without overlapping.
static void strokeSegment(float ax, float ay, float bx, float by) {
  const float ex = bx - ax, ey = by - ay;
  const float len2 = ex * ex + ey * ey;
  if (len2 < 1e-8f) return;
  const float len = sqrtf(len2);
  const float ux = ex / len, uy = ey / len;
  const float nx = -uy * STROKE_E, ny = ux * STROKE_E;

  const float qx[4] = { ax + nx, bx + nx, bx - nx, ax - nx };
  const float qy[4] = { ay + ny, by + ny, by - ny, ay - ny };

  float lo = qy[0], hi = qy[0];
  for (int i = 1; i < 4; i++) {
    if (qy[i] < lo) lo = qy[i];
    if (qy[i] > hi) hi = qy[i];
  }
  int y0 = (int)floorf(lo), y1 = (int)ceilf(hi);
  if (y0 < 0) y0 = 0;
  if (y1 > H) y1 = H;

  for (int py = y0; py < y1; py++) {
    const float yc = (float)py + 0.5f;
    float xlo = 0.0f, xhi = 0.0f;
    if (!quadRowSpan(qx, qy, yc, xlo, xhi)) continue;
    int x0 = (int)floorf(xlo), x1 = (int)ceilf(xhi);
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;

    const int   base = py * W;
    const float ry   = yc - ay;
    for (int px = x0; px < x1; px++) {
      const float rx = (float)px + 0.5f - ax;
      const float t  = rx * ux + ry * uy;
      if (t < 0.0f || t > len) continue;
      inkAt(base + px, STROKE_E - fabsf(rx * -uy + ry * ux));
    }
  }
}

// Convex hull of two disks, filled and/or stroked in one pass.
//
// The exact signed distance (iq's round cone): project onto the axis, decide
// between the two caps and the tangent flank by which side of the cone's
// normal the point falls, and only the caps need a square root.
static void bandDraw(const Circle &c0, const Circle &c1, bool doFill) {
  const float ex = c1.x - c0.x, ey = c1.y - c0.y;
  const float h  = sqrtf(ex * ex + ey * ey);
  if (h < 1e-4f) return;
  const float hx = ex / h, hy = ey / h;
  const float bb = (c0.r - c1.r) / h;
  if (bb >= 1.0f || bb <= -1.0f) return;          // one disk swallows the other
  const float aa = sqrtf(1.0f - bb * bb);
  const float kMax = aa * h;

  // Row spans from the hull of the two *expanded* disks — the disks plus the
  // quad on their four outer tangent points. Expanding both radii by the same
  // amount leaves (r0 - r1)/h alone, so the tangent direction is unchanged.
  const float r0e = c0.r + STROKE_E, r1e = c1.r + STROKE_E;
  const float px0 =  bb * hx + aa * hy, py0 =  bb * hy - aa * hx;
  const float px1 =  bb * hx - aa * hy, py1 =  bb * hy + aa * hx;
  const float qx[4] = { c0.x + px0 * r0e, c1.x + px0 * r1e, c1.x + px1 * r1e, c0.x + px1 * r0e };
  const float qy[4] = { c0.y + py0 * r0e, c1.y + py0 * r1e, c1.y + py1 * r1e, c0.y + py1 * r0e };

  float top = c0.y - r0e, bot = c0.y + r0e;
  if (c1.y - r1e < top) top = c1.y - r1e;
  if (c1.y + r1e > bot) bot = c1.y + r1e;
  int y0 = (int)floorf(top), y1 = (int)ceilf(bot);
  if (y0 < 0) y0 = 0;
  if (y1 > H) y1 = H;

  for (int py = y0; py < y1; py++) {
    const float yc = (float)py + 0.5f;

    float lo = 0.0f, hi = 0.0f;
    bool  any = quadRowSpan(qx, qy, yc, lo, hi);
    for (int s = 0; s < 2; s++) {
      const float cx = s ? c1.x : c0.x, cy = s ? c1.y : c0.y, re = s ? r1e : r0e;
      const float dy = yc - cy;
      if (fabsf(dy) >= re) continue;
      const float half = sqrtf(re * re - dy * dy);
      if (!any)                 { lo = cx - half; hi = cx + half; any = true; }
      else {
        if (cx - half < lo) lo = cx - half;
        if (cx + half > hi) hi = cx + half;
      }
    }
    if (!any) continue;

    int x0 = (int)floorf(lo), x1 = (int)ceilf(hi);
    if (x0 < 0) x0 = 0;
    if (x1 > W) x1 = W;

    const int   base = py * W;
    const float ry   = yc - c0.y;
    for (int px = x0; px < x1; px++) {
      const float rx = (float)px + 0.5f - c0.x;
      const float u  = rx * hx + ry * hy;
      const float v  = fabsf(rx * -hy + ry * hx);

      float sd;
      const float k = aa * u - bb * v;
      if (k < 0.0f) {
        sd = sqrtf(rx * rx + ry * ry) - c0.r;
      } else if (k > kMax) {
        const float dx = rx - ex, dy = ry - ey;
        sd = sqrtf(dx * dx + dy * dy) - c1.r;
      } else {
        sd = aa * v + bb * u - c0.r;
      }

      if (doFill) bgAt(base + px, 0.5f - sd);
      inkAt(base + px, STROKE_E - fabsf(sd));
    }
  }
}

// ---------------------------------------------------------------------------
// Rubber band
// ---------------------------------------------------------------------------

// getTangentPoints(): the outer tangent line, on the perpRight side of c0->c1.
static inline bool tangentPoints(const Circle &c0, const Circle &c1, Vec2 &t0, Vec2 &t1) {
  const float ex = c1.x - c0.x, ey = c1.y - c0.y;
  const float d  = sqrtf(ex * ex + ey * ey);
  if (d < 1e-4f) return false;                  // the JS divides by d regardless
  const float dx = ex / d, dy = ey / d;
  const float a  = (c0.r - c1.r) / d;
  const float b  = sqrtf(a * a >= 1.0f ? 0.0f : 1.0f - a * a);
  const float nx = a * dx + b * dy;             // perpRight(dir) = (dir.y, -dir.x)
  const float ny = a * dy - b * dx;
  t0 = Vec2{ c0.x + nx * c0.r, c0.y + ny * c0.r };
  t1 = Vec2{ c1.x + nx * c1.r, c1.y + ny * c1.r };
  return true;
}

// rubberBandPath() + stroke(). Arcs joined by their outer tangents.
static void strokeRubberBand(const Circle *c, int n) {
  if (n < 2) return;

  static Vec2 t1[COUNT + 2], t2[COUNT + 2];
  for (int i = 0; i < n; i++)
    if (!tangentPoints(c[i], c[(i + 1) % n], t1[i], t2[i])) return;

  for (int i = 0; i < n; i++) {
    const Vec2 arrival = t2[(i - 1 + n) % n];
    const Vec2 depart  = t1[i];
    const float inv    = 1.0f / c[i].r;

    // Both points sit on the circle, so dividing by r is all the normalising
    // the wedge directions need — no atan2 anywhere.
    Wedge w;
    w.ax = (arrival.x - c[i].x) * inv;  w.ay = (arrival.y - c[i].y) * inv;
    w.bx = (depart.x  - c[i].x) * inv;  w.by = (depart.y  - c[i].y) * inv;
    w.wide = (w.ax * w.by - w.ay * w.bx) < 0.0f;    // sin(sweep) < 0 => past pi

    strokeCircleBand(c[i].x, c[i].y, c[i].r, &w);
    strokeSegment(t1[i].x, t1[i].y, t2[i].x, t2[i].y);
  }
}

// hullIndices(): sweep a direction around the circle and collect the argmax of
// the support function. Strict `>` scanning upward, so ties fall to the lowest
// index — the same way the JS resolves them.
static int hullIndices(const Circle *c, int n, int *out) {
  if (n < 2) {
    for (int i = 0; i < n; i++) out[i] = i;
    return n;
  }
  int m = 0, last = -1;
  for (int s = 0; s < HULL_SAMPLES; s++) {
    const float co = hullCos[s], si = hullSin[s];
    int   best = 0;
    float bestVal = -1e30f;
    for (int i = 0; i < n; i++) {
      const float v = c[i].x * co + c[i].y * si + c[i].r;
      if (v > bestVal) { bestVal = v; best = i; }
    }
    if (best != last) {
      if (m < COUNT + 2) out[m++] = best;
      last = best;
    }
  }
  if (m > 1 && out[0] == out[m - 1]) m--;
  return m;
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

static void buildPalette() {
  uint32_t colors[SUBTRACTIVE_N];
  generateSubtractiveColors(colors);

  // WCAG is the right metric here — this is a legibility question, foreground
  // against background, not a "can I tell these two apart" question.
  int   bi = 0, bj = 1;
  float best = -1.0f;
  for (int i = 0; i < SUBTRACTIVE_N; i++)
    for (int j = i + 1; j < SUBTRACTIVE_N; j++) {
      const float c = wcagContrast(colors[i], colors[j]);
      if (c > best) { best = c; bi = i; bj = j; }
    }

  const bool iLighter = relLuminance(colors[bi]) >= relLuminance(colors[bj]);
  bgColor  = iLighter ? colors[bi] : colors[bj];
  inkColor = iLighter ? colors[bj] : colors[bi];

  for (int i = 0; i < 256; i++)
    inkLut[i] = blend565(bgColor, inkColor, (float)i / 255.0f);

  Serial.printf("bg #%06lX  ink #%06lX  contrast %.2f\n",
                (unsigned long)bgColor, (unsigned long)inkColor, best);
}

// ensurePositions(): rejection-sample the seed points, split them into hull and
// interior, and wire each interior point to its nearest hull points.
static void layout() {
  const float availW = (float)W - 2.0f * MARGIN;
  const float availH = (float)H - 2.0f * MARGIN;

  // The JS writes ((width - 2*margin) / sqrt(count)) * 0.75, which on a square
  // canvas is sqrt(area / count) * 0.75. That is the form that generalises.
  const float minDist = sqrtf(availW * availH / (float)COUNT) * 0.75f;

  nCircles = 0;
  for (int attempt = 0; nCircles < COUNT && attempt < COUNT * 40; attempt++) {
    const Vec2 p = { rngRange(MARGIN, (float)W - MARGIN),
                     rngRange(MARGIN, (float)H - MARGIN) };
    bool ok = true;
    for (int i = 0; i < nCircles; i++) {
      const float dx = positions[i].x - p.x, dy = positions[i].y - p.y;
      if (sqrtf(dx * dx + dy * dy) < minDist) { ok = false; break; }
    }
    if (ok) positions[nCircles++] = p;
  }

  // stableDisks all carry maxR, and a constant added to every support value
  // cannot move the argmax — so this is the convex hull of the seed points.
  static Circle stable[COUNT];
  for (int i = 0; i < nCircles; i++) stable[i] = Circle{ positions[i].x, positions[i].y, MAX_R };

  static int order[COUNT + 2];
  const int  nOrder = hullIndices(stable, nCircles, order);

  bool onHull[COUNT] = { false };
  nBoundary = 0;
  for (int i = 0; i < nOrder; i++)
    if (!onHull[order[i]]) { onHull[order[i]] = true; boundaryIdx[nBoundary++] = (uint8_t)order[i]; }

  // Each interior node connects to its CONN nearest boundary nodes. The JS
  // sorts a copy of boundaryIndices, and Array.sort is stable, so equal
  // distances keep boundary order — hence the strict `<` below.
  nEdges = 0;
  for (int j = 0; j < nCircles; j++) {
    if (onHull[j]) continue;

    float dist[COUNT];
    bool  taken[COUNT] = { false };
    for (int b = 0; b < nBoundary; b++) {
      const float dx = positions[boundaryIdx[b]].x - positions[j].x;
      const float dy = positions[boundaryIdx[b]].y - positions[j].y;
      dist[b] = sqrtf(dx * dx + dy * dy);
    }

    const int k = CONN < nBoundary ? CONN : nBoundary;
    for (int m = 0; m < k; m++) {
      int pick = -1;
      for (int b = 0; b < nBoundary; b++)
        if (!taken[b] && (pick < 0 || dist[b] < dist[pick])) pick = b;
      if (pick < 0) break;
      taken[pick] = true;
      edgeA[nEdges] = (uint8_t)j;
      edgeB[nEdges] = boundaryIdx[pick];
      nEdges++;
    }
  }

  Serial.printf("%d nodes, %d on the hull, %d tendons, minDist %.1f\n",
                nCircles, nBoundary, nEdges, minDist);
}

static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);
  noiseSeed();            // Random.setSeed() rebuilds the noise table first
  buildPalette();
  layout();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

static void relax() {
  for (int it = 0; it < RELAX_ITERS; it++) {
    for (int i = 0; i < nCircles; i++)
      for (int j = i + 1; j < nCircles; j++) {
        Circle &a = circles[i];
        Circle &b = circles[j];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float d  = sqrtf(dx * dx + dy * dy);
        const float minD = a.r + b.r;
        if (d >= minD || d < 1e-6f) continue;
        const float overlap = minD - d;
        const float nx = dx / d, ny = dy / d;
        const float aShare = b.r / minD, bShare = a.r / minD;
        a.x -= nx * overlap * aShare;  a.y -= ny * overlap * aShare;
        b.x += nx * overlap * bShare;  b.y += ny * overlap * bShare;
      }
    for (int i = 0; i < nCircles; i++) {
      circles[i].x = clampf(circles[i].x, circles[i].r, (float)W - circles[i].r);
      circles[i].y = clampf(circles[i].y, circles[i].r, (float)H - circles[i].r);
    }
  }
}

static void drawHull() {
  static Circle layer[COUNT + 2];
  for (int k = 0; k < LAYERS; k++) {
    bool ok = true;
    for (int i = 0; i < nHull; i++) {
      layer[i] = circles[hullOrder[i]];
      layer[i].r -= (float)k * HULL_SP;
      if (layer[i].r <= 0.0f) { ok = false; break; }
    }
    if (!ok) break;                        // the JS drops the whole layer too
    strokeRubberBand(layer, nHull);
  }
}

static void drawTendons() {
  for (int e = 0; e < nEdges; e++) {
    const Circle &ci = circles[edgeA[e]];
    const Circle &cj = circles[edgeB[e]];
    const float dx = cj.x - ci.x, dy = cj.y - ci.y;
    const float d  = sqrtf(dx * dx + dy * dy);
    if (d <= ci.r + cj.r + GAP_EPS) continue;

    for (int k = 0; k < LAYERS; k++) {
      Circle a = ci, b = cj;
      a.r -= (float)k * LAYER_SP;
      b.r -= (float)k * LAYER_SP;
      if (a.r <= 0.0f || b.r <= 0.0f) break;

      // Only the outermost fill can change anything. Layer k's region is layer
      // k-1's eroded by LAYER_SP, and the only ink inside layer k-1 is its own
      // stroke, which reaches STROKE_E in — so every inner fill repaints
      // background over background. See the static_assert above.
      bandDraw(a, b, k == 0);
    }
  }
}

static void drawCircles() {
  for (int i = 0; i < nCircles; i++) {
    const Circle &c = circles[i];
    fillDisk(c.x, c.y, c.r);

    int rings = (int)floorf(c.r / LAYER_SP);
    if (rings > LAYERS) rings = LAYERS;
    for (int k = 0; k < rings; k++) {
      const float rk = c.r - (float)k * LAYER_SP;
      if (rk <= 0.0f) break;
      strokeCircleBand(c.x, c.y, rk, nullptr);
    }
    strokeCircleBand(c.x, c.y, CENTER_R, nullptr);
  }
}

static void composite() {
  static lgfx::rgb565_t row[W];
  for (int y = 0; y < H; y++) {
    const uint8_t *src = ab + y * W;
    for (int x = 0; x < W; x++) row[x].raw = inkLut[src[x]];
    cv.pushImage(0, y, W, 1, row);
  }
}

static uint32_t tGeom, tHull, tTendon, tDisk, tComp, tPush;

static void drawFrame(float playhead) {
  const uint32_t t0 = wallMicros();

  // The noise loops on a circle in (z, w) — and the JS winds it twice per
  // playhead, so the piece breathes at 4s inside an 8s loop.
  const float angle = 2.0f * (float)M_PI * playhead * 2.0f;
  const float pz = (sinf(angle) + 1.0f) * 0.25f;
  const float pw = (cosf(angle) + 1.0f) * 0.25f;

  for (int i = 0; i < nCircles; i++) {
    const float nx = positions[i].x * NOISE_K;
    const float ny = positions[i].y * NOISE_K;
    const float t  = simplexNoise4D(nx, ny, pz, pw);
    circles[i].r = clampf(MIN_R + (t + 1.0f) * 0.5f * (MAX_R - MIN_R), MIN_R, MAX_R);
    circles[i].x = positions[i].x + simplexNoise4D(nx + NOISE_OFF, ny, pz, pw) * DRIFT;
    circles[i].y = positions[i].y + simplexNoise4D(nx, ny + NOISE_OFF, pz, pw) * DRIFT;
  }
  relax();
  nHull = hullIndices(circles, nCircles, hullOrder);

  const uint32_t t1 = wallMicros();
  memset(ab, 0, AB_BYTES);
  if (nHull >= 2) drawHull();              // the JS bails on the whole frame here

  const uint32_t t2 = wallMicros();
  if (nHull >= 2) drawTendons();

  const uint32_t t3 = wallMicros();
  if (nHull >= 2) drawCircles();

  const uint32_t t4 = wallMicros();
  composite();

  const uint32_t t5 = wallMicros();
  present();

  const uint32_t t6 = wallMicros();
  tGeom = t1 - t0; tHull = t2 - t1; tTendon = t3 - t2;
  tDisk = t4 - t3; tComp = t5 - t4;  tPush   = t6 - t5;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  // Guarded, not unconditional: switcher.cpp re-runs this setup() on every
  // switch back, and allocating each time would leak 55KB a visit.
  if (!ab) ab = (uint8_t *)psramAlloc(AB_BYTES);
  if (!ab) {
    Serial.println("FATAL: coverage plane allocation failed");
    while (true) delay(1000);
  }

  for (int s = 0; s < HULL_SAMPLES; s++) {
    const float th = (float)s / (float)HULL_SAMPLES * 2.0f * (float)M_PI;
    hullCos[s] = cosf(th);
    hullSin[s] = sinf(th);
  }

  generate(esp_random());
}

void loop() {
  static uint32_t start   = 0;
  static bool     started = false;
  static uint32_t frames  = 0;

  // One composition, held for as long as the board is powered — the JS caches
  // its layout and never rebuilds it either. RESET is the way to a new one.
  const uint32_t now = millis();
  if (!started) { start = now; started = true; }

  drawFrame((float)((now - start) % LOOP_MS) / (float)LOOP_MS);

  const uint32_t total = tGeom + tHull + tTendon + tDisk + tComp + tPush;
  if (++frames % 24 == 0)
    Serial.printf("frame %lu.%02lums = geom %lu + hull %lu + tendons %lu + disks %lu"
                  " + composite %lu + push %luus\n",
                  (unsigned long)(total / 1000), (unsigned long)(total % 1000) / 10,
                  (unsigned long)tGeom, (unsigned long)tHull, (unsigned long)tTendon,
                  (unsigned long)tDisk, (unsigned long)tComp, (unsigned long)tPush);

  const uint32_t spent = total / 1000;
  if (spent < (uint32_t)FRAME_MS) delay((uint32_t)FRAME_MS - spent);
}
