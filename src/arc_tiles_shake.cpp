// ============================================================================
// arc-tile grid, shake to regenerate
// ============================================================================
//   pio run -e shake_native -t exec      SDL window (click it to "shake")
//   pio run -e shake_esp32  -t upload    the board
//   pio run -e shake_shot                headless PNG capture
//
// Keep in sync with the Arduino IDE copy:
//   cp src/arc_tiles_shake.cpp ~/Documents/Arduino/arc_tiles_shake/sketch.h
//   cp -R src/shared ~/Documents/Arduino/arc_tiles_shake/
// ============================================================================

#define SKETCH_TITLE "arc tiles 172x320"

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/imu.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const int      RES_X   = 3;
static const int      RES_Y   = 6;
static const int      NCELLS  = RES_X * RES_Y;
static const uint32_t HOLD_MS = 8000;   // only used if the IMU is missing

// Inset the grid by this many pixels on every side. The margin needs no
// drawing of its own — fillScreen already lays down bg, and the grid is
// simply laid out inside the smaller rect.
static const int      MARGIN  = 8;

// The panel's glass has rounded corners, so the composition is clipped to a
// rounded rect that runs parallel to them. SCREEN_RADIUS is the display's own
// corner radius in pixels — 26 is calibrated by eye on the board, not derived.
// Deriving it from the spec (~2mm corner at this panel's ~0.1mm pitch) gives
// 20, which reads visibly square against the bezel; don't "correct" it back.
// The artwork sits MARGIN inside the glass, so its radius is concentric —
// outer radius minus the inset — and that subtraction is what keeps the two
// curves parallel rather than merely both round. Set SCREEN_RADIUS to 0 for
// square corners.
static const float    SCREEN_RADIUS = 26.0f;
static const float    CLIP_R  = SCREEN_RADIUS > MARGIN ? SCREEN_RADIUS - MARGIN : 0.0f;

// The clip rect is exactly the grid rect, and MARGIN is symmetric, so its
// centre is the screen centre.
static const float    CLIP_CX = W * 0.5f;
static const float    CLIP_CY = H * 0.5f;
static const float    CLIP_HW = (W - 2 * MARGIN) * 0.5f;
static const float    CLIP_HH = (H - 2 * MARGIN) * 0.5f;

// Shake gesture. At rest the accelerometer reads ~1.0g (gravity); a deliberate
// shake spikes well past 2g. Lower SHAKE_G for a hair trigger, raise it if the
// piece regenerates when you just pick the board up. The cooldown stops one
// wrist-flick — which is several peaks — from firing a dozen times.
static const float    SHAKE_G           = 2.9f;
static const uint32_t SHAKE_COOLDOWN_MS = 600;

// How many foreground colours a composition may use. 1 = a two-tone piece:
// one background, one ink. Raise it (up to 16) to go back to multi-colour —
// MIN_DELTA_E below only does any work when this is > 1.
static const int      MAX_COLORS = 1;

// Colour separation thresholds.
//   BG_CONTRAST  — WCAG ratio vs black; decides which entries can be background
//   FG_CONTRAST  — WCAG ratio vs the chosen background (legibility: correct use)
//   MIN_DELTA_E  — oklab distance between two foreground colours (see below)
static const float BG_CONTRAST = 4.0f;
static const float FG_CONTRAST = 3.0f;
static const float MIN_DELTA_E = 0.20f;   // tune me: 0.10 permissive, 0.35 strict

// ---------------------------------------------------------------------------
// Cell types — each "-arc" cell is a quarter disc centred on one corner
// ---------------------------------------------------------------------------
enum CellType : uint8_t { T_FULL = 0, T_TL = 1, T_TR = 2, T_BL = 3, T_BR = 4 };

