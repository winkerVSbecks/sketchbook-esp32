// ============================================================================
// trochoidal wave — stacked wave layers, Atkinson-dithered
// ============================================================================
//   pio run -e wave_native -t exec      SDL window
//   pio run -e wave_esp32  -t upload    the board
//   pio run -e wave_shot                headless PNG capture, then run
//                                       .pio/build/wave_shot/program DIR SEED N
//
// Ported from sketchbook-ssam/src/sketches/trochoidal-wave/dither.ts.
//
// The JS draws each layer as a closed path — moveTo(0, height), a polyline
// along the trochoid, lineTo(width, y), lineTo(width, height), fill(). Traced
// out, that path is much simpler than it looks:
//
//   * The polyline runs from x = -10*dx to x = (numberOfPoints+10)*dx, and
//     dx = width/numberOfPoints*2 — so it spans roughly [-0.1w, 2.1w] and
//     leaves the canvas on the right less than halfway through the samples.
//     Every sample past x = width is off-screen work.
//   * The two closing lineTo calls both live at or beyond x = width, so the
//     segment back from x ≈ 2.1w to (width, y) never enters the viewport and
//     contributes nothing to the filled region. `y` there is the layer's base
//     line, not the curve's value at the right edge — invisible either way.
//   * xPos is monotonic. The trochoid displaces x by radius*sin(freq*x + t),
//     so dxPos/dx = 1 - radius*freq*cos(...), and radius*freq peaks at
//     (19+18) * (0.0001+0.0001+18*0.0005) = 0.34 across every parameter the
//     generator can produce. It never folds, and the product is scale
//     invariant so that holds here too. rasterSegment() handles a fold anyway.
//
// So each layer reduces to: fill from the curve down to the bottom of the
// canvas. Later layers paint over earlier ones, and what you see of layer i is
// the band between its own curve and layer i+1's.
//
// The doubled `context.lineTo(xPos, yPos)` in the JS sample loop emits a
// zero-length segment. It contributes nothing to the fill and the path is
// never stroked. Dead.
//
// Keep in sync with the Arduino IDE copy:
//   cp src/trochoidal_wave.cpp ~/Documents/Arduino/trochoidal_wave/sketch.h
//   cp -R src/shared            ~/Documents/Arduino/trochoidal_wave/
// ============================================================================

#define SKETCH_TITLE  "trochoidal wave"
#define SKETCH_FRAMES 24

#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/dither.h"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static const uint32_t LOOP_MS  = 4000;                  // ssam duration
static const uint32_t FRAME_MS = 1000 / 24;             // ssam playFps

// Downscale factor for the dither pass.
//
// The JS uses 0.125 on a 1080px canvas: a dither cell is 8 canvas pixels, or
// 0.74% of the composition's width, and the piece is 135 cells across. Reusing
// 0.125 here would give 21x40 cells — a wave layer would be two cells tall and
// the composition would stop existing. What has to survive is the ratio of
// cell to feature, and at scale 2 that lands almost exactly on the JS:
//
//                      JS (1080, /8)     here (172x320, /2)
//   cells across       135               86
//   layer height       7-27 cells        8-32 cells
//   wave amplitude     2.5-10 cells      3-13 cells
//
// The other reading agrees: 0.74% of 172px is 1.27px, and 2 is the nearest
// integer that still reads as chunky. Must divide W and H.
static const int DITHER_SCALE = 2;
static const int SW = W / DITHER_SCALE;
static const int SH = H / DITHER_SCALE;
static_assert(W % DITHER_SCALE == 0 && H % DITHER_SCALE == 0,
              "DITHER_SCALE must divide 172 and 320");

// false reproduces the JS: only the red channel is dithered, green and blue
// pass through at full depth (see shared/dither.h for why). true is the
// per-channel colour dither the option names in dither.ts imply, and the
// livelier of the two if you want to diverge.
static const bool DITHER_ALL_CHANNELS = false;

