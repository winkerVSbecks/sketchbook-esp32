// ============================================================================
// collapse — negative-space blinds that fall together under gravity
// ============================================================================
//   pio run -e collapse_native -t exec      SDL window
//   pio run -e collapse_esp32  -t upload    the board
//   pio run -e collapse_shot   -t exec      headless PNG capture
//
// A greedy pass scatters rectangles across a cell grid, scoring each candidate
// on how well it balances the mass around an off-centre anchor and how little
// accidental gutter it leaves. Then gravity pulls every rect one cell at a time
// toward the centre until nothing can move, and a seal pass — which is allowed
// to overlap — drags each rect over its nearer neighbours until every lane of
// its span is in contact, so no gap survives inside the mass. A backdrop
// shrink-wraps the pack.
//
// The rects never move once built. Only the colour panels clipped inside them
// slide, which is what makes this cheap: one dirty-rect push per moving blind
// instead of a 22ms full-frame blit.
// ============================================================================

#define SKETCH_TITLE  "collapse"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/harmony.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// The JS carries a tweakpane with four strategy dropdowns. Each one is pinned
// in its config object, so exactly one arm of each ever runs; those are the
// arms below. Named here so it's obvious what was left on the table:
//   axis    'narrow'    (also: long, vertical, horizontal, random)
//   dir     'uniform'   (also: alternate, random)  -> always +1
//   phase   'group'     (also: uniform, stagger, randomQuantized)
//   pairing 'gradient'  (also: chained, pairs)
static const uint32_t LOOP_MS     = 6000;   // one full blind cycle
static const int      LOOP_REPEAT = 4;      // cycles before a new composition

static const int   MARGIN        = 1;       // empty border, in cells
static const float INK_RATIO     = 0.32f;   // stop placing once this much is inked
static const int   CANDIDATES    = 32;      // samples per placement
static const int   TOP_K         = 3;       // tournament width among the best
static const int   MIN_SIDE      = 2;       // cells
static const int   WRAP_PAD      = 1;       // backdrop margin around the pack
static const int   PANELS        = 2;       // colour panels per blind
static const float DWELL         = 0.5f;    // 0 = continuous slide, ->1 = hard steps
static const float STATIC_CHANCE = 0.1f;
static const bool  ALIGN_TOUCH   = true;
static const bool  BG_TINT       = true;

// Grid. The original canvas is square and lays out on res x res with res in
// [16, 32); this panel is 172x320, so a single res would make every cell 1.86x
// taller than wide and quietly turn sampleCell's "square" class into a tall
// rect. Two resolutions instead, with RES_Y derived so cells come out square,
// which keeps the shape classes meaning what they say. Everything downstream —
// gravity, the seal pass, the distance field — runs in cell space and only
// cares that the grid is rectangular.
static const int RES_X_MIN = 8, RES_X_MAX = 13;    // rangeFloor: 8..12
static const int MAX_RES_X = 12, MAX_RES_Y = 22;
static const int MAX_CELLS = MAX_RES_X * MAX_RES_Y;
static const int MAX_RECTS = 16;

// Cell counts land near the original's low end (120..264 vs 256..961), so the
// rect sizes are rescaled to match: sides are a fraction of RES_X rather than
// the JS's flat 2..10, and the rect budget is smaller.
static const int  RECT_BUDGET[]  = { 4, 6, 9, 12 };
static const int  RECT_BUDGET_N  = 4;
static const float MAX_SIDE_FRAC = 0.55f;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct CellRect { int8_t x, y, w, h; };

struct Blind {
  int16_t  px, py, pw, ph;        // pixel bounds, snapped to integer cell edges
  uint32_t colors[PANELS];
  float    phase;
  uint8_t  axis;                  // 0 = slides along x, 1 = along y
  bool     isStatic;
};

static int   resX, resY, nCells;
static float resMean;             // stands in for the JS's single `res`
static int   maxRects, maxSide, minSide;
static float offCenter, voidWeight;

