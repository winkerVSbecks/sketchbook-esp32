// ============================================================================
// terminal charts — a woven terminal of box-drawing chart glyphs
// ============================================================================
//   pio run -e charts_native -t exec      SDL window
//   pio run -e charts_esp32  -t upload    the board
//   pio run -e charts_shot                headless PNG capture, then run
//                                         .pio/build/charts_shot/program DIR SEED FRAMES
//
// Ported from sketchbook-ssam/src/sketches/terminal-charts/compositions.ts.
//
// The piece is a character grid, nothing else. A binary space partition cuts
// the screen into panels; each panel draws horizontal "weft" series scrolling
// left to right and vertical "warp" series running top to bottom, and where a
// warp crosses a weft it goes over or under by index parity — which is what
// makes it read as woven cloth rather than as overlapping charts.
//
// Wefts come in four styles, so the same data reads four ways: box-drawing
// line, block-density band, plain ASCII (=, +, |), and an OpenSSH-randomart
// visit-count ramp. Warps are always a single rule.
//
// Everything else is font work — see shared/termfont.h.
//
// Keep in sync with the Arduino IDE copy:
//   cp src/terminal_charts.cpp ~/Documents/Arduino/terminal_charts/sketch.h
//   cp -R src/shared ~/Documents/Arduino/terminal_charts/
// ============================================================================

#define SKETCH_TITLE  "terminal charts"
#define SKETCH_FRAMES 8

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/termfont.h"

#include <string.h>

// ---------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------
// The browser runs 14px Menlo on a 1080px square: charW ~8.4, LINE_H 20, so
// 128 columns by 54 rows. On 172x320 a 4x6 cell gives 43 x 53 — the same row
// count, a third of the columns. That is the trade this port turns on: a wider
// cell would draw prettier glyphs but leave too few columns for a panel to
// hold a chart, and splitRects would stop finding anywhere to split.
static const int COLS    = W / TERM_CELL_W;                    // 43
static const int ROWS    = H / TERM_CELL_H;                    // 53
static const int GRID_X0 = (W - COLS * TERM_CELL_W) / 2;       // 0
static const int GRID_Y0 = (H - ROWS * TERM_CELL_H) / 2;       // 1

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// These are the JS `config` object, retuned for 43 columns. The row-shaped
// numbers carry over unchanged (53 rows vs 54), the column-shaped ones do not:
// minRectCols 18 against a 39-column root would make canSplitV permanently
// false, splitRects would return the root untouched, and the whole piece would
// be one panel. See the port notes at the bottom of this file.
static const int   MIN_RECT_ROWS  = 9;      // JS 9   — rows scale 1:1
static const int   MIN_RECT_COLS  = 12;     // JS 18  — see above
static const float SPLIT_CHANCE   = 0.78f;
static const int   MAX_DEPTH      = 6;
static const int   GUTTER         = 1;
static const float WEFT_DENSITY   = 0.42f;
static const float WARP_DENSITY   = 0.28f;
static const float DENSITY_MIX    = 0.35f;
static const float ASCII_MIX      = 0.30f;
static const float RANDOMART_MIX  = 0.25f;
static const int   WARP_AMPLITUDE = 2;
static const int   MARGIN_ROWS    = 2;      // JS 3 rows of 54 -> 2 of 53
static const int   MARGIN_COLS    = 2;      // JS 4 cols of 128 -> 2 of 43
static const int   PLOT_ROWS      = 5;

// Samples per visible plot, and the length of the cyclic buffer they scroll
// through. The JS uses 64 in a plot up to 118 columns wide — a bit under two
// columns per sample. Here plots run 8-37 columns, so 64 samples would stack
// three or four to a column and the line would collapse into a scribble; 24
// keeps roughly the same samples-per-column ratio.
static const int N        = 24;
static const int BUFFER_N = 64;

// The 8s loop the JS `duration` sets. One full pass of the buffer per loop, so
// the scroll is seamless; the composition is rebuilt on the wrap.
static const uint32_t LOOP_MS = 8000;

#if defined(SKETCH_HEADLESS)
  // Spread captures over half the loop. At 24fps eight consecutive frames span
  // 330ms, which is two buffer steps — the PNGs would be near-identical and
  // tell you nothing about whether the scroll works.
  static const uint32_t FRAME_MS = LOOP_MS / 16;
#else
  static const uint32_t FRAME_MS = 1000 / 24;
#endif

// The JS uses palette[0] as the background and the rest as ink, with no
// contrast test at all. On a 1080px canvas an ink two steps from the
// background is a soft grey; at 4x6 it is mud. This is the one filter the port
// adds — a WCAG ratio, which is the correct metric for ink against paper.
static const float MIN_INK_CONTRAST = 2.2f;

// Sized from the geometry, not guessed. A 39x49 root with 12x9 minimums tiles
// into at most 3 panels across and 5 down; the worst case for series count is
// three undivided full-height columns, 19 wefts each.
static const int MAX_RECTS = 16;
static const int MAX_WEFTS = 64;
static const int MAX_WARPS = 64;
static const int MAX_INK   = 10;