static const int MAX_LAYERS = 20;                       // rangeFloor(5, 20)
static const int N_POINTS   = 200;                      // waveParams.numberOfPoints
static const int I_LO       = -10;                      // the JS sample range
static const int I_HI       = N_POINTS + 10;

// The JS canvas is 1080x1080; this panel is 172x320. Lengths carry over
// proportionally — x-ish quantities by W/1080, y-ish by H/1080 — which is what
// keeps amplitude-over-layer-spacing and cycles-across-the-width the same as
// the browser. Those two ratios are the whole composition. Scaling uniformly
// instead would leave the waves a third as tall as they should be.
static const float SX = (float)W / 1080.0f;
static const float SY = (float)H / 1080.0f;

static const float DX          = (float)W / N_POINTS * 2.0f;   // sample spacing
static const float AMP_WOBBLE  = 10.0f * SY;                   // sin(time) * 10
static const float FREQ_WOBBLE = 0.0001f / SX;                 // sin(time) * 0.0001
static const float FREQ_STEP   = 0.0005f / SX;                 // i * 0.0005
static const float RADIUS_STEP = 1.0f * SX;                    // i * 1

// A column no segment reached. Larger than H, so the band walk skips it.
static const float NO_CURVE = 1e9f;

// ---------------------------------------------------------------------------
// Composition state
// ---------------------------------------------------------------------------
static int      layerCount;
static bool     grayscale;
static uint32_t bg;
static uint32_t layerColor[MAX_LAYERS];

static float baseFrequency, baseAmplitude, baseRadius;  // already in panel units

// layerTop[i][px] — the curve's y at the centre of column px. Everything from
// there down to H is layer i.
static float layerTop[MAX_LAYERS][W];

// The small image the dither runs on, as three 8-bit planes.
static uint8_t smR[SW * SH], smG[SW * SH], smB[SW * SH];

// One column-group's worth of box-filter accumulators, in colour x rows.
static float cellR[SH], cellG[SH], cellB[SH];

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static void buildPalette() {
  if (grayscale) {
    // hsl(0 0% L%) with L = 75 * idx / layerCount, idx 0..layerCount. idx 0 is
    // black and gets shift()ed off as the background; the layers take 1..n.
    bg = 0x000000;
    for (int i = 0; i < layerCount; i++) {
      const float l = 75.0f * (float)(i + 1) / (float)layerCount;
      const uint32_t v = (uint32_t)(l * 2.55f + 0.5f);
      layerColor[i] = (v << 16) | (v << 8) | v;
    }
    return;
  }

  // Random.pick([...mindfulPalettes, ...autoAlbersPalettes]), then shift() the
  // first entry off for the background. The JS shift() mutates the palette
  // array it got from the imported module, so that palette is permanently one
  // colour short for the rest of the page's life — see the report.
  const Palette pal = randomMindfulOrAlbers();
  const int     nfg = pal.n - 1;
  bg = pal.c[0];
  for (int i = 0; i < layerCount; i++)
    layerColor[i] = nfg > 0 ? pal.c[1 + (i % nfg)] : 0xFFFFFF;
}

// ---------------------------------------------------------------------------
// Curve
// ---------------------------------------------------------------------------
// Stamp one polyline segment into the column tops, sampling at pixel centres.
// Written to survive a fold in xPos even though the generator can't produce
// one: the lower of the two branches wins, which is what the nonzero-winding
// fill would show.
static void rasterSegment(float *top, float xa, float ya, float xb, float yb) {
  if (xb < xa) {
    float t;
    t = xa; xa = xb; xb = t;
    t = ya; ya = yb; yb = t;
  }
  const float dx = xb - xa;
  if (dx < 1e-6f) return;

  int p0 = (int)ceilf(xa - 0.5f);
  int p1 = (int)floorf(xb - 0.5f);
  if (p0 < 0)  p0 = 0;
  if (p1 >= W) p1 = W - 1;

  const float slope = (yb - ya) / dx;
  for (int p = p0; p <= p1; p++) {
    const float y = ya + ((float)p + 0.5f - xa) * slope;
    if (y < top[p]) top[p] = y;
  }
}

