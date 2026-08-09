// ============================================================================
// jali truchet — an arched jali window, its screen woven from truchet tiles
// ============================================================================
//   pio run -e jali_native -t exec      SDL window: drag = tilt, bottom-strip
//                                       click = new composition
//   pio run -e jali_esp32  -t upload    the board: tilt = parallax, shake or
//                                       BOOT = new composition
//   pio run -e jali_shot   -t exec      headless PNG capture (tilt drifts on
//                                       its own, so the frames show parallax)
//
// Port of sketchbook-ssam/src/sketches/jali/truchet.ts. The original draws an
// arch-shaped window on a square 1080 canvas: the arch interior filled and
// used as a clip, a truchet lattice of four polyline tile types stroked
// inside it, a stroked frame down the left and along the bottom, and a
// filled-and-stroked extrusion on the right that gives the arch 3D depth.
// Here the composition's fractional geometry is stretched onto 172x320 —
// the arch fills the portrait panel — while the lattice cells stay square,
// as they were on the square canvas.
//
// Every bezierCurveTo in the original passes the same control point twice, so
// each "cubic" is really P(t) = (1-t)^3 A + 3t(1-t) C + t^3 B: three simple
// arcs, sampled into per-scanline boundary tables. Everything else reduces to
// scanline fills and round-capped polyline strokes, rendered with analytic
// coverage (LovyanGFX's own arcs alias).
//
// The port's one addition is parallax: the lattice lives on its own plane,
// rendered once into an oversized PSRAM buffer and re-blitted every frame at
// an offset driven by the accelerometer, so tilting the board slides the
// screen behind the arch. The arch fill, frame and extrusion never redraw —
// the per-row blit spans are inset past the frame strokes' ink, so a frame is
// just row memcpys plus one dirty-rect push (~48K px, ~9ms of SPI at 40MHz).
// The lattice keeps a ~2px standoff from the frame stroke because of that
// inset; at this scale it reads as the stroke's own breathing room.
// ============================================================================

#define SKETCH_TITLE  "jali"
#define SKETCH_FRAMES 10

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/imu.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// The original's config: resolution 32, scale 1 (a no-op transform), offset 50
// on a 1080 canvas. Offset is the extrusion depth; the whole composition also
// shifts left by offset/2 so the extruded arch reads centred.
static const float OFFSET_F = 50.0f / 1080.0f;      // extrusion depth, of width

// Strokes. The original uses lineWidth 4 on 1080px (~0.37% of the canvas),
// which lands under one pixel here; these are the smallest widths that still
// read on the panel. The frame is heavier than the lattice on purpose — it is
// the structural outline the lattice sits behind.
static const float FRAME_STROKE = 2.0f;
static const float LAT_STROKE   = 1.5f;

// Lattice grid. The original's 32 columns would be 5.4px cells here — mush
// under any stroke. 12px square cells keep the stroke:cell ratio near the
// original's (4:33.75) and leave the tiles' quarter-grid steps at 3px.
static const int CELL = 12;

// Parallax. TILT_FULL_G is the tilt that pins the lattice to full travel —
// sin(27 deg) of gravity. The ease is a time-constant low-pass so the lattice
// settles rather than jitters on accelerometer noise.
static const int   PARALLAX_MAX  = 10;      // px of lattice travel each way
static const float TILT_FULL_G   = 0.45f;
static const float TILT_TAU_MS   = 120.0f;
// -1 reads as a plane behind the window: tilt an edge up and the lattice
// slides toward it, the way a distant object crosses a window frame. Flip if
// it feels inverted in the hand.
static const float PARALLAX_SIGN = -1.0f;

// Shake gesture, same tuning story as arc_tiles_shake: at rest the
// accelerometer reads ~1g, a deliberate shake spikes past 2g, and the
// cooldown keeps one wrist-flick from firing a dozen times.
static const float    SHAKE_G           = 2.9f;
static const uint32_t SHAKE_COOLDOWN_MS = 600;