enum WeftKind : uint8_t { K_LINE = 0, K_DENSITY, K_ASCII, K_RANDOMART };

// ---------------------------------------------------------------------------
// State — all of it fixed-size and rebuilt in place. The JS allocates Grid and
// weftIdxGrid fresh on every frame; here they are cleared with two memsets.
// ---------------------------------------------------------------------------
struct Rect   { int16_t row, col, rows, cols; };
struct Bounds { int16_t rowMin, rowMax, colMin, colMax; };

struct Pattern {
  Rect    rect;
  Bounds  bounds;
  int16_t plotStartCol, plotEndCol, plotStartRow, innerRows;
  int16_t weftStart, weftCount;
  int16_t warpStart, warpCount;
  int16_t warpAmplitude;
  uint8_t borderColor;
};

static uint32_t bg;
static uint32_t ink[MAX_INK];
static uint16_t inkRaw[MAX_INK];              // pre-swapped, ready for termBlit
static int      nInk;

static Rect    rects[MAX_RECTS];
static Pattern patterns[MAX_RECTS];
static int     nRects;

static float   weftBuf[MAX_WEFTS][BUFFER_N];
static uint8_t weftKind[MAX_WEFTS];
static uint8_t weftColor[MAX_WEFTS];
static int16_t weftBaseRow[MAX_WEFTS];
static float   weftAxisMax[MAX_WEFTS];
static int     nWefts;

static float   warpBuf[MAX_WARPS][BUFFER_N];
static int16_t warpCol[MAX_WARPS];
static uint8_t warpColor[MAX_WARPS];
static int     nWarps;

static uint8_t gChar[ROWS][COLS];
static uint8_t gColor[ROWS][COLS];
static int8_t  gWeft[ROWS][COLS];

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static void buildPalette() {
  Palette p = randomPalette();
  bg   = p.c[0];
  nInk = 0;

  const int cap = p.n - 1 < MAX_INK ? p.n - 1 : MAX_INK;
  for (int i = 1; i <= cap; i++) {
    if (wcagContrast(p.c[i], bg) >= MIN_INK_CONTRAST) ink[nInk++] = p.c[i];
  }

  // Two fallbacks. The JS has none: a one-entry palette leaves PALETTE equal
  // to [BG] and every glyph is painted in the background colour, and an empty
  // one paints `undefined`. Neither is survivable on a panel.
  if (nInk < 2) {
    nInk = 0;
    float best1 = -1.0f, best2 = -1.0f;
    int   i1 = -1, i2 = -1;
    for (int i = 1; i <= cap; i++) {
      const float c = wcagContrast(p.c[i], bg);
      if (c > best1)      { best2 = best1; i2 = i1; best1 = c; i1 = i; }
      else if (c > best2) { best2 = c;     i2 = i; }
    }
    if (i1 >= 0) ink[nInk++] = p.c[i1];
    if (i2 >= 0) ink[nInk++] = p.c[i2];
  }
  if (nInk == 0) ink[nInk++] = relLuminance(bg) > 0.3f ? 0x000000 : 0xFFFFFF;

  for (int i = 0; i < nInk; i++) inkRaw[i] = termRaw(to565(ink[i]));
}

// ---------------------------------------------------------------------------
// Series generators — direct ports, floats throughout
// ---------------------------------------------------------------------------
static void genWave(float *out, int n, float amplitude, int periods) {
  const int p = periods < 1 ? 1 : periods;
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (float)n;
    out[i] = amplitude * (0.5f + 0.4f * sinf(t * 2.0f * (float)M_PI * (float)p))
                       * rngRange(0.92f, 1.08f);
  }
}

static void genTrending(float *out, int n, float start, float end) {
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (float)n;
    const float s = 0.5f - 0.5f * cosf(t * 2.0f * (float)M_PI);
    const float v = (start + (end - start) * s) * rngRange(0.85f, 1.15f);
    out[i] = v > 0.0f ? v : 0.0f;
  }
}

static void genFlat(float *out, int n, float val) {
  for (int i = 0; i < n; i++) {
    const float v = val * rngRange(0.8f, 1.2f);
    out[i] = v > 0.0f ? v : 0.0f;
  }
}

static void genWarpWave(float *out, int n, float phase, int periods) {
  const int p = periods < 1 ? 1 : periods;
  for (int i = 0; i < n; i++) {
    const float t        = (float)i / (float)n;
    const float primary  = sinf(t * 2.0f * (float)M_PI * (float)p + phase);
    const float harmonic = 0.25f * sinf(t * 4.0f * (float)M_PI * (float)p + phase * 1.3f);
    out[i] = 0.5f + 0.45f * (primary + harmonic);
  }
}