// Walk the trochoid for layer i and fill layerTop[i].
//
//   xPos = x - radius * sin(freq * x + time)
//   yPos = y - amplitude * cos(freq * x + time)
//
// x advances by a constant DX, so the angle advances by a constant dTheta and
// sin/cos come from a rotation recurrence instead of 440 library calls per
// layer. Drift over the ~220 samples is around 1e-5 — invisible, and the
// transcendentals are the single largest cost in the frame otherwise.
static void buildLayer(int i, float time) {
  float *top = layerTop[i];
  for (int p = 0; p < W; p++) top[p] = NO_CURVE;

  const float yBase     = (float)i * ((float)H / (float)layerCount);
  const float amplitude = baseAmplitude + sinf(time) * AMP_WOBBLE;
  const float frequency = baseFrequency + sinf(time) * FREQ_WOBBLE + (float)i * FREQ_STEP;
  const float radius    = baseRadius + (float)i * RADIUS_STEP;

  const float dTheta = frequency * DX;
  const float cd = cosf(dTheta), sd = sinf(dTheta);

  float x = (float)I_LO * DX;
  float a = frequency * x + time;
  float s = sinf(a), c = cosf(a);

  float xPrev = x - radius * s;
  float yPrev = yBase - amplitude * c;

  for (int k = I_LO + 1; k < I_HI; k++) {
    const float sn = s * cd + c * sd;              // rotate (s, c) by dTheta
    const float cn = c * cd - s * sd;
    s = sn; c = cn;
    x += DX;

    const float xPos = x - radius * s;
    const float yPos = yBase - amplitude * c;
    rasterSegment(top, xPrev, yPrev, xPos, yPos);
    xPrev = xPos; yPrev = yPos;

    if (xPos >= (float)W) break;                   // the rest is off-canvas
  }
}

// ---------------------------------------------------------------------------
// Box downsample
// ---------------------------------------------------------------------------
// The JS renders full size, draws that into a canvas an eighth the size (a box
// filter), dithers, and draws the result back up with smoothing off. Rendering
// full size here would mean 55k pixel writes into the sprite followed by 55k
// readbacks, all to compute an average this sketch already knows analytically:
// every column is a short stack of flat-coloured spans. So the box filter runs
// straight off the geometry. It is exact rather than sampled, which also means
// the wave edges arrive antialiased for free.

static void addSpan(float ya, float yb, uint32_t rgb) {
  if (ya < 0.0f)       ya = 0.0f;
  if (yb > (float)H)   yb = (float)H;
  if (yb <= ya)        return;

  const float r = (float)((rgb >> 16) & 0xFF);
  const float g = (float)((rgb >>  8) & 0xFF);
  const float b = (float)( rgb        & 0xFF);

  int oy0 = (int)(ya * (1.0f / DITHER_SCALE));
  int oy1 = (int)ceilf(yb * (1.0f / DITHER_SCALE));
  if (oy0 < 0)   oy0 = 0;
  if (oy1 > SH)  oy1 = SH;

  for (int oy = oy0; oy < oy1; oy++) {
    const float lo = (float)(oy * DITHER_SCALE);
    const float hi = lo + DITHER_SCALE;
    const float a  = ya > lo ? ya : lo;
    const float z  = yb < hi ? yb : hi;
    const float wgt = z - a;
    if (wgt <= 0.0f) continue;
    cellR[oy] += r * wgt;
    cellG[oy] += g * wgt;
    cellB[oy] += b * wgt;
  }
}

