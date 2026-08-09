# Porting patterns

## Contents

- [Porting patterns](#porting-patterns)
  - [Contents](#contents)
  - [Reducing canvas paths to primitives](#reducing-canvas-paths-to-primitives)
  - [Language-level translation rules](#language-level-translation-rules)
  - [Seeded randomness](#seeded-randomness)
  - [Colour](#colour)
    - [Two metrics, two jobs](#two-metrics-two-jobs)
    - [oklch to RGB565](#oklch-to-rgb565)
    - [Banding](#banding)
    - [Always add a fallback](#always-add-a-fallback)
  - [Analytic anti-aliasing](#analytic-anti-aliasing)
  - [Performance](#performance)
  - [Structuring the port](#structuring-the-port)

## Reducing canvas paths to primitives

Do this before writing any C++. Trace each path construction on paper and work
out the region it fills. Canvas path code is frequently more complicated than
the shape it produces.

Things to watch for:

- **`arcTo` resolves to a circular arc tangent to two lines.** Compute the
  tangent points. When the current point already equals the first tangent
  point, no connecting line is drawn. Often the whole construction is a
  quarter disc centred on a corner.
- **Zero-area subpaths fill nothing.** A `moveTo` + `lineTo` pair with no third
  point is dead code, however prominent it looks.
- **`closePath` draws a straight chord**, not an arc back to the start. A
  triangle plus a circular segment often unions into a quarter disc.
- **Square-cell assumptions.** `arcTo(..., w)` with radius `w` only produces the
  intended shape when the cell is square. On a 172×320 panel cells usually
  aren't, so either keep them square and centre the grid, or generalise the
  shape to an ellipse quadrant. Ellipse quadrants preserve edge continuity
  between neighbouring cells because the arcs still meet the corners, so
  tile-matching rules transfer unchanged.

Report the reduction to the user. "Your five cell types are all quarter discs
centred on a corner" is more useful than any amount of translated code, and it
tells them whether the sketch was ever as complex as it looked.

## Language-level translation rules

**Use `float`, never `double`.** The S3's FPU is single-precision; `double` is
software-emulated and roughly 20x slower. JS numbers are all doubles, so a
mechanical port lands in the slow path silently. Write `2.0f` not `2.0`, and
use the `f`-suffixed math functions: `sinf`, `cosf`, `powf`, `fmodf`, `sqrtf`,
`cbrtf`, `floorf`, `lroundf`.

**Preallocate everything.** No `new` in the draw loop, no growing arrays. Use
fixed-size arrays with an explicit count. `int items[MAX]; int n = 0;`

**Encode enums and lookup tables as integers.** String-keyed objects become
`enum` plus arrays. Sets of allowed values become bitmasks — a five-element
membership test is one `uint8_t` and a shift.

**Replicate iteration order exactly** when porting logic that depends on it.
JS object key order is insertion order and `Array.sort` is stable, so ties
resolve to first-seen. Mirror that with a strict-greater-than comparison
scanning in the original order.

**Mutation during iteration is often load-bearing.** A `forEach` that modifies
the array it's walking means earlier changes feed later ones. Keep the same
in-place single pass rather than snapshotting.

## Seeded randomness

`canvas-sketch-util/random` uses ARC4 (David Bau's seedrandom). Reimplementing
it in C is not worth the effort, so exact seed parity is off the table by
default.

If the user wants a seed to mean the same thing in the browser and on the
board, the cheap fix runs the other way: put mulberry32 in the JS sketch. Six
lines there beats a few hundred here.

```cpp
static uint32_t rngState = 1;

static void  rngSeed(uint32_t s) { rngState = s ? s : 1; }
static float rngNext() {
  rngState += 0x6D2B79F5u;
  uint32_t t = rngState;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return (float)((t ^ (t >> 14)) >> 8) / 16777216.0f;
}
static int rngIndex(int n) { int i = (int)(rngNext() * n); return i >= n ? n - 1 : i; }

static void rngShuffle(uint32_t *a, int n) {          // Fisher-Yates
  for (int i = n - 1; i > 0; i--) {
    int j = rngIndex(i + 1);
    uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
  }
}
```

Seed from `esp_random()` on device, `rand()` on native, or a fixed number while
tuning.

## Colour

Store palettes as `uint32_t` 0xRRGGBB. Convert to RGB565 only at the point of
writing a pixel — do all blending and comparison in 888 or better.

```cpp
static inline float srgbLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static inline uint16_t to565(uint32_t rgb) {
  return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}
```

### Two metrics, two jobs

**WCAG contrast** is a luminance ratio. Correct for "is this legible against
that background". Wrong for "are these two colours distinguishable" — it's
blind to hue, so a saturated blue and a saturated rust score about 1.0.

```cpp
static float relLuminance(uint32_t rgb) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}
static float wcagContrast(uint32_t a, uint32_t b) {
  float la = relLuminance(a), lb = relLuminance(b);
  float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
  return (hi + 0.05f) / (lo + 0.05f);
}
```

**Oklab distance** is the right metric for colour-vs-colour separation.

```cpp
static void toOklab(uint32_t rgb, float &L, float &A, float &B) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
  float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
  float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
  L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}
static float deltaEok(uint32_t c1, uint32_t c2) {
  float L1, A1, B1, L2, A2, B2;
  toOklab(c1, L1, A1, B1);
  toOklab(c2, L2, A2, B2);
  float dL = L1 - L2, dA = A1 - A2, dB = B1 - B2;
  return sqrtf(dL * dL + dA * dA + dB * dB);
}
```

Threshold around 0.20 as a starting point; 0.10 is permissive, 0.35 strict.
Expose it as a named constant so it can be tuned in SDL.

Culori's equivalent for the JS side is `differenceEuclidean('oklab')`.

### oklch to RGB565

For sketches that specify colour in oklch, resolve palettes once at setup
rather than per pixel. Go oklch → oklab → linear sRGB → gamma → 565.

### Banding

RGB565 gives 32 levels of red and blue, 64 of green. Flat fills are fine.
Long gradients band visibly — add a 4×4 Bayer dither at the conversion point
if the sketch depends on smooth gradients.

### Always add a fallback

Filter chains that produce an empty colour set are survivable in JS — it paints
`undefined`. On device that's a crash or a blank screen. Add a two-stage
fallback: relax the strictest filter, then fall back to black or white by
background luminance.

## Analytic anti-aliasing

At 172×320, aliasing is the most visible difference from the browser.
LovyanGFX's `fillArc` aliases and its angle convention is easy to get wrong.
Compute coverage directly instead.

For a quarter ellipse centred on `(cx, cy)` with radii `(rx, ry)`:

```cpp
for (int py = y0; py < y1; py++) {
  const float dy = (py + 0.5f) - cy;
  const float ny = dy / ry, ny2 = ny * ny;

  for (int px = x0; px < x1; px++) {
    const float dx = (px + 0.5f) - cx;
    const float nx = dx / rx;
    const float q  = sqrtf(nx * nx + ny2);        // 1.0 == exactly on the edge

    float cover;
    if (q < 0.0001f) {
      cover = 1.0f;
    } else {
      // q is a ratio, not a distance — divide by the gradient magnitude to
      // convert it into a signed distance in pixels
      const float gradLen = sqrtf(nx * nx / (rx*rx) + ny2 / (ry*ry)) / q;
      const float dist    = (q - 1.0f) / gradLen;
      cover = 0.5f - dist;
      if (cover <= 0.0f) continue;
      if (cover >  1.0f) cover = 1.0f;
    }
    cv.drawPixel(px, py, cover >= 1.0f ? to565(fg) : blend565(bg, fg, cover));
  }
}
```

A pixel centred exactly on the curve gets 0.5; half a pixel inside gets 1.0.
When `rx == ry` the gradient division cancels and this reduces to true signed
distance, `sqrt(dx² + dy²) - r`. Because it's per-pixel rather than a scanline
span fill, near-vertical edges antialias as well as near-horizontal ones.

**The enabling condition**: shapes confined to non-overlapping regions blend
only against the background, so no framebuffer readback is needed. Check this
holds before relying on it.

Measured cost: about 2ms for a full 172×320 screen on desktop, comfortably
under a frame on the S3. For a static sketch it's free.

## Performance

Only relevant for animated sketches.

- Full-screen push at 40MHz SPI ≈ 22ms → ~45fps ceiling before drawing anything
- At 80MHz ≈ 11ms → ~90fps, but GPIO-matrix routing makes 80MHz unreliable
- **Dirty rects are the real win.** Most generative animation changes small
  regions; push only those with `pushSprite(&lcd, x, y, w, h)` inside
  `startWrite()`/`endWrite()`
- Keep the sprite in internal SRAM: `cv.setPsram(false)`
- Supersampling is available if needed: render at 2× into PSRAM (344×640×2 =
  440KB, fits) and box-downsample

## Structuring the port

Split the source so the platform-specific part is small and everything else is
shared:

1. Platform shims (`#ifdef ARDUINO`)
2. Panel class (`#ifdef ARDUINO`)
3. Config constants — grid resolution, timings, thresholds, all named
4. Palettes
5. PRNG
6. Colour helpers
7. Generator — pure math, no drawing, portable verbatim
8. Renderer — the only part touching the sprite
9. `setup` / `loop`
10. SDL `main` (`#ifndef ARDUINO`)

The generator should print its results to serial. Getting correct seeds,
palettes, and layout counts on the console before drawing anything makes the
rendering step much easier to debug.