// Each JS argument list evaluates left to right and every one of these
// consumes RNG, so the locals are not tidiness — C++ leaves argument
// evaluation order unspecified and inlining them would desync the stream.
static void pickWeftBuffer(float *out, int n) {
  const int kind = rngRangeFloor(0, 4);
  if (kind == 0) {
    const int v = rngRangeFloor(3, 9);
    genFlat(out, n, (float)v);
  } else if (kind == 1) {
    const int a = rngRangeFloor(5, 10);
    const int p = rngRangeFloor(1, 3);
    genWave(out, n, (float)a, p);
  } else if (kind == 2) {
    const int s = rngRangeFloor(1, 5);
    const int e = rngRangeFloor(5, 10);
    genTrending(out, n, (float)s, (float)e);
  } else {
    const int a = rngRangeFloor(6, 10);
    const int p = rngRangeFloor(1, 4);
    genWave(out, n, (float)a, p);
  }
}

// extendCyclic(), without the copy. The JS concatenates the buffer with its
// own head so a slice past the end still reads; indexing modulo BUFFER_N is
// the same thing for every offset the playhead can produce. See the port notes
// for how narrow that guarantee actually is in the JS.
static inline float bufAt(const float *b, int offset, int i) {
  const int k = offset + i;
  return b[k >= BUFFER_N ? k - BUFFER_N : k];
}

// ---------------------------------------------------------------------------
// Grid writes
// ---------------------------------------------------------------------------
static inline bool inBounds(int r, int c, const Bounds &b) {
  return r >= b.rowMin && r <= b.rowMax && c >= b.colMin && c <= b.colMax;
}

// The weft `mark()`: claims the cell for a weft index, so warps can decide
// whether to cross over or under it.
static inline void mark(int r, int c, uint8_t g, uint8_t col, int weftIdx, const Bounds &b) {
  if (!inBounds(r, c, b)) return;
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  gChar[r][c]  = g;
  gColor[r][c] = col;
  gWeft[r][c]  = (int8_t)weftIdx;
}

// The warp `put()`: writes the cell without claiming it.
static inline void put(int r, int c, uint8_t g, uint8_t col, const Bounds &b) {
  if (!inBounds(r, c, b)) return;
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  gChar[r][c]  = g;
  gColor[r][c] = col;
}

// ---------------------------------------------------------------------------
// Weft series — four styles over the same sample-to-cell mapping
// ---------------------------------------------------------------------------
static int sCols[N], sRows[N];
static int sPlotW;

static bool prepSeries(const float *buf, int offset, int plotStartCol, int plotEndCol,
                       float axisMax, int baseRow) {
  sPlotW = plotEndCol - plotStartCol;
  if (sPlotW <= 1 || N < 2 || axisMax <= 0.0f) return false;
  for (int i = 0; i < N; i++) {
    sCols[i] = (int)lroundf((float)(i * (sPlotW - 1)) / (float)(N - 1));
    float v  = bufAt(buf, offset, i);
    if (v > axisMax) v = axisMax;
    sRows[i] = baseRow + (int)lroundf((1.0f - v / axisMax) * (float)PLOT_ROWS);
  }
  return true;
}

static void drawWeftSeries(const float *buf, int offset, uint8_t col, int baseRow,
                           int plotStartCol, int plotEndCol, float axisMax,
                           int weftIdx, const Bounds &b) {
  if (!prepSeries(buf, offset, plotStartCol, plotEndCol, axisMax, baseRow)) return;

  for (int i = 0; i < N; i++) {
    const int r    = sRows[i];
    const int cEnd = i < N - 1 ? sCols[i + 1] : sPlotW;
    for (int c = sCols[i]; c < cEnd; c++) mark(r, plotStartCol + c, G_HLINE, col, weftIdx, b);
  }

  for (int i = 1; i < N; i++) {
    const int absC = plotStartCol + sCols[i];
    const int r1 = sRows[i - 1], r2 = sRows[i];
    if (r1 == r2) continue;
    const int top = r1 < r2 ? r1 : r2;
    const int bot = r1 < r2 ? r2 : r1;

    if (r2 < r1) {
      mark(r1, absC, G_ARC_BR, col, weftIdx, b);
      mark(r2, absC, G_ARC_TL, col, weftIdx, b);
    } else {
      mark(r1, absC, G_ARC_TR, col, weftIdx, b);
      mark(r2, absC, G_ARC_BL, col, weftIdx, b);
    }
    for (int r = top + 1; r < bot; r++) mark(r, absC, G_VLINE, col, weftIdx, b);
  }
}