static uint8_t  occ[MAX_CELLS];
static uint8_t  trialOcc[MAX_CELLS];
static int16_t  distField[MAX_CELLS];
static int16_t  bfsQueue[MAX_CELLS];

static CellRect rects[MAX_RECTS];
static int      nRects;
static Blind    blinds[MAX_RECTS];

static uint32_t palette[HARMONY_N];
static uint32_t backdropColor;    // lightest palette entry
static uint32_t canvasColor;      // the tint the pack floats on

static int16_t  backdrop[4];      // x, y, w, h in px
static int      colEdge[MAX_RES_X + 1], rowEdge[MAX_RES_Y + 1];

static uint32_t compositionAt;    // millis() when the current pack was built

static inline float wrap01(float v) { return v - floorf(v); }

// ---------------------------------------------------------------------------
// Negative-space layout
// ---------------------------------------------------------------------------

// Multi-source BFS from every inked cell: 0 on ink, growing into the void.
// Returns the deepest distance reached.
static int distanceField(const uint8_t *o) {
  for (int i = 0; i < nCells; i++) distField[i] = -1;

  int qn = 0;
  for (int i = 0; i < nCells; i++) if (o[i]) { distField[i] = 0; bfsQueue[qn++] = (int16_t)i; }
  if (qn == 0) {
    const int d = resX > resY ? resX : resY;
    for (int i = 0; i < nCells; i++) distField[i] = (int16_t)d;
    return d;
  }

  for (int head = 0; head < qn; head++) {
    const int i = bfsQueue[head];
    const int x = i % resX, y = i / resX;
    const int16_t d = distField[i] + 1;
    if (x > 0        && distField[i - 1]    < 0) { distField[i - 1]    = d; bfsQueue[qn++] = (int16_t)(i - 1); }
    if (x < resX - 1 && distField[i + 1]    < 0) { distField[i + 1]    = d; bfsQueue[qn++] = (int16_t)(i + 1); }
    if (y > 0        && distField[i - resX] < 0) { distField[i - resX] = d; bfsQueue[qn++] = (int16_t)(i - resX); }
    if (y < resY - 1 && distField[i + resX] < 0) { distField[i + resX] = d; bfsQueue[qn++] = (int16_t)(i + resX); }
  }

  int dMax = 0;
  for (int i = 0; i < nCells; i++) if (distField[i] > dMax) dMax = distField[i];
  return dMax;
}

// Lower is better: penalise in-between emptiness (accidental gutters), reward
// the depth of the largest void — capped, so once a void is deep enough there's
// no further incentive to huddle the ink.
static float voidScore(const uint8_t *o) {
  const int dMax = distanceField(o);

  int farThresh = (int)lroundf(resMean * 0.2f);
  if (farThresh < 2) farThresh = 2;

  int empty = 0, mid = 0;
  for (int i = 0; i < nCells; i++) {
    const int d = distField[i];
    if (d <= 0) continue;
    empty++;
    if (d > 1 && d < farThresh) mid++;
  }

  const float midFrac  = empty > 0 ? (float)mid / (float)empty : 0.0f;
  const float depthCap = resMean * 0.3f;
  const float depth    = (float)dMax < depthCap ? (float)dMax : depthCap;
  return midFrac - depth / depthCap;
}

static bool sampleCell(CellRect &r) {
  const int innerX = resX - MARGIN * 2, innerY = resY - MARGIN * 2;
  const int third  = maxSide / 3;
  const int thin   = minSide > third ? minSide : third;

  int w, h;
  switch (rngIndex(3)) {                                     // tall / wide / square
    case 0:
      w = rngRangeFloor(minSide, thin + 1);
      h = rngRangeFloor(w * 2 < maxSide ? w * 2 : maxSide, maxSide + 1);
      break;
    case 1:
      h = rngRangeFloor(minSide, thin + 1);
      w = rngRangeFloor(h * 2 < maxSide ? h * 2 : maxSide, maxSide + 1);
      break;
    default:
      w = rngRangeFloor(minSide, maxSide + 1);
      h = w;
      break;
  }
  if (w > innerX || h > innerY) return false;

  r.w = (int8_t)w;
  r.h = (int8_t)h;
  r.x = (int8_t)rngRangeFloor(MARGIN, resX - MARGIN - w + 1);
  r.y = (int8_t)rngRangeFloor(MARGIN, resY - MARGIN - h + 1);
  return true;
}

