// ============================================================================
// arc-tile grid — new composition every 8s
// ============================================================================
//   pio run -e native   -t exec      SDL window
//   pio run -e esp32    -t upload    the board
//   pio run -e arc_shot -t exec      headless PNG capture
//
// Keep in sync with the Arduino IDE copy:
//   cp src/main.cpp ~/Documents/Arduino/arc_tiles_147b/sketch.h
//   cp -R src/shared ~/Documents/Arduino/arc_tiles_147b/
// ============================================================================

#define SKETCH_TITLE "arc tiles 172x320"

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const int      RES_X   = 3;
static const int      RES_Y   = 6;
static const int      NCELLS  = RES_X * RES_Y;
static const uint32_t HOLD_MS = 8000;

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
static void drawCell(const GridCell &c, int x0, int x1, int y0, int y1) {
  const int w = x1 - x0, h = y1 - y0;
  if (w <= 0 || h <= 0) return;

  if (c.type == T_FULL) {
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
      const float dx = (px + 0.5f) - cx;
      const float nx = dx / rx;
      const float q  = sqrtf(nx * nx + ny2);

      float cover;
      if (q < 0.0001f) {
        cover = 1.0f;
      } else {
        const float gradLen = sqrtf(nx * nx * invRx2 + ny2 * invRy2) / q;
        const float dist    = (q - 1.0f) / gradLen;
        cover = 0.5f - dist;
        if (cover <= 0.0f) continue;
        if (cover >  1.0f) cover = 1.0f;
      }
      cv.drawPixel(px, py, cover >= 1.0f ? to565(c.color) : blend565(bg, c.color, cover));
    }
  }
}

static void render() {
  cv.fillScreen(to565(bg));

  int colEdge[RES_X + 1], rowEdge[RES_Y + 1];
  for (int i = 0; i <= RES_X; i++) colEdge[i] = (int)lroundf(i * (float)W / RES_X);
  for (int j = 0; j <= RES_Y; j++) rowEdge[j] = (int)lroundf(j * (float)H / RES_Y);

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
  static uint32_t last = 0;
  if (millis() - last >= HOLD_MS) {
    last = millis();
    generate(esp_random());
  }
  delay(20);
}