static const int LAT_PAD       = 2;     // stroke halo past the arch bbox
static const int CURVE_SAMPLES = 16;    // segments per arc when stroking it

// ---------------------------------------------------------------------------
// The four tile types
// ---------------------------------------------------------------------------
// Each is a 7-point polyline on the cell's quarter grid, corner to opposite
// corner: two rising (bottom-left to top-right) and two falling, each in a
// staircase and a mirrored-staircase variant. Endpoints land on cell corners,
// which is what makes neighbouring tiles weave into one continuous screen.
static const uint8_t TILE[4][7][2] = {
  { {0,4}, {1,3}, {1,2}, {2,2}, {3,2}, {3,1}, {4,0} },
  { {0,0}, {1,1}, {2,1}, {2,2}, {2,3}, {3,3}, {4,4} },
  { {0,4}, {1,3}, {2,3}, {2,2}, {2,1}, {3,1}, {4,0} },
  { {0,0}, {1,1}, {1,2}, {2,2}, {3,2}, {3,3}, {4,4} },
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uint32_t colBg, colFrame, colJali, colSide, colOutside;

// Geometry, resolved to pixels in setup(). Names follow the original's
// coordinates: x1/x2 the arch's sides, x3 its centreline, y3 the apex, y1
// where the curves spring from the straight sides, y2 the base.
static float OFFX;
static float X_L, X_R, X_C, X_LIP, X_SIDE;
static float Y_APEX, Y_SPRING, Y_BASE, CPY;

// Per-scanline boundaries of the arch interior and the extrusion's outer
// edge, plus the lattice blit spans (inset past the frame strokes' ink).
static float   archL[H], archR[H], sideR[H];
static int16_t spanL[H], spanR[H];
static int     archTop, archBot;              // arch interior rows
static int     blitTop, blitBot;              // rows with a non-empty span
static int     dirtyX0, dirtyX1;              // span bbox, for the push

// The lattice plane: swapped RGB565, PARALLAX_MAX + LAT_PAD bigger than the
// arch bbox on every side, in PSRAM. ~62KB.
static uint16_t *lat = nullptr;
static int LAT_X0, LAT_Y0, LAT_W, LAT_H, LAT_COLS, LAT_ROWS;

static bool  imuOk = false;
static float tiltX = 0.0f, tiltY = 0.0f;      // low-passed gravity, in g
static int   curOx = 0, curOy = 0;            // last blitted offset

// ---------------------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------------------
// A 16bpp LGFX_Sprite stores byte-swapped RGB565 with stride == width (see
// termfont.h), and the lattice buffer keeps the same layout — that is what
// lets the per-frame blit be a straight memcpy between the two.
static inline uint16_t rawOf(uint16_t c565)  { return (uint16_t)((c565 >> 8) | (c565 << 8)); }
static inline uint16_t unraw(uint16_t raw)   { return (uint16_t)((raw >> 8) | (raw << 8)); }

static inline void blendPx(uint16_t *fb, int stride, int x, int y,
                           uint32_t ink, float cover) {
  uint16_t *p = fb + (size_t)y * (size_t)stride + (size_t)x;
  if (cover >= 1.0f) { *p = rawOf(to565(ink)); return; }
  *p = rawOf(blend565(from565(unraw(*p)), ink, cover));
}

// ---------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------
// The degenerate cubic all three arcs share: from A to B, pulled toward C.
struct Curve { float ax, ay, cx, cy, bx, by; };

static inline void curveAt(const Curve &c, float t, float &x, float &y) {
  const float u  = 1.0f - t;
  const float w0 = u * u * u, w1 = 3.0f * t * u, w2 = t * t * t;
  x = w0 * c.ax + w1 * c.cx + w2 * c.bx;
  y = w0 * c.ay + w1 * c.cy + w2 * c.by;
}

// Resolve a curve into per-row x by marching t and interpolating at each row
// centre it crosses. y(t) is monotonic for these three arcs (their control
// point sits between the endpoints' y), so each row is crossed once.
static void curveRows(const Curve &c, float *out) {
  const int NS = 512;
  float px, py;
  curveAt(c, 0.0f, px, py);
  for (int i = 1; i <= NS; i++) {
    float x, y;
    curveAt(c, (float)i / (float)NS, x, y);
    const float lo = fminf(py, y), hi = fmaxf(py, y);
    int r0 = (int)ceilf(lo - 0.5f), r1 = (int)floorf(hi - 0.5f);
    if (r0 < 0) r0 = 0;
    if (r1 > H - 1) r1 = H - 1;
    for (int r = r0; r <= r1; r++) {
      const float yc = (float)r + 0.5f;
      if (yc < lo || yc > hi) continue;
      const float dy = y - py;
      const float t  = fabsf(dy) > 1e-6f ? (yc - py) / dy : 0.0f;
      out[r] = px + (x - px) * clampf(t, 0.0f, 1.0f);
    }
    px = x;
    py = y;
  }
}

// ---------------------------------------------------------------------------
// Polyline stroke
// ---------------------------------------------------------------------------
// A round-capped, round-joined stroke as one coverage pass: per pixel, the
// distance to the nearest segment of the chain. Caps and joins fall out of the
// min() for free, and because each pixel is written once, overlapping
// segments of the same stroke can't double-darken their AA edges.
static const int MAX_SEGS = 64;

struct Seg { float x0, y0, x1, y1, xmin, xmax, ymin, ymax; };

static inline float segDist(const Seg &s, float px, float py) {
  const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0.0f ? ((px - s.x0) * dx + (py - s.y0) * dy) / l2 : 0.0f;
  t = clampf(t, 0.0f, 1.0f);
  const float ex = s.x0 + t * dx - px, ey = s.y0 + t * dy - py;
  return sqrtf(ex * ex + ey * ey);
}

static void strokePolyline(uint16_t *fb, int stride, int fbW, int fbH,
                           const float (*pts)[2], int n, float halfW, uint32_t ink) {
  static Seg segs[MAX_SEGS];
  const int ns = n - 1;
  const float pad = halfW + 1.0f;

  float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
  for (int i = 0; i < ns; i++) {
    Seg &s = segs[i];
    s.x0 = pts[i][0];     s.y0 = pts[i][1];
    s.x1 = pts[i + 1][0]; s.y1 = pts[i + 1][1];
    s.xmin = fminf(s.x0, s.x1); s.xmax = fmaxf(s.x0, s.x1);
    s.ymin = fminf(s.y0, s.y1); s.ymax = fmaxf(s.y0, s.y1);
    bx0 = fminf(bx0, s.xmin); bx1 = fmaxf(bx1, s.xmax);
    by0 = fminf(by0, s.ymin); by1 = fmaxf(by1, s.ymax);
  }

  int ry0 = (int)floorf(by0 - pad), ry1 = (int)ceilf(by1 + pad);
  if (ry0 < 0) ry0 = 0;
  if (ry1 > fbH - 1) ry1 = fbH - 1;

  for (int py = ry0; py <= ry1; py++) {
    const float yc = (float)py + 0.5f;

    // Segments whose ink can reach this row, and the x range they cover.
    int   act[MAX_SEGS], na = 0;
    float rx0 = 1e9f, rx1 = -1e9f;
    for (int i = 0; i < ns; i++) {
      if (yc < segs[i].ymin - pad || yc > segs[i].ymax + pad) continue;
      act[na++] = i;
      rx0 = fminf(rx0, segs[i].xmin - pad);
      rx1 = fmaxf(rx1, segs[i].xmax + pad);
    }
    if (!na) continue;

    int px0 = (int)floorf(rx0), px1 = (int)ceilf(rx1);
    if (px0 < 0) px0 = 0;
    if (px1 > fbW - 1) px1 = fbW - 1;

    for (int px = px0; px <= px1; px++) {
      const float xc = (float)px + 0.5f;
      float d = 1e9f;
      for (int k = 0; k < na; k++) d = fminf(d, segDist(segs[act[k]], xc, yc));
      const float cover = halfW - d + 0.5f;
      if (cover <= 0.0f) continue;
      blendPx(fb, stride, px, py, ink, cover > 1.0f ? 1.0f : cover);
    }
  }
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static uint32_t hslToRgb(float h, float s, float l) {
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;
  const float c  = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
  const float hp = h / 60.0f;
  const float x  = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if      (hp < 1.0f) { r = c; g = x; }
  else if (hp < 2.0f) { r = x; g = c; }
  else if (hp < 3.0f) { g = c; b = x; }
  else if (hp < 4.0f) { g = x; b = c; }
  else if (hp < 5.0f) { r = x; b = c; }
  else                { r = c; b = x; }
  const float m = l - c * 0.5f;
  return rgbPack(r + m, g + m, b + m);
}

// rampensau's generateColorRamp({total: 6}), then .reverse(), as the original
// calls it: hue sweeps a full cycle from a random start (easing x^2),
// saturation 0.40 -> 0.35 (x^2), lightness random-in-[0,0.1] -> 0.9 (x^1.5),
// all in HSL. Ported from rampensau's documented defaults; if the library's
// defaults have drifted since, this is a snapshot, not a live import.
static int rampColors(uint32_t *out) {
  const int   n      = 6;
  const float hStart = rngNext() * 360.0f;
  const float lLo    = rngNext() * 0.1f;
  for (int i = 0; i < n; i++) {
    const float relI = (float)i / (float)(n - 1);
    const float h    = 360.0f + hStart + (1.0f - relI * relI - 0.5f) * 360.0f;
    const float s    = 0.40f + (0.35f - 0.40f) * relI * relI;
    const float l    = lLo + (0.9f - lLo) * powf(relI, 1.5f);
    out[(n - 1) - i] = hslToRgb(h, s, l);
  }
  return n;
}

static void buildPalette() {
  uint32_t list[12];
  int n;
  if (rngChance()) {
    n = rampColors(list);
  } else {
    // Random.pick([...autoAlbers, ...mindfulPalettes, ...clrs]) — a flat pick
    // across the same three sets randomPalette() covers.
    const Palette p = randomPalette();
    n = p.n;
    for (int i = 0; i < n; i++) list[i] = p.c[i];
  }
  rngShuffle(list, n);

  colBg    = list[0];
  colFrame = list[1];
  colJali  = list[2];
  colSide  = list[3];
  // Four of the mindful palettes only hold four colours. The JS shift()s past
  // the end there and assigns undefined to fillStyle, which canvas ignores —
  // so the arch interior keeps the last fillStyle, the background. Port the
  // behaviour, not the intent.
  colOutside = n >= 5 ? list[4] : colBg;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static void geometryInit() {
  OFFX     = OFFSET_F * (float)W;
  X_L      = 0.25f * W - OFFX * 0.5f;
  X_R      = 0.75f * W - OFFX * 0.5f;
  X_C      = 0.50f * W - OFFX * 0.5f;
  Y_APEX   = 0.10f * H;
  Y_SPRING = 0.35f * H;
  Y_BASE   = 0.90f * H;
  CPY      = 0.8f * Y_SPRING;           // the y1*0.8 control every arc shares
  X_LIP    = X_C + 1.25f * OFFX;
  X_SIDE   = X_R + OFFX;

  for (int y = 0; y < H; y++) {
    archL[y] = X_L;
    archR[y] = X_R;
    sideR[y] = X_SIDE;
  }
  curveRows(Curve{ X_C, Y_APEX, X_L, CPY, X_L, Y_SPRING }, archL);
  curveRows(Curve{ X_R, Y_SPRING, X_R, CPY, X_C, Y_APEX }, archR);
  curveRows(Curve{ X_LIP, Y_APEX, X_SIDE, CPY, X_SIDE, Y_SPRING }, sideR);

  archTop = (int)ceilf(Y_APEX - 0.5f);
  archBot = (int)floorf(Y_BASE - 0.5f);

  // Blit spans: the arch interior inset past the frame strokes' ink (half
  // width plus AA), so the per-frame blit never repaints a stroke pixel and
  // the strokes never need redrawing. Near the apex the curves converge and
  // the spans close on their own.
  const float inset = FRAME_STROKE * 0.5f + 1.0f;
  const int   lastRow = (int)floorf(Y_BASE - inset - 0.5f);
  blitTop = -1;
  blitBot = -1;
  dirtyX0 = W;
  dirtyX1 = -1;
  for (int y = 0; y < H; y++) {
    int l = 1, r = 0;
    if (y >= archTop && y <= lastRow) {
      l = (int)ceilf(archL[y] + inset);
      r = (int)floorf(archR[y] - inset);
    }
    spanL[y] = (int16_t)l;
    spanR[y] = (int16_t)r;
    if (r < l) continue;
    if (blitTop < 0) blitTop = y;
    blitBot = y;
    if (l < dirtyX0) dirtyX0 = l;
    if (r > dirtyX1) dirtyX1 = r;
  }

  LAT_X0   = (int)floorf(X_L) - PARALLAX_MAX - LAT_PAD;
  LAT_Y0   = (int)floorf(Y_APEX) - PARALLAX_MAX - LAT_PAD;
  LAT_W    = ((int)ceilf(X_R) + PARALLAX_MAX + LAT_PAD) - LAT_X0;
  LAT_H    = ((int)ceilf(Y_BASE) + PARALLAX_MAX + LAT_PAD) - LAT_Y0;
  LAT_COLS = (LAT_W + CELL - 1) / CELL;
  LAT_ROWS = (LAT_H + CELL - 1) / CELL;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void renderLattice() {
  const uint16_t bgRaw = rawOf(to565(colOutside));
  for (int i = 0; i < LAT_W * LAT_H; i++) lat[i] = bgRaw;

  // Column-major like the original's x-outer/y-inner walk.
  const float q = (float)CELL * 0.25f;
  for (int cx = 0; cx < LAT_COLS; cx++) {
    for (int cy = 0; cy < LAT_ROWS; cy++) {
      const uint8_t (*tile)[2] = TILE[rngIndex(4)];
      float pts[7][2];
      for (int k = 0; k < 7; k++) {
        pts[k][0] = (float)(cx * CELL) + (float)tile[k][0] * q;
        pts[k][1] = (float)(cy * CELL) + (float)tile[k][1] * q;
      }
      strokePolyline(lat, LAT_W, LAT_W, LAT_H, pts, 7, LAT_STROKE * 0.5f, colJali);
    }
  }
}

static void blitLattice(int ox, int oy) {
  uint16_t *fb = (uint16_t *)cv.getBuffer();
  for (int y = blitTop; y <= blitBot; y++) {
    const int l = spanL[y], n = spanR[y] - l + 1;
    if (n <= 0) continue;
    memcpy(fb + (size_t)y * W + l,
           lat + (size_t)(y - LAT_Y0 - oy) * LAT_W + (l - LAT_X0 - ox),
           (size_t)n * 2);
  }
}

// Everything except the lattice, drawn once per composition in the original's
// order: background, arch interior, lattice, frame stroke, extrusion.
static void renderStatic(int ox, int oy) {
  uint16_t *fb = (uint16_t *)cv.getBuffer();

  cv.fillSprite(to565(colBg));

  // Arch interior ("Jali shell"). Plain spans — every edge of this region is
  // restroked below, so the strokes carry the anti-aliasing.
  const uint16_t outside565 = to565(colOutside);
  for (int y = archTop; y <= archBot; y++) {
    const int xs = (int)lroundf(archL[y]), xe = (int)lroundf(archR[y]);
    if (xe > xs) cv.drawFastHLine(xs, y, xe - xs, outside565);
  }

  blitLattice(ox, oy);

  // Frame: apex, down the left arc, then the left side and the base.
  {
    float pts[CURVE_SAMPLES + 3][2];
    int n = 0;
    const Curve left = { X_C, Y_APEX, X_L, CPY, X_L, Y_SPRING };
    for (int i = 0; i <= CURVE_SAMPLES; i++, n++)
      curveAt(left, (float)i / CURVE_SAMPLES, pts[n][0], pts[n][1]);
    pts[n][0] = X_L; pts[n][1] = Y_BASE; n++;
    pts[n][0] = X_R; pts[n][1] = Y_BASE; n++;
    strokePolyline(fb, W, W, H, pts, n, FRAME_STROKE * 0.5f, colFrame);
  }

  // Extrusion: filled between the arch's right boundary and its offset copy,
  // then the whole outline stroked, both in the side colour.
  const uint16_t side565 = to565(colSide);
  for (int y = archTop; y <= archBot; y++) {
    const int xs = (int)lroundf(archR[y]), xe = (int)lroundf(sideR[y]);
    if (xe > xs) cv.drawFastHLine(xs, y, xe - xs, side565);
  }
  {
    float pts[2 * CURVE_SAMPLES + 5][2];
    int n = 0;
    pts[n][0] = X_R; pts[n][1] = Y_BASE; n++;
    const Curve right = { X_R, Y_SPRING, X_R, CPY, X_C, Y_APEX };
    for (int i = 0; i <= CURVE_SAMPLES; i++, n++)
      curveAt(right, (float)i / CURVE_SAMPLES, pts[n][0], pts[n][1]);
    pts[n][0] = X_LIP; pts[n][1] = Y_APEX; n++;
    const Curve outer = { X_LIP, Y_APEX, X_SIDE, CPY, X_SIDE, Y_SPRING };
    for (int i = 1; i <= CURVE_SAMPLES; i++, n++)
      curveAt(outer, (float)i / CURVE_SAMPLES, pts[n][0], pts[n][1]);
    pts[n][0] = X_SIDE; pts[n][1] = Y_BASE; n++;
    pts[n][0] = X_R;    pts[n][1] = Y_BASE; n++;
    strokePolyline(fb, W, W, H, pts, n, FRAME_STROKE * 0.5f, colSide);
  }
}

// Push only the lattice's span bbox. Headless still captures whole frames —
// the sprite always holds the complete composition, so present() is correct
// there and the dirty rect is purely a transfer optimisation.
static void presentDirty() {
#if defined(SKETCH_HEADLESS)
  present();
#else
  lcd.startWrite();
  lcd.setClipRect(dirtyX0, blitTop, dirtyX1 - dirtyX0 + 1, blitBot - blitTop + 1);
  cv.pushSprite(&lcd, 0, 0);
  lcd.clearClipRect();
  lcd.endWrite();
#endif
}

// ---------------------------------------------------------------------------
static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  buildPalette();
  Serial.printf("palette: bg %06lX frame %06lX jali %06lX side %06lX outside %06lX\n",
                (unsigned long)colBg, (unsigned long)colFrame, (unsigned long)colJali,
                (unsigned long)colSide, (unsigned long)colOutside);

  uint32_t t0 = wallMicros();
  renderLattice();
  const uint32_t latUs = wallMicros() - t0;

  t0 = wallMicros();
  renderStatic(curOx, curOy);
  const uint32_t staticUs = wallMicros() - t0;

  Serial.printf("lattice %dx%d cells into %dx%d px: %lums | static scene: %lums\n",
                LAT_COLS, LAT_ROWS, LAT_W, LAT_H,
                (unsigned long)(latUs / 1000), (unsigned long)(staticUs / 1000));

  present();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(50);

  if (!panelBegin()) {
    Serial.println("sprite alloc failed");
    while (true) delay(1000);
  }
  buttonBegin();
  imuOk = imuBegin();

  geometryInit();
  lat = (uint16_t *)psramAlloc((size_t)LAT_W * LAT_H * 2);
  if (!lat) {
    Serial.println("lattice alloc failed");
    while (true) delay(1000);
  }
  Serial.printf("blit rows %d..%d, x %d..%d (%d px dirty -> ~%.1fms SPI @40MHz)\n",
                blitTop, blitBot, dirtyX0, dirtyX1,
                (dirtyX1 - dirtyX0 + 1) * (blitBot - blitTop + 1),
                (dirtyX1 - dirtyX0 + 1) * (blitBot - blitTop + 1) * 0.0004f);

  generate(newSeed());
}

void loop() {
  const uint32_t frameStart = wallMicros();
  const uint32_t now        = millis();

  // New composition: BOOT on the board, the bottom-strip click on SDL...
  static uint32_t lastFire = 0;
  bool regen = buttonPressed();
#if defined(ARDUINO)
  // ...and a shake. Hardware only: the SDL stand-in reads any click as a
  // shake, and this sketch's clicks are already spoken for by drag-to-tilt.
  if (!regen && imuOk && now - lastFire >= SHAKE_COOLDOWN_MS && shakeDetected(SHAKE_G))
    regen = true;
#endif
  if (regen) {
    lastFire = now;
    generate(newSeed());
  }

  // Tilt -> parallax. dt-aware low-pass, so the same time constant holds at
  // any frame rate (headless steps 800ms a frame; the ease must not lag it).
  static uint32_t lastMs = 0;
  const uint32_t dt = lastMs ? now - lastMs : 16;
  lastMs = now;
  const float alpha = 1.0f - expf(-(float)dt / TILT_TAU_MS);

  float gx, gy;
  imuTilt(gx, gy);
  tiltX += (gx - tiltX) * alpha;
  tiltY += (gy - tiltY) * alpha;

  const float k  = PARALLAX_SIGN * (float)PARALLAX_MAX / TILT_FULL_G;
  int ox = (int)lroundf(clampf(tiltX * k, (float)-PARALLAX_MAX, (float)PARALLAX_MAX));
  int oy = (int)lroundf(clampf(tiltY * k, (float)-PARALLAX_MAX, (float)PARALLAX_MAX));

  if (ox != curOx || oy != curOy) {
    curOx = ox;
    curOy = oy;

    const uint32_t b0 = wallMicros();
    blitLattice(ox, oy);
    const uint32_t blitUs = wallMicros() - b0;
    presentDirty();
    const uint32_t pushUs = wallMicros() - b0 - blitUs;

    static uint32_t accBlit = 0, accPush = 0, accN = 0;
    accBlit += blitUs;
    accPush += pushUs;
    if (++accN >= 300) {
      Serial.printf("frame: blit %luus + push %luus avg over %lu\n",
                    (unsigned long)(accBlit / accN), (unsigned long)(accPush / accN),
                    (unsigned long)accN);
      accBlit = accPush = accN = 0;
    }
  }

#if defined(SKETCH_HEADLESS)
  // Step a SKETCH_FRAMES-th of the tilt-drift circle per capture, so one run
  // sweeps the lattice through its full travel.
  delay(TILT_DRIFT_MS / SKETCH_FRAMES - 16);
#else
  // Pace to ~60fps; an unchanged offset costs nothing but this sleep.
  const uint32_t spentMs = (wallMicros() - frameStart) / 1000;
  if (spentMs < 15) delay(15 - spentMs);
#endif
}