static bool cellsOccupied(const uint8_t *o, const CellRect &r) {
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) if (o[y * resX + x]) return true;
  return false;
}

static void mark(uint8_t *o, const CellRect &r) {
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) o[y * resX + x] = 1;
}

// Rects abutting along an edge must share at least one flush perpendicular edge
// (tops/bottoms when side by side, lefts/rights when stacked) — no staggered
// seams. Corner-only contact and rects that don't touch both pass.
static bool touchAligned(const CellRect &a, const CellRect &b) {
  const int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
  const int xOverlap = (ax1 < bx1 ? ax1 : bx1) - (a.x > b.x ? a.x : b.x);
  const int yOverlap = (ay1 < by1 ? ay1 : by1) - (a.y > b.y ? a.y : b.y);

  if ((ax1 == b.x || bx1 == a.x) && yOverlap > 0) return a.y == b.y || ay1 == by1;
  if ((ay1 == b.y || by1 == a.y) && xOverlap > 0) return a.x == b.x || ax1 == bx1;
  return true;
}

static inline bool rectsOverlap(const CellRect &a, const CellRect &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Rects by distance of their centre from the grid centre. Array.sort in JS is
// stable, so ties keep placement order; the insertion sort below matches that.
static void orderByCentre(int *order, bool nearestFirst) {
  float d[MAX_RECTS];
  const float cx = resX * 0.5f, cy = resY * 0.5f;
  for (int i = 0; i < nRects; i++) {
    const float dx = rects[i].x + rects[i].w * 0.5f - cx;
    const float dy = rects[i].y + rects[i].h * 0.5f - cy;
    d[i] = sqrtf(dx * dx + dy * dy);
    order[i] = i;
  }
  for (int i = 1; i < nRects; i++) {
    const int   oi = order[i];
    const float di = d[oi];
    int j = i - 1;
    while (j >= 0 && (nearestFirst ? d[order[j]] > di : d[order[j]] < di)) { order[j + 1] = order[j]; j--; }
    order[j + 1] = oi;
  }
}

static bool tryMoveRect(int i, int dx, int dy) {
  CellRect c = rects[i];
  c.x = (int8_t)(c.x + dx);
  c.y = (int8_t)(c.y + dy);
  if (c.x < 0 || c.y < 0 || c.x + c.w > resX || c.y + c.h > resY) return false;
  for (int j = 0; j < nRects; j++) if (j != i && rectsOverlap(c, rects[j])) return false;
  rects[i] = c;
  return true;
}

// Slide rect i toward the centre along one axis by the LARGEST per-lane gap to
// the first block ahead in that lane. Lanes already in contact contribute 0;
// lanes with nothing ahead are ignored. Moving by the max seals every lane at
// once, overlapping the nearer blocks — which the collapse explicitly permits.
static bool slideToContact(int i, int axis) {
  CellRect &r = rects[i];
  const int mainPos   = axis == 0 ? r.x : r.y;
  const int mainSize  = axis == 0 ? r.w : r.h;
  const int crossPos  = axis == 0 ? r.y : r.x;
  const int crossSize = axis == 0 ? r.h : r.w;

  const float toCentre = (axis == 0 ? resX : resY) * 0.5f - (mainPos + mainSize * 0.5f);
  if (toCentre == 0.0f) return false;
  const int s = toCentre > 0.0f ? 1 : -1;

  int d = 0;
  for (int lane = crossPos; lane < crossPos + crossSize; lane++) {
    int gap = -1;                                            // -1 stands for Infinity
    for (int j = 0; j < nRects; j++) {
      if (j == i) continue;
      const CellRect &o = rects[j];
      const int oMain      = axis == 0 ? o.x : o.y;
      const int oMainSize  = axis == 0 ? o.w : o.h;
      const int oCross     = axis == 0 ? o.y : o.x;
      const int oCrossSize = axis == 0 ? o.h : o.w;
      if (lane < oCross || lane >= oCross + oCrossSize) continue;

      int g;
      if (oMain < mainPos + mainSize && mainPos < oMain + oMainSize) g = 0;   // in contact
      else if (s > 0 && oMain >= mainPos + mainSize)                 g = oMain - (mainPos + mainSize);
      else if (s < 0 && oMain + oMainSize <= mainPos)                g = mainPos - (oMain + oMainSize);
      else continue;                                                          // behind us
      if (gap < 0 || g < gap) gap = g;
    }
    if (gap >= 0 && gap > d) d = gap;
  }

  const int cap = (int)floorf(fabsf(toCentre));              // never cross the centre line
  if (d > cap) d = cap;
  if (d <= 0) return false;

  if (axis == 0) r.x = (int8_t)(r.x + s * d);
  else           r.y = (int8_t)(r.y + s * d);
  return true;
}

// The no-overlap gravity stops a rect at its FIRST contact, which leaves
// notches where the rest of its span still faces a gap. Working from the
// outside in, slide everything to full-span contact until stable.
static void sealGaps() {
  const int maxRounds = 4 * (resX > resY ? resX : resY);
  bool moved = true;
  for (int round = 0; moved && round < maxRounds; round++) {
    moved = false;
    int order[MAX_RECTS];
    orderByCentre(order, false);
    for (int k = 0; k < nRects; k++) {
      if (slideToContact(order[k], 0)) moved = true;
      if (slideToContact(order[k], 1)) moved = true;
    }
  }
}

// Gravity toward the centre: in rounds, nearest rect first, each rect slides one
// cell along x and one along y if the space is free. Converges when nothing can
// move — every rect then sits flush against a neighbour or the centre line.
static void collapseRects() {
  const float cx = resX * 0.5f, cy = resY * 0.5f;
  const int maxRounds = 4 * (resX > resY ? resX : resY);

  bool moved = true;
  for (int round = 0; moved && round < maxRounds; round++) {
    moved = false;
    int order[MAX_RECTS];
    orderByCentre(order, true);
    for (int k = 0; k < nRects; k++) {
      const int i = order[k];
      const float ddx = cx - (rects[i].x + rects[i].w * 0.5f);
      const float ddy = cy - (rects[i].y + rects[i].h * 0.5f);
      if (fabsf(ddx) > 0.5f && tryMoveRect(i, ddx > 0.0f ? 1 : -1, 0)) moved = true;
      if (fabsf(ddy) > 0.5f && tryMoveRect(i, 0, ddy > 0.0f ? 1 : -1)) moved = true;
    }
  }
  sealGaps();
}

// Greedy placement: each step samples candidates and keeps one of the best few
// by moment imbalance toward the anchor plus the void penalty. A tournament
// rather than the strict argmin, which converges to a corner-touching staircase.
static void layout() {
  memset(occ, 0, (size_t)nCells);
  nRects = 0;

  const float tx = resX * 0.5f + rngRange(-1.0f, 1.0f) * offCenter * (resX * 0.5f);
  const float ty = resY * 0.5f + rngRange(-1.0f, 1.0f) * offCenter * (resY * 0.5f);

  float sumW = 0.0f, sumWx = 0.0f, sumWy = 0.0f;

  while (nRects < maxRects && sumW / (float)nCells < INK_RATIO) {
    CellRect cand[CANDIDATES];
    float    score[CANDIDATES];
    int      nScored = 0;

    for (int c = 0; c < CANDIDATES; c++) {
      CellRect r;
      if (!sampleCell(r) || cellsOccupied(occ, r)) continue;
      if (ALIGN_TOUCH) {
        bool aligned = true;
        for (int p = 0; p < nRects; p++) if (!touchAligned(r, rects[p])) { aligned = false; break; }
        if (!aligned) continue;
      }

      const float w  = (float)(r.w * r.h);
      const float cx = (sumWx + w * (r.x + r.w * 0.5f)) / (sumW + w);
      const float cy = (sumWy + w * (r.y + r.h * 0.5f)) / (sumW + w);
      const float ex = cx - tx, ey = cy - ty;

      memcpy(trialOcc, occ, (size_t)nCells);
      mark(trialOcc, r);

      cand[nScored]  = r;
      score[nScored] = sqrtf(ex * ex + ey * ey) / resMean + voidWeight * voidScore(trialOcc);
      nScored++;
    }
    if (nScored == 0) break;

    for (int i = 1; i < nScored; i++) {                       // stable, ascending
      const CellRect ri = cand[i];
      const float    si = score[i];
      int j = i - 1;
      while (j >= 0 && score[j] > si) { score[j + 1] = score[j]; cand[j + 1] = cand[j]; j--; }
      score[j + 1] = si;
      cand[j + 1]  = ri;
    }

    const CellRect r = cand[rngRangeFloor(0, TOP_K < nScored ? TOP_K : nScored)];
    mark(occ, r);
    const float w = (float)(r.w * r.h);
    sumW  += w;
    sumWx += w * (r.x + r.w * 0.5f);
    sumWy += w * (r.y + r.h * 0.5f);
    rects[nRects++] = r;
  }

  collapseRects();

  // Re-ink from the collapsed rects — `occ` above tracked the scattered
  // placement, and gravity has moved everything since. The seal pass is allowed
  // to overlap, which mark() absorbs.
  memset(occ, 0, (size_t)nCells);
  for (int i = 0; i < nRects; i++) mark(occ, rects[i]);
}

// The premise of the piece is that no gap survives inside the mass, so count
// what did: void cells with no path out to the edge of the grid. Flood the
// outside and see what's left.
static int enclosedVoids() {
  memcpy(trialOcc, occ, (size_t)nCells);            // 1 == ink, or void reached
  int qn = 0;

  for (int x = 0; x < resX; x++) {
    const int top = x, bot = (resY - 1) * resX + x;
    if (!trialOcc[top]) { trialOcc[top] = 1; bfsQueue[qn++] = (int16_t)top; }
    if (!trialOcc[bot]) { trialOcc[bot] = 1; bfsQueue[qn++] = (int16_t)bot; }
  }
  for (int y = 0; y < resY; y++) {
    const int l = y * resX, r = l + resX - 1;
    if (!trialOcc[l]) { trialOcc[l] = 1; bfsQueue[qn++] = (int16_t)l; }
    if (!trialOcc[r]) { trialOcc[r] = 1; bfsQueue[qn++] = (int16_t)r; }
  }
  for (int head = 0; head < qn; head++) {
    const int i = bfsQueue[head], x = i % resX, y = i / resX;
    if (x > 0        && !trialOcc[i - 1])    { trialOcc[i - 1]    = 1; bfsQueue[qn++] = (int16_t)(i - 1); }
    if (x < resX - 1 && !trialOcc[i + 1])    { trialOcc[i + 1]    = 1; bfsQueue[qn++] = (int16_t)(i + 1); }
    if (y > 0        && !trialOcc[i - resX]) { trialOcc[i - resX] = 1; bfsQueue[qn++] = (int16_t)(i - resX); }
    if (y < resY - 1 && !trialOcc[i + resX]) { trialOcc[i + resX] = 1; bfsQueue[qn++] = (int16_t)(i + resX); }
  }

  int holes = 0;
  for (int i = 0; i < nCells; i++) if (!trialOcc[i]) holes++;
  return holes;
}

// Area rank, 0 = largest mass. Descending and stable, so equal areas keep
// placement order the way Array.sort does.
static void rankByArea(int *rank) {
  int order[MAX_RECTS], area[MAX_RECTS];
  for (int i = 0; i < nRects; i++) { order[i] = i; area[i] = rects[i].w * rects[i].h; }
  for (int i = 1; i < nRects; i++) {
    const int oi = order[i], ai = area[oi];
    int j = i - 1;
    while (j >= 0 && area[order[j]] < ai) { order[j + 1] = order[j]; j--; }
    order[j + 1] = oi;
  }
  for (int k = 0; k < nRects; k++) rank[order[k]] = k;
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
static void buildPalette() {
  const Oklch base = { rngRange(0.4f, 0.8f), rngRange(0.1f, 0.4f), rngRange(0.0f, 360.0f) };
  const HarmonyStyle style = (HarmonyStyle)rngIndex(HS_COUNT);

  Oklch ramp[HARMONY_N];
  harmonyGenerate(base, HARM_TINTS_SHADES, style, ramp);
  for (int i = 0; i < HARMONY_N; i++) palette[i] = oklchToRgb(ramp[i]);

  // tintsShades never touches the lightness stops, so paletteLightest() always
  // lands on the last one — but ask it anyway, so swapping in another harmony
  // doesn't silently pick the wrong entry.
  backdropColor = paletteLightest(palette, HARMONY_N);

  // The JS writes this as CSS relative colour: oklch(from bg calc(l*2)
  // calc(c*.1) h). With l already at 0.98 the doubling always saturates, so the
  // canvas is white and `bgTint` can't actually change anything under this
  // harmony — see the note in the report. Kept because it would matter the
  // moment `lightest()` returns a mid-lightness entry.
  const Oklch b = rgbToOklch(backdropColor);
  canvasColor = BG_TINT ? oklchToRgb(clampf(b.l * 2.0f, 0.0f, 1.0f), b.c * 0.1f, b.h)
                        : 0xFFFFFF;

  Serial.printf("style %s  base oklch(%.2f %.3f %.0f)\n",
                HARMONY_STYLE_NAMES[style], base.l, base.c, base.h);
  for (int i = 0; i < HARMONY_N; i++)
    Serial.printf("%s#%06lX", i ? " " : "palette: ", (unsigned long)palette[i]);
  Serial.println();
}

// ---------------------------------------------------------------------------
// Blinds
// ---------------------------------------------------------------------------
static void buildBlinds() {
  int rank[MAX_RECTS];
  rankByArea(rank);

  for (int i = 0; i < nRects; i++) {
    const CellRect &r = rects[i];
    Blind &b = blinds[i];

    b.px = (int16_t)colEdge[r.x];
    b.py = (int16_t)rowEdge[r.y];
    b.pw = (int16_t)(colEdge[r.x + r.w] - b.px);
    b.ph = (int16_t)(rowEdge[r.y + r.h] - b.py);

    b.axis     = b.ph > b.pw ? 0 : 1;                        // 'narrow': slide the short way
    b.phase    = nRects > 1 ? (float)rank[i] / (float)(nRects - 1) : 0.0f;
    b.isStatic = rngChance(STATIC_CHANCE);

    // 'gradient': one entry of the ramp per mass rank, then the placement order
    // fans it +-0.12 in lightness and the panels a further +-0.05.
    const uint32_t base = palette[rank[i] % HARMONY_N];
    const float    ramp = nRects > 1 ? 0.12f - 0.24f * (float)i / (float)(nRects - 1) : 0.0f;
    for (int k = 0; k < PANELS; k++)
      b.colors[k] = shadeL(base, ramp + 0.05f - 0.10f * (float)k / (float)(PANELS - 1));
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// Dwell-then-move: hold for `dwell` of each step, then slide one panel extent
// linearly. dwell 0 = continuous slide, dwell -> 1 = hard steps.
static float dwellSlide(float t, int steps, float dwell) {
  const float total = t * (float)steps;
  const float i     = floorf(total);
  const float f     = total - i;
  const float span  = 1.0f - dwell > 1e-6f ? 1.0f - dwell : 1e-6f;
  const float move  = clampf((f - dwell) / span, 0.0f, 1.0f);
  return i + move < (float)steps ? i + move : (float)steps;
}

static void drawBlind(const Blind &b, float t) {
  if (b.isStatic) {
    cv.fillRect(b.px, b.py, b.pw, b.ph, to565(b.colors[0]));
    return;
  }

  const int   extentI = b.axis == 0 ? b.pw : b.ph;
  const float extent  = (float)extentI;
  const float slide   = dwellSlide(wrap01(t + b.phase), PANELS, DWELL) * extent;

  // Band k spans [k*extent - slide, (k+1)*extent - slide) inside the blind.
  // Drawing them in order lets each overwrite the tail of the one before,
  // exactly as the JS does with PANELS+1 overlapping fillRects under a clip.
  cv.setClipRect(b.px, b.py, b.pw, b.ph);
  for (int k = 0; k <= PANELS; k++) {
    const uint32_t col = b.colors[k % PANELS];
    const float    off = (float)k * extent - slide;
    const int      o   = (int)floorf(off);

    if (b.axis == 0) cv.fillRect(b.px + o, b.py, b.pw, b.ph, to565(col));
    else             cv.fillRect(b.px, b.py + o, b.pw, b.ph, to565(col));

    // The leading edge lands mid-pixel. Blend that one line against the band it
    // just covered, or the edge snaps a whole pixel at a time — very visible on
    // a 172px panel where a blind is only 20-60px across.
    if (k > 0 && o >= 0 && o < extentI) {
      const uint16_t mix = blend565(b.colors[(k - 1) % PANELS], col, 1.0f - (off - (float)o));
      if (b.axis == 0) cv.drawFastVLine(b.px + o, b.py, b.ph, mix);
      else             cv.drawFastHLine(b.px, b.py + o, b.pw, mix);
    }
  }
  cv.clearClipRect();
}

static void renderAll(float t) {
  cv.fillScreen(to565(canvasColor));
  if (nRects > 0) cv.fillRect(backdrop[0], backdrop[1], backdrop[2], backdrop[3], to565(backdropColor));
  for (int i = 0; i < nRects; i++) drawBlind(blinds[i], t);
}

// Push only the blinds that moved. A full frame is 55040 px, ~22ms of SPI at
// 40MHz; the moving blinds are a fraction of that. Headless still captures
// whole frames — the sprite always holds the complete composition, so present()
// is correct there and the dirty rects are purely a transfer optimisation.
static void presentBlinds() {
#if defined(SKETCH_HEADLESS)
  present();
#else
  lcd.startWrite();
  for (int i = 0; i < nRects; i++) {
    const Blind &b = blinds[i];
    if (b.isStatic) continue;
    lcd.setClipRect(b.px, b.py, b.pw, b.ph);
    cv.pushSprite(&lcd, 0, 0);
  }
  lcd.clearClipRect();
  lcd.endWrite();
#endif
}

// ---------------------------------------------------------------------------
static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  // The JS draws res / maxRects / offCenter / voidWeight at module scope, which
  // runs before Random.setSeed() — so a seed doesn't reproduce a composition
  // there. Drawn from the seeded stream here instead.
  resX = rngRangeFloor(RES_X_MIN, RES_X_MAX);
  resY = (int)lroundf((float)resX * (float)H / (float)W);
  if (resY > MAX_RES_Y) resY = MAX_RES_Y;
  nCells  = resX * resY;
  resMean = 0.5f * (float)(resX + resY);

  const int innerX = resX - MARGIN * 2, innerY = resY - MARGIN * 2;
  maxSide = (int)lroundf((float)resX * MAX_SIDE_FRAC);
  if (maxSide > innerX) maxSide = innerX;
  if (maxSide > innerY) maxSide = innerY;
  if (maxSide < 1)      maxSide = 1;
  minSide = MIN_SIDE > maxSide ? maxSide : MIN_SIDE;

  maxRects = rngPick(RECT_BUDGET, RECT_BUDGET_N);
  if (maxRects > MAX_RECTS) maxRects = MAX_RECTS;

  offCenter  = rngRange(0.0f, 0.6f);
  voidWeight = rngRange(0.0f, 2.0f);

  for (int i = 0; i <= resX; i++) colEdge[i] = (int)lroundf(i * (float)W / (float)resX);
  for (int j = 0; j <= resY; j++) rowEdge[j] = (int)lroundf(j * (float)H / (float)resY);

  buildPalette();
  layout();

  // Shrink-wrap the pack: its cell bounds plus WRAP_PAD, snapped to cell edges.
  if (nRects > 0) {
    int minX = resX, minY = resY, maxX = 0, maxY = 0;
    for (int i = 0; i < nRects; i++) {
      const CellRect &r = rects[i];
      if (r.x < minX) minX = r.x;
      if (r.y < minY) minY = r.y;
      if (r.x + r.w > maxX) maxX = r.x + r.w;
      if (r.y + r.h > maxY) maxY = r.y + r.h;
    }
    minX = minX - WRAP_PAD < 0 ? 0 : minX - WRAP_PAD;
    minY = minY - WRAP_PAD < 0 ? 0 : minY - WRAP_PAD;
    maxX = maxX + WRAP_PAD > resX ? resX : maxX + WRAP_PAD;
    maxY = maxY + WRAP_PAD > resY ? resY : maxY + WRAP_PAD;
    backdrop[0] = (int16_t)colEdge[minX];
    backdrop[1] = (int16_t)rowEdge[minY];
    backdrop[2] = (int16_t)(colEdge[maxX] - backdrop[0]);
    backdrop[3] = (int16_t)(rowEdge[maxY] - backdrop[1]);
  }

  buildBlinds();

  int dirty = 0, moving = 0;
  for (int i = 0; i < nRects; i++)
    if (!blinds[i].isStatic) { dirty += blinds[i].pw * blinds[i].ph; moving++; }

  Serial.printf("grid %dx%d  budget %d  ink %d rects (%d moving)  offCenter %.2f  voidWeight %.2f  holes %d\n",
                resX, resY, maxRects, nRects, moving, offCenter, voidWeight, enclosedVoids());
  Serial.printf("dirty %d px/frame -> %.1fms SPI @40MHz (full frame %.1fms)\n",
                dirty, dirty * 0.0004f, W * H * 0.0004f);

  const uint32_t t0 = wallMicros();
  renderAll(0.0f);
  const uint32_t drawUs = wallMicros() - t0;
  present();
  Serial.printf("first frame drawn in %luus\n", (unsigned long)drawUs);

  compositionAt = millis();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  generate(esp_random());
}

void loop() {
  const uint32_t frameStart = millis();
  const uint32_t elapsed    = frameStart - compositionAt;
  if (elapsed >= LOOP_MS * LOOP_REPEAT) {
    generate(esp_random());
    return;
  }

  const float t = (float)(elapsed % LOOP_MS) / (float)LOOP_MS;

  const uint32_t t0 = wallMicros();
  for (int i = 0; i < nRects; i++) if (!blinds[i].isStatic) drawBlind(blinds[i], t);
  const uint32_t drawUs = wallMicros() - t0;

  presentBlinds();

  // One line per second of sketch time, so the real cost of a frame is visible
  // on serial without drowning it. wallMicros() is a wall clock in every build; only
  // the reporting interval rides the virtual one.
  static uint32_t reportAt = 0, accUs = 0, accN = 0;
  accUs += drawUs;
  accN++;
  if (millis() - reportAt >= 1000) {
    reportAt = millis();
    Serial.printf("frame: draw %luus avg over %lu frames\n",
                  (unsigned long)(accUs / accN), (unsigned long)accN);
    accUs = 0;
    accN  = 0;
  }

#if defined(SKETCH_HEADLESS)
  // The headless clock only moves when the sketch asks it to (16ms a loop, plus
  // this), so stepping a whole SKETCH_FRAMES-th of the cycle per capture makes
  // one run cover the full loop instead of its first quarter-second.
  delay(LOOP_MS / SKETCH_FRAMES - 16);
#else
  // Pace to the browser's 60fps. The playhead is derived from millis(), so a
  // dropped frame costs smoothness and nothing else — but with the dirty-rect
  // push coming in around 8ms there's no reason to spend the leftover SPI
  // bandwidth on frames past 60.
  const uint32_t spent = millis() - frameStart;
  delay(spent < 16 ? 16 - spent : 1);
#endif
}
