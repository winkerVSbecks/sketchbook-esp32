// ============================================================================
// the core — six nested isometric cubes: tap to pop, hold to squish, jelly
// ============================================================================
//   pio run -e core_a18_native -t exec     SDL window (click = tap, hold = squish)
//   pio run -e core_a18        -t upload   the board
//   pio run -e core_a18_shot               headless PNG capture, then run it
//
// Port of the-core.js (canvas-sketch, 1080x1080, 4s loop). Six nested
// hexagons read as an isometric cube stack; each hexagon's six vertices
// split into two groups of three that slide apart along the 60° diagonal,
// driven by a quintic bounce easing. Every cuboid has its own duration
// (0.35..0.6 of the loop), so they pop open in a stagger, overshoot, and
// settle. Each is filled with a two-stop linear gradient between two of its
// own vertices — a fixed sunset ramp on deep purple.
//
// The piece is fully deterministic: no randomness anywhere, so there is no
// reseed gesture and RESET changes nothing (the circle_moire situation).
// The glass is a jelly instead:
//
//   at rest   the settled-open pose — the original's end of loop, relaxed
//   tap       replay the pop: snap closed, stagger-bounce back open to rest
//             (2s here; the original's 4s spent its last 1.6s holding still,
//             which reads as lag on a toy)
//   hold      the stack shrinks diagonally shut under the finger — the pop
//             in reverse, along the piece's own shear axis, never a scale
//             or skew (squish starts once the press outlives the tap
//             window, so a tap doesn't flinch); a mid-pop hold freezes the
//             pop, release resumes it
//   release   a damped spring rings it back open, overshooting past the
//             rest pose — the inner cubes lag the outer ones a few frames,
//             which is what reads as jelly
//
// Nothing animates unless a finger asked for it, and every animation comes
// to rest; idle frames don't render or push.
//
// Rendering: LovyanGFX has no gradient-filled polygon, and the cuboids
// overlap during the stagger, so the renderer composites per pixel instead —
// analytic edge coverage against each convex hull (the porting-patterns
// recipe generalised from the quarter-ellipse), gradients lerped in RGB888,
// one to565Dither at the write. Two things keep the wobble's frame cost
// down: hexagons composite front to back so an opaque top layer ends the
// pixel after one evaluation (most pixels), and each scanline lands in one
// pushImage instead of a drawPixel per pixel. No framebuffer readback.
// ============================================================================

#define SKETCH_TITLE  "core"
#define SKETCH_FRAMES 12

#include "shared/platform.h"
#include "shared/color.h"
#include "shared/touch.h"

// ---------------------------------------------------------------------------
// Config — all geometry in the original's 1080-space, scaled at update time
// ---------------------------------------------------------------------------
static const int      N_CUBOIDS = 6;
static const float    SIZES[N_CUBOIDS]     = { 180.0f, 150.0f, 120.0f, 90.0f, 60.0f, 30.0f };
static const float    DURATIONS[N_CUBOIDS] = { 0.60f, 0.55f, 0.50f, 0.45f, 0.40f, 0.35f };
static const uint32_t COLORS[N_CUBOIDS][2] = {
  { 0xFC5F3D, 0xFC3563 },
  { 0xFD7569, 0xFD5D7E },
  { 0xFE8984, 0xFE7D8F },
  { 0xFEAA94, 0xFEA399 },
  { 0xFEC59D, 0xFEC2A0 },
  { 0xFEDBA2, 0xFDDAA4 },
};
static const uint32_t BG = 0x361D48;          // rgba(54, 29, 72)

static const float OFFSET      = 80.0f;
static const float DELTA_SCALE = 1.25f;       // getDelta(1.25)

// Width-proportional like the original square canvas, then grown 1.3x — the
// stack floats in a lot of purple at a straight W/1080, and the fully
// stretched, fully wobbling extremes still clear the panel edges at 1.3.
static const float SCALE = (float)W / 1080.0f * 1.3f;

// Gesture timing. A press that releases inside TAP_MS is a tap; only a press
// that outlives it starts squashing, so a tap plays the pop from a still
// stack instead of a flinched one.
static const uint32_t TAP_MS  = 200;
static const uint32_t PLAY_MS = 2000;   // the pop; the original loop was 4s