static void downsample() {
  const float invN = 1.0f / (float)(DITHER_SCALE * DITHER_SCALE);

  float    bandTop[MAX_LAYERS];
  uint32_t bandCol[MAX_LAYERS];

  for (int oy = 0; oy < SH; oy++) cellR[oy] = cellG[oy] = cellB[oy] = 0.0f;

  int ox = 0, inGroup = 0;
  for (int px = 0; px < W; px++) {
    // Painter's order collapsed: layer i hides layer i-1 wherever both cover,
    // so walking bottom-up and keeping a running minimum of the tops gives the
    // spans that actually survive.
    float lo = (float)H;
    int   nb = 0;
    for (int i = layerCount - 1; i >= 0; i--) {
      const float t = layerTop[i][px];
      if (t >= lo) continue;
      lo = t;
      bandTop[nb] = t < 0.0f ? 0.0f : t;
      bandCol[nb] = layerColor[i];
      nb++;
    }

    addSpan(0.0f, nb ? bandTop[nb - 1] : (float)H, bg);
    for (int k = nb - 1; k >= 0; k--)
      addSpan(bandTop[k], k == 0 ? (float)H : bandTop[k - 1], bandCol[k]);

    if (++inGroup < DITHER_SCALE) continue;

    for (int oy = 0; oy < SH; oy++) {
      const int idx = oy * SW + ox;
      smR[idx] = (uint8_t)(cellR[oy] * invN + 0.5f);
      smG[idx] = (uint8_t)(cellG[oy] * invN + 0.5f);
      smB[idx] = (uint8_t)(cellB[oy] * invN + 0.5f);
      cellR[oy] = cellG[oy] = cellB[oy] = 0.0f;
    }
    ox++;
    inGroup = 0;
  }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
static uint32_t tWaves, tBox, tDither, tPush;

static void drawFrame(float time) {
  uint32_t t0 = wallMicros();
  for (int i = 0; i < layerCount; i++) buildLayer(i, time);

  uint32_t t1 = wallMicros();
  downsample();

  uint32_t t2 = wallMicros();
  ditherAtkinsonPlane(smR, SW, SH);
  if (DITHER_ALL_CHANNELS) {
    ditherAtkinsonPlane(smG, SW, SH);
    ditherAtkinsonPlane(smB, SW, SH);
  }
  ditherExpandToSprite(smR, smG, smB, SW, SH, DITHER_SCALE);

  uint32_t t3 = wallMicros();
  present();

  uint32_t t4 = wallMicros();
  tWaves = t1 - t0; tBox = t2 - t1; tDither = t3 - t2; tPush = t4 - t3;
}

static void generate(uint32_t seed) {
  Serial.printf("\nSeed: %lu\n", (unsigned long)seed);
  rngSeed(seed);

  // Same draw order as the JS so the two read alike side by side.
  baseFrequency = rngRange(0.00001f, 0.0001f) / SX;
  baseRadius    = (float)rngRangeFloor(1, 20) * SX;
  baseAmplitude = (float)rngRangeFloor(20, 80) * SY;
  layerCount    = rngRangeFloor(5, 20);
  grayscale     = rngBoolean();

  buildPalette();

  Serial.printf("%d layers, %s, bg #%06lX, freq %.5f, amp %.1f, radius %.1f\n",
                layerCount, grayscale ? "grey" : "palette",
                (unsigned long)bg, baseFrequency, baseAmplitude, baseRadius);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    while (true) delay(1000);
  }

  generate(newSeed());
}

void loop() {
  static uint32_t compositionStart = 0;
  static bool     started = false;
  static uint32_t frames  = 0;

  // Like the JS, one composition runs forever — RESET is what replaces it.
  // compositionStart is latched on the first frame and is only a phase origin,
  // so the playhead below is a pure loop with no deadline in it.
  const uint32_t now = millis();
  if (!started) { compositionStart = now; started = true; }

  const float playhead = (float)((now - compositionStart) % LOOP_MS) / (float)LOOP_MS;
  drawFrame(playhead * 2.0f * (float)M_PI);

  const uint32_t total = tWaves + tBox + tDither + tPush;
  if (++frames % 24 == 0)
    Serial.printf("frame %lu.%02lums = waves %lu + box %lu + dither/expand %lu + push %luus\n",
                  (unsigned long)(total / 1000), (unsigned long)(total % 1000) / 10,
                  (unsigned long)tWaves, (unsigned long)tBox,
                  (unsigned long)tDither, (unsigned long)tPush);

  const uint32_t spent = total / 1000;
  if (spent < FRAME_MS) delay(FRAME_MS - spent);
}