static void drawDensityWeftSeries(const float *buf, int offset, uint8_t col, int baseRow,
                                  int plotStartCol, int plotEndCol, float axisMax,
                                  int weftIdx, const Bounds &b) {
  if (!prepSeries(buf, offset, plotStartCol, plotEndCol, axisMax, baseRow)) return;

  static const uint8_t GRADIENT[5] = { G_LIGHT, G_MEDIUM, G_FULL, G_MEDIUM, G_LIGHT };

  for (int i = 0; i < N; i++) {
    const int r    = sRows[i];
    const int cEnd = i < N - 1 ? sCols[i + 1] : sPlotW;
    for (int c = sCols[i]; c < cEnd; c++) {
      const int absC = plotStartCol + c;
      for (int d = -2; d <= 2; d++) mark(r + d, absC, GRADIENT[2 + d], col, weftIdx, b);
    }
  }

  for (int i = 1; i < N; i++) {
    const int absC = plotStartCol + sCols[i];
    const int r1 = sRows[i - 1], r2 = sRows[i];
    if (r1 == r2) continue;
    const int top = r1 < r2 ? r1 : r2;
    const int bot = r1 < r2 ? r2 : r1;
    for (int r = top; r <= bot; r++) mark(r, absC, G_FULL, col, weftIdx, b);
    mark(top - 1, absC, G_DARK, col, weftIdx, b);
    mark(bot + 1, absC, G_DARK, col, weftIdx, b);
  }
}

static void drawAsciiWeftSeries(const float *buf, int offset, uint8_t col, int baseRow,
                                int plotStartCol, int plotEndCol, float axisMax,
                                int weftIdx, const Bounds &b) {
  if (!prepSeries(buf, offset, plotStartCol, plotEndCol, axisMax, baseRow)) return;

  for (int i = 0; i < N; i++) {
    const int r    = sRows[i];
    const int cEnd = i < N - 1 ? sCols[i + 1] : sPlotW;
    for (int c = sCols[i]; c < cEnd; c++) mark(r, plotStartCol + c, G_EQUALS, col, weftIdx, b);
  }

  for (int i = 1; i < N; i++) {
    const int absC = plotStartCol + sCols[i];
    const int r1 = sRows[i - 1], r2 = sRows[i];
    if (r1 == r2) continue;
    const int top = r1 < r2 ? r1 : r2;
    const int bot = r1 < r2 ? r2 : r1;
    mark(r1, absC, G_PLUS, col, weftIdx, b);
    mark(r2, absC, G_PLUS, col, weftIdx, b);
    for (int r = top + 1; r < bot; r++) mark(r, absC, G_PIPE, col, weftIdx, b);
  }
}

// The randomart weft rasterises the series into a visit-count field and reads
// the OpenSSH ramp off the counts, so a busy region of the plot fills in with
// the heavy glyphs. Its rowAt() deliberately omits baseRow — the counts live
// in a local PLOT_ROWS+1 grid and baseRow is added at the mark.
static uint8_t visits[PLOT_ROWS + 1][COLS];

static void drawRandomartWeftSeries(const float *buf, int offset, uint8_t col, int baseRow,
                                    int plotStartCol, int plotEndCol, float axisMax,
                                    int weftIdx, const Bounds &b) {
  if (!prepSeries(buf, offset, plotStartCol, plotEndCol, axisMax, 0)) return;
  const int nrows = PLOT_ROWS + 1;

  for (int r = 0; r < nrows; r++) memset(visits[r], 0, (size_t)sPlotW);

  if (sRows[0] >= 0 && sRows[0] < nrows && sCols[0] >= 0 && sCols[0] < sPlotW) {
    visits[sRows[0]][sCols[0]]++;
  }
  for (int i = 0; i < N - 1; i++) {
    const int c0 = sCols[i],     r0 = sRows[i];
    const int dc = sCols[i + 1] - c0, dr = sRows[i + 1] - r0;
    int steps = (dc < 0 ? -dc : dc) > (dr < 0 ? -dr : dr) ? (dc < 0 ? -dc : dc) : (dr < 0 ? -dr : dr);
    if (steps < 1) steps = 1;
    for (int s = 1; s <= steps; s++) {
      const float t = (float)s / (float)steps;
      const int   c = (int)lroundf((float)c0 + (float)dc * t);
      const int   r = (int)lroundf((float)r0 + (float)dr * t);
      if (r >= 0 && r < nrows && c >= 0 && c < sPlotW && visits[r][c] < 255) visits[r][c]++;
    }
  }

  for (int r = 0; r < nrows; r++) {
    for (int c = 0; c < sPlotW; c++) {
      const int v = visits[r][c];
      if (v == 0) continue;
      mark(baseRow + r, plotStartCol + c, TERM_RAMP[v > 14 ? 14 : v], col, weftIdx, b);
    }
  }

  mark(baseRow + sRows[0],     plotStartCol + sCols[0],     G_UPS, col, weftIdx, b);
  mark(baseRow + sRows[N - 1], plotStartCol + sCols[N - 1], G_UPE, col, weftIdx, b);
}

// ---------------------------------------------------------------------------
// Warp series — the vertical half of the weave
// ---------------------------------------------------------------------------
static int sWarpCols[BUFFER_N];