// The jelly, expressed as a shear-extension multiplier: 1 = the rest pose,
// 0 = shrunk fully shut. Held, it approaches SQUISH_MIN with time constant
// SQUISH_TAU (a full shut read as too far — the squeeze stops partway);
// released from e0 it rings back as
// 1 + (e0-1) * e^(-t/RING_TAU) * cos(2pi*RING_HZ*t), each layer starting
// RING_LAG later than the one outside it. The first swing overshoots past 1
// (the stack stretches beyond rest) — that extreme still clears the panel.
static const float SQUISH_MIN = 0.4f;
static const float SQUISH_TAU = 0.12f;
static const float RING_HZ    = 2.5f;
static const float RING_TAU   = 0.45f;
static const float RING_LAG   = 0.035f;
static const float RING_DONE  = 0.004f;  // envelope below this is at rest

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// startVertices, from createCuboid(): a hexagon around the origin, its top
// and bottom edges horizontal, nudged 0.25*off along the shear axis. Vertex
// groups for the shear: indices 1,2,3 move (+offX, -offY), indices 0,4,5
// move the opposite way — dsign holds that split.
static const float DSIGN[6] = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f };

struct Cuboid {
  float sx[6], sy[6];        // start vertices, 1080-space
  // per-frame state, panel-space:
  float vx[6], vy[6];        // sheared, squashed vertices
  float nx[6], ny[6], nc[6]; // inward edge normals, dist = nx*x + ny*y + nc
  float x0, y0, x1, y1;      // bounding box
  float gax, gay;            // gradient start (vertex 5)
  float gdx, gdy, ginv;      // gradient axis and 1/|axis|^2
};

static Cuboid cuboids[N_CUBOIDS];

// The gradient stops as float channels, unpacked once — the inner loop lerps
// these instead of shifting 0xRRGGBB apart per pixel.
static float grad0[N_CUBOIDS][3];   // color 0
static float gradD[N_CUBOIDS][3];   // color 1 - color 0

// The quintic bounce from the JS Utils: overshoots past 1 and settles.
static inline float bounce(float t) {
  const float ts = t * t, tc = ts * t;
  return 33.0f * tc * ts - 106.0f * ts * ts + 126.0f * tc - 67.0f * ts + 15.0f * t;
}

static void initCuboids() {
  const float offX = OFFSET * cosf((float)M_PI / 3.0f);
  const float offY = OFFSET * sinf((float)M_PI / 3.0f);

  for (int k = 0; k < N_CUBOIDS; k++) {
    Cuboid &c = cuboids[k];
    const float size = SIZES[k];
    const float cos3 = size * cosf((float)M_PI / 3.0f);
    const float sin3 = size * sinf((float)M_PI / 3.0f);

    c.sx[0] = -size + 0.25f * offX;  c.sy[0] = -0.25f * offY;
    c.sx[1] = -size + cos3 - 0.25f * offX;  c.sy[1] = 0.25f * offY - sin3;
    c.sx[2] =  cos3 - 0.25f * offX;  c.sy[2] = 0.25f * offY - sin3;
    c.sx[3] =  size - 0.25f * offX;  c.sy[3] = 0.25f * offY;
    c.sx[4] =  cos3 + 0.25f * offX;  c.sy[4] = -0.25f * offY + sin3;
    c.sx[5] = -size + cos3 + 0.25f * offX;  c.sy[5] = -0.25f * offY + sin3;

    for (int ch = 0; ch < 3; ch++) {
      const int shift = 16 - 8 * ch;
      const float a = (float)((COLORS[k][0] >> shift) & 0xFF);
      const float b = (float)((COLORS[k][1] >> shift) & 0xFF);
      grad0[k][ch] = a;
      gradD[k][ch] = b - a;
    }
  }
}