// mirrors[type][edge] as a 5-bit mask. Edges: 0=top 1=right 2=bottom 3=left.
static const uint8_t MIRRORS[5][4] = {
  /* 0123    */ { 0x19, 0x0B, 0x07, 0x15 },
  /* 013-arc */ { 0x09, 0x00, 0x00, 0x05 },
  /* 012-arc */ { 0x11, 0x03, 0x00, 0x00 },
  /* 023-arc */ { 0x00, 0x00, 0x03, 0x11 },
  /* 123-arc */ { 0x00, 0x09, 0x05, 0x00 },
};

struct GridCell {
  uint8_t  x, y;
  bool     occupied;
  uint32_t color;
  CellType type;
  int16_t  areaId;
};

static GridCell grid[NCELLS];
static uint32_t bg;
static uint32_t colors[MAX_COLORS];
static int      nColors;

static inline int xyToIndex(int x, int y) { return y * RES_X + x; }

// ---------------------------------------------------------------------------
// Palette selection
// ---------------------------------------------------------------------------
static void buildPalette() {
  uint32_t base[9];
  const uint32_t *src = rngNext() < 0.5f ? CARMEN : BLESS;
  for (int i = 0; i < 9; i++) base[i] = src[i];
  rngShuffle(base, 9);

  uint32_t bgOpts[9]; int nbg = 0;
  bool isBgOpt[9] = { false };
  for (int i = 0; i < 9; i++) {
    if (wcagContrast(base[i], 0x000000) >= BG_CONTRAST) {
      bgOpts[nbg++] = base[i];
      isBgOpt[i] = true;
    }
  }
  if (nbg == 0) bgOpts[nbg++] = 0xFFFFFF;

  rngShuffle(bgOpts, nbg);
  bg = bgOpts[nbg - 1];                        // shuffle().pop()

  // Foreground set. Two independent jobs, two different metrics:
  //   vs background — WCAG, because that IS a legibility question
  //   vs each other — oklab distance, because that's a "can I tell these
  //                   apart" question, and WCAG can't see hue at all.
  // The original used WCAG >= 4.0 for both, which is why bless always
  // collapsed to one colour: its three dark entries (#000000, #8F0202,
  // #042411) sit at ratios of ~1.7-2.2 to each other, so whichever the
  // shuffle put first knocked out the other two every time.
  nColors = 0;
  for (int i = 0; i < 9 && nColors < MAX_COLORS; i++) {
    if (isBgOpt[i]) continue;
    if (wcagContrast(base[i], bg) < FG_CONTRAST) continue;

    bool ok = true;
    for (int j = 0; j < nColors; j++) {
      if (deltaEok(base[i], colors[j]) < MIN_DELTA_E) { ok = false; break; }
    }
    if (ok) colors[nColors++] = base[i];
  }

  // Fallbacks — the JS has none and would paint `undefined`.
  if (nColors == 0) {
    for (int i = 0; i < 9 && nColors < MAX_COLORS; i++) {
      if (!isBgOpt[i] && wcagContrast(base[i], bg) >= FG_CONTRAST) colors[nColors++] = base[i];
    }
  }
  if (nColors == 0) colors[nColors++] = (relLuminance(bg) > 0.3f) ? 0x000000 : 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// Grid generation
// ---------------------------------------------------------------------------
static void resetGrid() {
  for (int y = 0; y < RES_Y; y++)
    for (int x = 0; x < RES_X; x++) {
      GridCell &c = grid[xyToIndex(x, y)];
      c.x = x; c.y = y; c.occupied = false; c.color = 0; c.type = T_FULL; c.areaId = -1;
    }
}

static const int8_t DIRS[4][2] = { {0,1}, {1,0}, {0,-1}, {-1,0} };  // down right up left

static void createArea(int areaId) {
  int freeCells[NCELLS], nfree = 0;
  for (int i = 0; i < NCELLS; i++) if (!grid[i].occupied) freeCells[nfree++] = i;
  if (nfree == 0) return;

  uint32_t  color   = colors[rngIndex(nColors)];
  GridCell *current = &grid[freeCells[rngIndex(nfree)]];
  current->occupied = true;
  current->color    = color;
  current->areaId   = areaId;

  int count = 1;
  while (true) {
    int opts[4], nopts = 0;
    for (int d = 0; d < 4; d++) {
      int nx = current->x + DIRS[d][0];
      int ny = current->y + DIRS[d][1];
      if (nx < 0 || nx >= RES_X || ny < 0 || ny >= RES_Y) continue;
      if (grid[xyToIndex(nx, ny)].occupied) continue;
      opts[nopts++] = d;
    }
    if (nopts == 0) break;
    if (count > 10)  break;

    int d  = opts[rngIndex(nopts)];
    int dx = DIRS[d][0], dy = DIRS[d][1];
    GridCell *next = &grid[xyToIndex(current->x + dx, current->y + dy)];
    next->occupied = true;
    next->color    = color;
    next->areaId   = areaId;

    int edge = (dx ==  1) ? 1 : (dx == -1) ? 3 : (dy == 1) ? 2 : 0;
    uint8_t mask = MIRRORS[current->type][edge];
    if (mask == 0) break;

    CellType allowed[5]; int nallowed = 0;
    for (int t = 0; t < 5; t++) if (mask & (1u << t)) allowed[nallowed++] = (CellType)t;

    next->type = allowed[rngIndex(nallowed)];
    current    = next;
    count++;
  }
}

static void fillGridWithAreas() {
  int attempts = 0;
  while (attempts < 100) {
    int unoccupied = 0;
    for (int i = 0; i < NCELLS; i++) if (!grid[i].occupied) unoccupied++;
    if (unoccupied == 0) break;
    createArea(attempts);
    attempts++;
  }
  Serial.printf("Filled grid with areas in %d attempts\n", attempts);
}

static void reduce() {
  for (int i = 0; i < NCELLS; i++) {
    GridCell &cell = grid[i];

    uint32_t nb[4]; int nnb = 0;
    for (int d = 0; d < 4; d++) {
      int nx = cell.x + DIRS[d][0], ny = cell.y + DIRS[d][1];
      if (nx < 0 || nx >= RES_X || ny < 0 || ny >= RES_Y) continue;
      nb[nnb++] = grid[xyToIndex(nx, ny)].color;
    }

    bool shares = false;
    for (int k = 0; k < nnb; k++) if (nb[k] == cell.color) { shares = true; break; }
    if (shares || nnb == 0) continue;

    uint32_t best = cell.color; int bestCount = 0;
    for (int k = 0; k < nnb; k++) {
      int c = 0;
      for (int m = 0; m < nnb; m++) if (nb[m] == nb[k]) c++;
      if (c > bestCount) { bestCount = c; best = nb[k]; }
    }
    cell.color = best;
  }
}

// ---------------------------------------------------------------------------
// Rendering — analytic coverage AA
// ---------------------------------------------------------------------------

// Coverage of the rounded-rect clip at a pixel centre: 1 well inside, 0 well
// outside, fractional across the one-pixel band on the curve. Signed distance
// to a rounded rect, then the same `0.5 - dist` coverage estimate the arcs use,
// so the clipped edge is anti-aliased to match them.
static inline float clipCover(int px, int py) {
  const float qx = fabsf((px + 0.5f) - CLIP_CX) - (CLIP_HW - CLIP_R);
  const float qy = fabsf((py + 0.5f) - CLIP_CY) - (CLIP_HH - CLIP_R);
  const float mx = qx > 0.0f ? qx : 0.0f;
  const float my = qy > 0.0f ? qy : 0.0f;
  const float inner = qx > qy ? qx : qy;
  const float dist  = sqrtf(mx * mx + my * my) + (inner < 0.0f ? inner : 0.0f) - CLIP_R;
  const float cover = 0.5f - dist;
  return cover <= 0.0f ? 0.0f : (cover >= 1.0f ? 1.0f : cover);
}

// A rounded rect only differs from a plain one inside its four corner boxes, so
// every cell outside them keeps the cheap unclipped path. At 3x6 that's the
// four corner cells; the rest still get fillRect or the bare arc loop.
static inline bool touchesClipCorner(int x0, int x1, int y0, int y1) {
  if (CLIP_R <= 0.0f) return false;
  const bool nearX = (x0 < MARGIN + CLIP_R)     || (x1 > (W - MARGIN) - CLIP_R);
  const bool nearY = (y0 < MARGIN + CLIP_R)     || (y1 > (H - MARGIN) - CLIP_R);
  return nearX && nearY;
}

static void drawCell(const GridCell &c, int x0, int x1, int y0, int y1) {
  const int w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return;

  const bool arc     = (c.type != T_FULL);
  const bool clipped = touchesClipCorner(x0, x1, y0, y1);

  if (!arc && !clipped) {
    cv.fillRect(x0, y0, w, h, to565(c.color));
    return;
  }

  const float rx = (float)w, ry = (float)h;
  const float cx = (c.type == T_TL || c.type == T_BL) ? (float)x0 : (float)x1;
  const float cy = (c.type == T_TL || c.type == T_TR) ? (float)y0 : (float)y1;
  const float invRx2 = 1.0f / (rx * rx), invRy2 = 1.0f / (ry * ry);

  for (int py = y0; py < y1; py++) {
    const float dy = (py + 0.5f) - cy;
    const float ny = dy / ry, ny2 = ny * ny;

    for (int px = x0; px < x1; px++) {
      float cover = 1.0f;

      if (arc) {
        const float dx = (px + 0.5f) - cx;
        const float nx = dx / rx;
        const float q  = sqrtf(nx * nx + ny2);

        if (q >= 0.0001f) {
          const float gradLen = sqrtf(nx * nx * invRx2 + ny2 * invRy2) / q;
          const float dist    = (q - 1.0f) / gradLen;
          cover = 0.5f - dist;
          if (cover <= 0.0f) continue;
          if (cover >  1.0f) cover = 1.0f;
        }
      }

      // Multiplying coverages rather than masking keeps an arc that runs
      // diagonally through a corner anti-aliased on both edges at once.
      if (clipped) {
        cover *= clipCover(px, py);
        if (cover <= 0.0f) continue;
      }

      cv.drawPixel(px, py, cover >= 1.0f ? to565(c.color) : blend565(bg, c.color, cover));
    }
  }
}

static void render() {
  cv.fillScreen(to565(bg));

  const int innerW = W - 2 * MARGIN;
  const int innerH = H - 2 * MARGIN;

  int colEdge[RES_X + 1], rowEdge[RES_Y + 1];
  for (int i = 0; i <= RES_X; i++) colEdge[i] = MARGIN + (int)lroundf(i * (float)innerW / RES_X);
  for (int j = 0; j <= RES_Y; j++) rowEdge[j] = MARGIN + (int)lroundf(j * (float)innerH / RES_Y);

  for (int i = 0; i < NCELLS; i++) {
    const GridCell &c = grid[i];
    drawCell(c, colEdge[c.x], colEdge[c.x + 1], rowEdge[c.y], rowEdge[c.y + 1]);
  }

  present();
}

static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  buildPalette();
  Serial.printf("bg #%06lX / %d colors:", (unsigned long)bg, nColors);
  for (int i = 0; i < nColors; i++) Serial.printf(" #%06lX", (unsigned long)colors[i]);
  Serial.println();

  resetGrid();
  fillGridWithAreas();
  reduce();

  uint32_t t0 = millis();
  render();
  Serial.printf("rendered in %lums\n", (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------------------
static bool imuOk = false;

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  imuOk = imuBegin();

  generate(esp_random());
}

void loop() {
  static uint32_t lastFire = 0;
  const uint32_t now = millis();

  if (now - lastFire >= SHAKE_COOLDOWN_MS && shakeDetected(SHAKE_G)) {
    lastFire = now;
    generate(esp_random());
  }

  // With no IMU there's no way to ask for a new composition, so keep the old
  // timer as a safety net rather than freezing on a single frame forever.
  if (!imuOk && now - lastFire >= HOLD_MS) {
    lastFire = now;
    generate(esp_random());
  }

  delay(5);
}