static void drawWarpSeries(const float *buf, int offset, uint8_t col, int baseCol,
                           int amplitude, int warpIdx, int rowStart, int hRows,
                           const Bounds &b) {
  if (hRows < 2) return;
  for (int i = 0; i < hRows; i++) {
    sWarpCols[i] = baseCol + (int)lroundf((bufAt(buf, offset, i) - 0.5f) * 2.0f * (float)amplitude);
  }

  // Over-under. A cell already claimed by weft `wi` is crossed only when
  // (wi + warpIdx) is odd — that single parity test is the whole weave.
  auto canDraw = [&](int r, int c) -> bool {
    if (!inBounds(r, c, b)) return false;
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return false;
    const int wi = gWeft[r][c];
    if (wi == -1) return true;
    return ((wi + warpIdx) & 1) != 0;
  };
  auto tryPut = [&](int r, int c, uint8_t g) {
    if (canDraw(r, c)) put(r, c, g, col, b);
  };

  for (int i = 0; i < hRows; i++) {
    if (i > 0 && sWarpCols[i] != sWarpCols[i - 1]) continue;   // a step, not a run
    tryPut(rowStart + i, sWarpCols[i], G_VLINE);
  }

  for (int i = 1; i < hRows; i++) {
    const int r  = rowStart + i;
    const int c1 = sWarpCols[i - 1], c2 = sWarpCols[i];
    if (c1 == c2) continue;
    const int left  = c1 < c2 ? c1 : c2;
    const int right = c1 < c2 ? c2 : c1;

    if (c2 > c1) {
      tryPut(r, c1, G_ARC_BL);
      tryPut(r, c2, G_ARC_TR);
    } else {
      tryPut(r, c2, G_ARC_TL);
      tryPut(r, c1, G_ARC_BR);
    }
    for (int c = left + 1; c < right; c++) tryPut(r, c, G_HLINE);
  }
}