// updateCuboid(): while the playhead is inside a cuboid's duration the shear
// eases along the bounce; past it the cuboid holds at start + delta. The JS
// freezes whatever the last update wrote, which under monotonic playback is
// exactly this — written statelessly here so a frozen pop can resume. The
// jelly extension ek multiplies the shear itself, so the squish moves the
// vertices along the same diagonal the pop does — a shrink, never a skew.
static void updateCuboids(float ph, const float *ek) {
  const float offX = OFFSET * cosf((float)M_PI / 3.0f) * DELTA_SCALE;
  const float offY = OFFSET * sinf((float)M_PI / 3.0f) * DELTA_SCALE;
  const float cx = (float)W * 0.5f, cy = (float)H * 0.5f;

  for (int k = 0; k < N_CUBOIDS; k++) {
    Cuboid &c = cuboids[k];
    const float p = (ph < DURATIONS[k] ? bounce(ph / DURATIONS[k]) : 1.0f) * ek[k];

    c.x0 = 1e9f; c.y0 = 1e9f; c.x1 = -1e9f; c.y1 = -1e9f;
    for (int i = 0; i < 6; i++) {
      c.vx[i] = cx + (c.sx[i] + DSIGN[i] * offX * p) * SCALE;
      c.vy[i] = cy + (c.sy[i] - DSIGN[i] * offY * p) * SCALE;
      if (c.vx[i] < c.x0) c.x0 = c.vx[i];
      if (c.vx[i] > c.x1) c.x1 = c.vx[i];
      if (c.vy[i] < c.y0) c.y0 = c.vy[i];
      if (c.vy[i] > c.y1) c.y1 = c.vy[i];
    }

    // Inward-oriented unit normals, so coverage is a signed distance in
    // pixels. Orientation is settled against the centroid rather than
    // assumed from winding.
    float mx = 0.0f, my = 0.0f;
    for (int i = 0; i < 6; i++) { mx += c.vx[i]; my += c.vy[i]; }
    mx /= 6.0f; my /= 6.0f;
    for (int i = 0; i < 6; i++) {
      const int j = (i + 1) % 6;
      const float ex = c.vx[j] - c.vx[i], ey = c.vy[j] - c.vy[i];
      const float len = sqrtf(ex * ex + ey * ey);
      float nx = -ey / len, ny = ex / len;
      float nc = -(nx * c.vx[i] + ny * c.vy[i]);
      if (nx * mx + ny * my + nc < 0.0f) { nx = -nx; ny = -ny; nc = -nc; }
      c.nx[i] = nx; c.ny[i] = ny; c.nc[i] = nc;
    }

    // createLinearGradient(...points[4], ...points[1]) — vertices 5 and 2.
    c.gax = c.vx[5]; c.gay = c.vy[5];
    c.gdx = c.vx[2] - c.vx[5];
    c.gdy = c.vy[2] - c.vy[5];
    const float len2 = c.gdx * c.gdx + c.gdy * c.gdy;
    c.ginv = len2 > 0.0f ? 1.0f / len2 : 0.0f;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
// Coverage of a pixel centre against the convex hexagon: the minimum of the
// per-edge coverages, each 0.5 - signed distance clamped to [0,1] — the
// porting-patterns recipe with the ellipse's gradient division replaced by
// unit normals.
static inline float hexCover(const Cuboid &c, float px, float py) {
  float cover = 1.0f;
  for (int i = 0; i < 6; i++) {
    const float d = c.nx[i] * px + c.ny[i] * py + c.nc[i];
    if (d <= -0.5f) return 0.0f;
    const float ce = d >= 0.5f ? 1.0f : 0.5f + d;
    if (ce < cover) cover = ce;
  }
  return cover;
}

static void renderAll(float ph, const float *qk) {
  updateCuboids(ph, qk);

  const uint16_t bg565 = to565(BG);
  cv.fillScreen(bg565);

  // The stack's union bounding box: everything outside it is the fill above.
  float ux0 = 1e9f, uy0 = 1e9f, ux1 = -1e9f, uy1 = -1e9f;
  for (int k = 0; k < N_CUBOIDS; k++) {
    const Cuboid &c = cuboids[k];
    if (c.x0 < ux0) ux0 = c.x0;
    if (c.y0 < uy0) uy0 = c.y0;
    if (c.x1 > ux1) ux1 = c.x1;
    if (c.y1 > uy1) uy1 = c.y1;
  }
  int bx0 = (int)floorf(ux0), by0 = (int)floorf(uy0);
  int bx1 = (int)ceilf(ux1) + 1, by1 = (int)ceilf(uy1) + 1;
  if (bx0 < 0) bx0 = 0;
  if (by0 < 0) by0 = 0;
  if (bx1 > W) bx1 = W;
  if (by1 > H) by1 = H;

  const float bgR = (float)((BG >> 16) & 0xFF);
  const float bgG = (float)((BG >>  8) & 0xFF);
  const float bgB = (float)( BG        & 0xFF);

  static uint16_t lineBuf[W];

  // pushImage assumes big-endian source data by default; lineBuf holds
  // native to565 values (what drawPixel takes), so say so for the pushes.
  cv.setSwapBytes(true);

  for (int py = by0; py < by1; py++) {
    const float fy = (float)py + 0.5f;
    uint16_t *dst = lineBuf;

    for (int px = bx0; px < bx1; px++) {
      const float fx = (float)px + 0.5f;

      // Composite front to back — smallest cuboid is drawn last in the
      // canvas order, so it is topmost. T is the transparency still open
      // below the layers seen so far; an opaque layer zeroes it and ends
      // the pixel, which is the common case for every interior pixel.
      float ar = 0.0f, ag = 0.0f, ab = 0.0f, T = 1.0f;
      for (int k = N_CUBOIDS - 1; k >= 0; k--) {
        const Cuboid &c = cuboids[k];
        if (fx < c.x0 - 0.5f || fx > c.x1 + 0.5f ||
            fy < c.y0 - 0.5f || fy > c.y1 + 0.5f) continue;
        const float cover = hexCover(c, fx, fy);
        if (cover <= 0.0f) continue;
        float g = ((fx - c.gax) * c.gdx + (fy - c.gay) * c.gdy) * c.ginv;
        g = clampf(g, 0.0f, 1.0f);   // canvas gradients clamp past the stops
        const float w = cover * T;
        ar += (grad0[k][0] + gradD[k][0] * g) * w;
        ag += (grad0[k][1] + gradD[k][1] * g) * w;
        ab += (grad0[k][2] + gradD[k][2] * g) * w;
        T *= 1.0f - cover;
        if (T <= 0.002f) { T = 0.0f; break; }
      }

      if (T >= 1.0f) {   // untouched: flat background, same as the fill
        *dst++ = bg565;
        continue;
      }
      if (T > 0.0f) { ar += bgR * T; ag += bgG * T; ab += bgB * T; }
      const uint32_t col = ((uint32_t)ar << 16) | ((uint32_t)ag << 8) | (uint32_t)ab;
      *dst++ = to565Dither(col, px, py);
    }

    if (bx1 > bx0) cv.pushImage(bx0, py, bx1 - bx0, 1, lineBuf);
  }

  cv.setSwapBytes(false);   // the sprite is shared in a switcher: leave defaults
}

// ---------------------------------------------------------------------------
// Gesture and animation state
// ---------------------------------------------------------------------------
// Two independent machines that compose in the renderer: the shear (the
// original animation's playhead — idle means settled open at 1) and the
// squish (the jelly). File-scope so a switcher re-entry through setup()
// resets them — function-local statics would carry a half-finished wobble
// across sketches.
enum ShearMode  { SHEAR_IDLE, SHEAR_PLAY, SHEAR_HOLD };
enum SquishMode { SQ_QUIET, SQ_HELD, SQ_RING };

static ShearMode  shearMode = SHEAR_IDLE;
static SquishMode squishMode = SQ_QUIET;
static uint32_t   playStart = 0, ringStart = 0;
static uint32_t   pressAt = 0, lastStep = 0;
static float      phFrozen = 0.0f;
static float      ext = 1.0f, ext0 = 1.0f;   // jelly extension; 1 = at rest
static bool       wasDown = false;
static bool       restDrawn = false;

static void resetGesture() {
  shearMode  = SHEAR_IDLE;
  squishMode = SQ_QUIET;
  ext = 1.0f; ext0 = 1.0f;
  wasDown   = false;
  restDrawn = false;
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

  initCuboids();
  resetGesture();
  Serial.printf("\nthe core: %d cuboids, deterministic — tap pops, hold squishes, release wobbles\n",
                N_CUBOIDS);

  static const float EREST[N_CUBOIDS] = { 1, 1, 1, 1, 1, 1 };
  const uint32_t t0 = wallMicros();
  renderAll(1.0f, EREST);           // rest is the settled-open pose
  const uint32_t drawUs = wallMicros() - t0;
  present();
  restDrawn = true;
  Serial.printf("first frame drawn in %luus\n", (unsigned long)drawUs);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // No window, no finger: sweep the playhead so one capture run shows the
  // whole stagger. setup() wrote the rest pose, the loop covers 0 .. 1.
  static const float EREST[N_CUBOIDS] = { 1, 1, 1, 1, 1, 1 };
  static int frame = 1;
  const float ph = SKETCH_FRAMES > 1
                     ? clampf((float)(frame - 1) / (float)(SKETCH_FRAMES - 1), 0.0f, 1.0f)
                     : 0.0f;
  frame++;
  renderAll(ph, EREST);
  present();
  delay(16);
#else
  const uint32_t now = millis();

  int tx, ty;
  const bool down = touchPoint(&tx, &ty);

  if (down && !wasDown) pressAt = now;

  // A press that outlives the tap window starts squashing; if the pop was
  // mid-play it freezes under the finger.
  if (down && now - pressAt >= TAP_MS && squishMode != SQ_HELD) {
    squishMode = SQ_HELD;
    if (shearMode == SHEAR_PLAY) {
      phFrozen  = clampf((float)(now - playStart) / (float)PLAY_MS, 0.0f, 1.0f);
      shearMode = SHEAR_HOLD;
    }
  }

  if (!down && wasDown) {
    if (now - pressAt < TAP_MS) {
      shearMode = SHEAR_PLAY;              // tap: replay the pop from the top
      playStart = now;
    } else {
      ext0       = ext;                    // hold released: spring back open
      ringStart  = now;
      squishMode = fabsf(ext0 - 1.0f) > RING_DONE ? SQ_RING : SQ_QUIET;
      if (squishMode == SQ_QUIET) ext = 1.0f;
      if (shearMode == SHEAR_HOLD) {       // and let the frozen pop resume
        playStart = now - (uint32_t)(phFrozen * (float)PLAY_MS);
        shearMode = SHEAR_PLAY;
      }
    }
  }
  wasDown = down;

  // --- shear playhead: 1 at rest (the settled-open pose) ---
  float ph = 1.0f;
  switch (shearMode) {
    case SHEAR_PLAY: {
      const float t = (float)(now - playStart) / (float)PLAY_MS;
      if (t >= 1.0f) { shearMode = SHEAR_IDLE; ph = 1.0f; }
      else           { ph = t; }
      break;
    }
    case SHEAR_HOLD: ph = phFrozen; break;
    default:         ph = 1.0f; break;
  }

  // --- squish: the extension shrinks toward shut while held, rings back ---
  float ek[N_CUBOIDS];
  if (squishMode == SQ_HELD) {
    const float dt = (float)(now - lastStep) * 0.001f;
    ext += (SQUISH_MIN - ext) * (1.0f - expf(-dt / SQUISH_TAU));
    for (int k = 0; k < N_CUBOIDS; k++) ek[k] = ext;
  } else if (squishMode == SQ_RING) {
    const float t = (float)(now - ringStart) * 0.001f;
    for (int k = 0; k < N_CUBOIDS; k++) {
      const float tk = t - (float)k * RING_LAG;
      ek[k] = tk <= 0.0f
                ? ext0
                : 1.0f + (ext0 - 1.0f) * expf(-tk / RING_TAU) * cosf(6.2831853f * RING_HZ * tk);
    }
    ext = ek[0];   // so a re-press mid-wobble shrinks from where the jelly is
    const float tInner = t - (float)(N_CUBOIDS - 1) * RING_LAG;
    if (tInner > 0.0f && fabsf(ext0 - 1.0f) * expf(-tInner / RING_TAU) < RING_DONE) {
      squishMode = SQ_QUIET;
      ext = 1.0f;
      for (int k = 0; k < N_CUBOIDS; k++) ek[k] = 1.0f;
    }
  } else {
    for (int k = 0; k < N_CUBOIDS; k++) ek[k] = 1.0f;
  }
  lastStep = now;

  // --- render only while something moves, plus one frame to land at rest ---
  const bool active = shearMode != SHEAR_IDLE || squishMode != SQ_QUIET;
  if (active || !restDrawn) {
    const uint32_t t0 = wallMicros();
    renderAll(ph, ek);
    const uint32_t drawUs = wallMicros() - t0;
    present();
    restDrawn = !active;

    static uint32_t reportAt = 0;
    if (millis() - reportAt >= 1000) {
      reportAt = millis();
      Serial.printf("ph %.3f  ext %.3f  draw %luus\n", ph, ek[0], (unsigned long)drawUs);
    }
  } else {
    delay(8);
  }
#endif
}