static void drawRectBorder(const Rect &rect, uint8_t col) {
  const int r0 = rect.row, r1 = rect.row + rect.rows - 1;
  const int c0 = rect.col, c1 = rect.col + rect.cols - 1;
  if (r1 <= r0 || c1 <= c0) return;

  auto setCell = [&](int r, int c, uint8_t g) {
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    gChar[r][c] = g; gColor[r][c] = col;
  };

  for (int c = c0 + 1; c < c1; c++) { setCell(r0, c, G_HLINE); setCell(r1, c, G_HLINE); }
  for (int r = r0 + 1; r < r1; r++) { setCell(r, c0, G_VLINE); setCell(r, c1, G_VLINE); }
  setCell(r0, c0, G_BOX_TL);
  setCell(r0, c1, G_BOX_TR);
  setCell(r1, c0, G_BOX_BL);
  setCell(r1, c1, G_BOX_BR);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
static void splitRects(const Rect &b, int depth) {
  if (nRects >= MAX_RECTS) return;                 // geometry can't reach this

  const bool canH = b.rows >= MIN_RECT_ROWS * 2;
  const bool canV = b.cols >= MIN_RECT_COLS * 2;
  if ((!canH && !canV) || depth >= MAX_DEPTH || (depth > 0 && !rngChance(SPLIT_CHANCE))) {
    rects[nRects++] = b;
    return;
  }

  // cols/2 because a character cell is about twice as tall as it is wide, so
  // this is the aspect ratio you see, not the one you count.
  const float aspect = (float)b.cols / 2.0f / (float)b.rows;
  bool splitH;
  if (canH && !canV)      splitH = true;
  else if (!canH && canV) splitH = false;
  else                    splitH = rngChance(aspect > 1.0f ? 0.3f : 0.7f);

  if (splitH) {
    const int at = rngRangeFloor(MIN_RECT_ROWS, b.rows - MIN_RECT_ROWS + 1);
    const Rect top = { b.row, b.col, (int16_t)at, b.cols };
    const Rect bot = { (int16_t)(b.row + at), b.col, (int16_t)(b.rows - at), b.cols };
    splitRects(top, depth + 1);
    splitRects(bot, depth + 1);
    return;
  }

  const int at = rngRangeFloor(MIN_RECT_COLS, b.cols - MIN_RECT_COLS + 1);
  const Rect left  = { b.row, b.col, b.rows, (int16_t)at };
  const Rect right = { b.row, (int16_t)(b.col + at), b.rows, (int16_t)(b.cols - at) };
  splitRects(left, depth + 1);
  splitRects(right, depth + 1);
}

static void buildPattern(const Rect &rect, Pattern &pat) {
  pat.rect         = rect;
  pat.plotStartCol = rect.col + GUTTER;
  pat.plotEndCol   = rect.col + rect.cols - GUTTER;
  pat.plotStartRow = rect.row + GUTTER;
  pat.innerRows    = rect.rows - GUTTER * 2;
  if (pat.innerRows < 1) pat.innerRows = 1;
  int innerCols = pat.plotEndCol - pat.plotStartCol;
  if (innerCols < 1) innerCols = 1;

  pat.bounds = { pat.plotStartRow, (int16_t)(pat.plotStartRow + pat.innerRows - 1),
                 pat.plotStartCol, (int16_t)(pat.plotEndCol - 1) };

  int weftCount = (int)((float)pat.innerRows * WEFT_DENSITY);
  const int weftCap = pat.innerRows - 1 > 1 ? pat.innerRows - 1 : 1;
  if (weftCount > weftCap) weftCount = weftCap;
  if (weftCount < 1)       weftCount = 1;
  if (weftCount > MAX_WEFTS - nWefts) weftCount = MAX_WEFTS - nWefts;

  int warpCount = (int)((float)innerCols * WARP_DENSITY);
  const int warpCap = innerCols / 2 > 1 ? innerCols / 2 : 1;
  if (warpCount > warpCap) warpCount = warpCap;
  if (warpCount < 1)       warpCount = 1;

  // warpAmpRoom is what actually decides the amplitude, and with warpDensity
  // at 0.28 there are roughly 3.6 columns per warp, so floor(3.6/2) - 1 = 0
  // almost everywhere. See the port notes. Derived before the capacity clamp
  // below so the geometry, not the array bound, sets it — and so warpCount is
  // still guaranteed non-zero at the division.
  const int warpAmpRoom = innerCols / warpCount / 2 - 1 > 0 ? innerCols / warpCount / 2 - 1 : 0;
  pat.warpAmplitude = (int16_t)(WARP_AMPLITUDE < warpAmpRoom ? WARP_AMPLITUDE : warpAmpRoom);

  // Defensive only: the worst layout the 39x49 root can tile into needs 57
  // wefts and 50 warps, both inside the arrays.
  if (warpCount > MAX_WARPS - nWarps) warpCount = MAX_WARPS - nWarps;

  const float localDensityMix   = rngRange(fmaxf(0.0f, DENSITY_MIX   - 0.2f),
                                           fminf(1.0f, DENSITY_MIX   + 0.2f));
  const float localAsciiMix     = rngRange(fmaxf(0.0f, ASCII_MIX     - 0.2f),
                                           fminf(1.0f, ASCII_MIX     + 0.2f));
  const float localRandomartMix = rngRange(fmaxf(0.0f, RANDOMART_MIX - 0.2f),
                                           fminf(1.0f, RANDOMART_MIX + 0.2f));

  pat.weftStart = (int16_t)nWefts;
  pat.weftCount = (int16_t)weftCount;
  for (int i = 0; i < weftCount; i++) {
    float *buf = weftBuf[nWefts + i];
    pickWeftBuffer(buf, BUFFER_N);

    const float roll       = rngNext();
    const float tAscii     = localAsciiMix;
    const float tDensity   = tAscii + localDensityMix;
    const float tRandomart = tDensity + localRandomartMix;
    weftKind[nWefts + i] = roll < tAscii     ? K_ASCII
                         : roll < tDensity   ? K_DENSITY
                         : roll < tRandomart ? K_RANDOMART
                                             : K_LINE;

    float mx = 1.0f;
    for (int k = 0; k < BUFFER_N; k++) if (buf[k] > mx) mx = buf[k];
    weftAxisMax[nWefts + i] = mx;
  }

  const int availRows = pat.innerRows - PLOT_ROWS - 1 > 0 ? pat.innerRows - PLOT_ROWS - 1 : 0;
  for (int i = 0; i < weftCount; i++) {
    const int offsetRow = weftCount == 1 ? availRows / 2 : (i * availRows) / (weftCount - 1);
    weftBaseRow[nWefts + i] = (int16_t)(pat.plotStartRow + offsetRow);
  }

  uint8_t rectPalette[MAX_INK];
  for (int i = 0; i < nInk; i++) rectPalette[i] = (uint8_t)i;
  rngShuffle(rectPalette, nInk);
  const int want = rngRangeFloor(2, 4);
  const int nRectPal = nInk < want ? nInk : want;

  for (int i = 0; i < weftCount; i++) weftColor[nWefts + i] = rectPalette[i % nRectPal];
  nWefts += weftCount;

  pat.warpStart = (int16_t)nWarps;
  pat.warpCount = (int16_t)warpCount;
  for (int i = 0; i < warpCount; i++) {
    const float phase   = ((float)i / (float)warpCount) * 2.0f * (float)M_PI
                        + rngRange(0.0f, (float)M_PI);
    const int   periods = rngRangeFloor(1, 4);
    genWarpWave(warpBuf[nWarps + i], BUFFER_N, phase, periods);
    warpCol[nWarps + i]   = (int16_t)(pat.plotStartCol
                                      + (int)lroundf(((float)i + 0.5f) * (float)innerCols / (float)warpCount));
    warpColor[nWarps + i] = rectPalette[(i + 1) % nRectPal];
  }
  nWarps += warpCount;

  pat.borderColor = rectPalette[0];
}

static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  buildPalette();
  Serial.printf("bg #%06lX / %d ink:", (unsigned long)bg, nInk);
  for (int i = 0; i < nInk; i++) Serial.printf(" #%06lX", (unsigned long)ink[i]);
  Serial.println();

  nRects = 0; nWefts = 0; nWarps = 0;
  const Rect root = { MARGIN_ROWS, MARGIN_COLS,
                      (int16_t)(ROWS - MARGIN_ROWS * 2), (int16_t)(COLS - MARGIN_COLS * 2) };
  splitRects(root, 0);
  for (int i = 0; i < nRects; i++) buildPattern(rects[i], patterns[i]);

  Serial.printf("%d panels, %d wefts, %d warps\n", nRects, nWefts, nWarps);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
static void buildFrame(int offset) {
  memset(gChar, 0, sizeof(gChar));
  memset(gWeft, -1, sizeof(gWeft));      // 0xFF bytes == -1 for int8_t

  for (int p = 0; p < nRects; p++) {
    const Pattern &pat = patterns[p];

    for (int i = 0; i < pat.weftCount; i++) {
      const int w = pat.weftStart + i;
      switch (weftKind[w]) {
        case K_DENSITY:   drawDensityWeftSeries  (weftBuf[w], offset, weftColor[w], weftBaseRow[w],
                                                  pat.plotStartCol, pat.plotEndCol,
                                                  weftAxisMax[w], w, pat.bounds); break;
        case K_ASCII:     drawAsciiWeftSeries    (weftBuf[w], offset, weftColor[w], weftBaseRow[w],
                                                  pat.plotStartCol, pat.plotEndCol,
                                                  weftAxisMax[w], w, pat.bounds); break;
        case K_RANDOMART: drawRandomartWeftSeries(weftBuf[w], offset, weftColor[w], weftBaseRow[w],
                                                  pat.plotStartCol, pat.plotEndCol,
                                                  weftAxisMax[w], w, pat.bounds); break;
        default:          drawWeftSeries         (weftBuf[w], offset, weftColor[w], weftBaseRow[w],
                                                  pat.plotStartCol, pat.plotEndCol,
                                                  weftAxisMax[w], w, pat.bounds); break;
      }
    }

    for (int i = 0; i < pat.warpCount; i++) {
      const int k = pat.warpStart + i;
      drawWarpSeries(warpBuf[k], offset, warpColor[k], warpCol[k], pat.warpAmplitude,
                     i, pat.plotStartRow, pat.innerRows, pat.bounds);
    }

    drawRectBorder(pat.rect, pat.borderColor);
  }
}

static void blitGrid() {
  cv.fillScreen(to565(bg));
  uint16_t *fb = (uint16_t *)cv.getBuffer();
  for (int r = 0; r < ROWS; r++) {
    const int py = GRID_Y0 + r * TERM_CELL_H;
    for (int c = 0; c < COLS; c++) {
      const uint8_t g = gChar[r][c];
      if (!g) continue;
      termBlit(fb, W, GRID_X0 + c * TERM_CELL_W, py, g, inkRaw[gColor[r][c]]);
    }
  }
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
  static uint32_t curLoop = 0;
  static uint32_t accBuild = 0, accBlit = 0, accPush = 0;
  static int      nFrames  = 0;

  const uint32_t now = millis();
  const uint32_t idx = now / LOOP_MS;
  if (idx != curLoop) {                    // the JS holds one composition for
    curLoop = idx;                         // ever; on a panel, 8s is plenty
    generate(esp_random());
  }
  const int offset = (int)(((now % LOOP_MS) * (uint32_t)BUFFER_N) / LOOP_MS);

  // wallMicros(), not millis(): the headless build's millis() is a virtual
  // clock that only moves when the sketch calls delay(), which is what makes
  // the playhead above step deterministically — and what makes it useless for
  // measuring anything.
  const uint32_t t0 = wallMicros();
  buildFrame(offset);
  const uint32_t t1 = wallMicros();
  blitGrid();
  const uint32_t t2 = wallMicros();
  present();
  const uint32_t t3 = wallMicros();

  accBuild += t1 - t0; accBlit += t2 - t1; accPush += t3 - t2;
  if (++nFrames == 24) {
    Serial.printf("build %luus  blit %luus  push %luus  frame %luus\n",
                  (unsigned long)(accBuild / 24), (unsigned long)(accBlit / 24),
                  (unsigned long)(accPush / 24),
                  (unsigned long)((accBuild + accBlit + accPush) / 24));
    accBuild = accBlit = accPush = 0;
    nFrames  = 0;
  }

  const uint32_t spent = (t3 - t0) / 1000;
  delay(spent >= FRAME_MS ? 1 : FRAME_MS - spent);
}

// ============================================================================
// Port notes — what translating this revealed about the original
// ============================================================================
// * `config.warpAmplitude` is a 0..5 slider that can never exceed 1, and is 0
//   for every panel of any size. buildPattern derives
//   `warpAmpRoom = floor(innerCols / warpCount / 2) - 1` from
//   `warpCount = floor(innerCols * 0.28)`, which pins innerCols/warpCount near
//   3.57, so the expression is floor(1.78) - 1 = 0. Enumerated over every
//   reachable width, amplitude is nonzero only at innerCols 16, 17, 20, 21, 24,
//   28 and 32 — the handful where the two floors round favourably — and is 0
//   for every innerCols >= 33. Reaching 2 would need innerCols/warpCount >= 6,
//   which happens only at innerCols 6 or 7, far below either minRectCols. So in
//   the browser, where minRectCols 18 puts most panels well above 33 columns,
//   nearly every warp is a dead straight rule, `hasTransition` is false, and
//   the second loop of drawWarpSeries — the ╭╮╰╯ jogs — never runs; genWarpWave
//   computes a two-harmonic wave per warp that is then rounded away. Applies to
//   the JS. Ported as-is, and on this 43-column grid innerCols runs 10..37, so
//   amplitude 1 is common and the jogs do appear.
//
// * `extendCyclic` has exactly one element of slack on the warp path, and the
//   `% BUFFER_N` is not what saves it. The warp buffers are
//   `extendCyclic(genWarpWave(BUFFER_N, ...), innerRows)` — length
//   BUFFER_N + min(innerRows, BUFFER_N), because `arr.slice(0, extra)` caps at
//   the source length — and the frame slices `[offset, offset + innerRows)`.
//   While innerRows <= BUFFER_N the length grows with the slice and it fits for
//   any offset up to BUFFER_N inclusive, so even a playhead of exactly 1.0
//   lands safely; the modulo is redundant at the current geometry. What does
//   break it is innerRows > BUFFER_N, where the length pins at 2 * BUFFER_N and
//   the slice runs short for every offset past 2 * BUFFER_N - innerRows. At
//   LINE_H 20 that needs a canvas over ~1700px tall — a 4K export clears it —
//   and at innerRows 100 the worst offset loses 19 of the 100 rows.
//   Downstream, drawWarpSeries takes `H = values.length` and only guards
//   `H < 2`, so it draws H rows from rowStart instead of innerRows: the warp
//   stops short of the panel floor, and since H varies with offset the stopping
//   point moves every frame. Warps would retract and extend as the loop plays
//   and the bottom band of each panel would lose both its rules and its
//   over-under interleave — with no throw and no warning, because
//   `Array.prototype.slice` clamps silently. Applies to the JS. The weft path
//   has the same shape but asks 64 out of a doubled 160-long buffer, largest
//   slice end 79 + 64 = 143, so it has 17 elements spare and cannot run short.
//
// * The four weft styles do not get a quarter each. asciiMix 0.30 + densityMix
//   0.35 + randomartMix 0.25 = 0.90, and 'line' is the else branch — so the
//   plain box-drawing chart, the one style that reads unambiguously as a line
//   chart, is the rarest at 10%. Worse, each mix is jittered independently by
//   +/-0.2 per rect, so their sum lands anywhere in [0.30, 1.50]; once it
//   reaches 1.0 the roll can never fall through and that rect cannot produce a
//   line weft at all, which is roughly a third of them. Ported as written.
//
// * `ramp[0]`, the space in `" .o+=*BOX@%&#/^"`, is unreachable — cells with
//   zero visits are skipped before the lookup, so the lowest index ever
//   produced is 1. The table is kept the same length here for the same reason
//   it is there: the visit count indexes it directly.
//
// * `drawWeftSeries` uses `W` for the last segment's end column
//   (`cEnd = i < N - 1 ? cols[i + 1] : W`) where every other branch uses a
//   `cols[]` value. It happens to be right — `cols[N-1]` is `W - 1`, so the run
//   is the single last cell — but it reads like an off-by-one and is the only
//   place plot width leaks into the loop body.
//
// * `Random.shuffle(palette.slice()).slice(0, Math.min(palette.length,
//   Random.rangeFloor(2, 4)))` shuffles the whole palette and then takes 2 or 3
//   — so `weftColors[i % rectPalette.length]` cycles a 2- or 3-colour subset,
//   and `warpColors` uses the same subset rotated by one. Worth naming: it is
//   why panels read as colour families rather than as a uniform palette.
//
// * The JS never checks ink against background. `randomPalette()` returns sets
//   whose first two entries are often one step apart (the sequential ramps in
//   mindful-palettes especially), and at 14px on 1080px that reads as a tint.
//   At 4x6 it reads as nothing, so this port filters ink by WCAG ratio against
//   the background — the one metric that is genuinely a legibility question —
//   and keeps the two highest-contrast entries if the filter would leave fewer
//   than two.
//
// Divergences beyond the palette filter, all forced by the 43x53 grid:
//   minRectCols 18 -> 12   at 18 the root (39 cols) can never satisfy
//                          `cols >= minRectCols * 2`, canSplitV is always
//                          false, and every composition is a single column of
//                          panels. 12 restores the 2-3 columns the browser gets.
//   marginCols  4 -> 2     4 of 128 columns is 3% of the width; 4 of 43 is 9%.
//   marginRows  3 -> 2     same argument, less severe.
//   N          64 -> 24    64 samples in a 10-column panel puts three or four
//                          samples in every column, and the line degenerates
//                          into the vertical connectors with no horizontal run
//                          left to read as a trend.
//   BUFFER_N   80 -> 64    holds the 125ms-per-step scroll rate of the original
//                          (8000/64 here, 8000/80 there) at the smaller N.
// ============================================================================
