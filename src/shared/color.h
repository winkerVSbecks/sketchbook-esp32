// ============================================================================
// color.h — colour space conversion, contrast metrics, RGB565 output
// ============================================================================
// Colours live as 0xRRGGBB `uint32_t` everywhere except the moment a pixel is
// written. Blend and compare in 888; convert to 565 last.
//
// The two contrast functions answer different questions and are not
// interchangeable:
//
//   wcagContrast()  "is this legible against that background" — a luminance
//                   ratio, blind to hue. Correct for foreground-vs-background.
//   deltaEok()      "can I tell these two apart" — perceptual distance in
//                   oklab. Correct for foreground-vs-foreground.
//
// Using WCAG for both is what collapses a palette to a single colour: a
// saturated blue and a saturated rust score about 1.0 against each other, so
// whichever comes first knocks the other out.
// ============================================================================
#pragma once

#include <math.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// sRGB transfer function
// ---------------------------------------------------------------------------
static inline float srgbLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static inline float srgbEncode(float c) {
  return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint32_t rgbPack(float r, float g, float b) {
  uint32_t ri = (uint32_t)(clampf(r, 0.0f, 1.0f) * 255.0f + 0.5f);
  uint32_t gi = (uint32_t)(clampf(g, 0.0f, 1.0f) * 255.0f + 0.5f);
  uint32_t bi = (uint32_t)(clampf(b, 0.0f, 1.0f) * 255.0f + 0.5f);
  return (ri << 16) | (gi << 8) | bi;
}

// ---------------------------------------------------------------------------
// Contrast metrics
// ---------------------------------------------------------------------------
static inline float relLuminance(uint32_t rgb) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static inline float wcagContrast(uint32_t a, uint32_t b) {
  float la = relLuminance(a), lb = relLuminance(b);
  float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
  return (hi + 0.05f) / (lo + 0.05f);
}

// ---------------------------------------------------------------------------
// Oklab / Oklch
// ---------------------------------------------------------------------------
// Lowercase locals throughout: the Arduino core's binary.h #defines B0, B1,
// B01, B10 ... as binary literals, so `float B1` fails to compile on device
// under core 2.x (which is what PlatformIO's espressif32 platform ships). Core
// 3.x dropped them, so the Arduino IDE hides this — don't "tidy" these back to
// uppercase.
static inline void toOklab(uint32_t rgb, float &lOut, float &aOut, float &bOut) {
  float r = srgbLinear(((rgb >> 16) & 0xFF) / 255.0f);
  float g = srgbLinear(((rgb >>  8) & 0xFF) / 255.0f);
  float b = srgbLinear(( rgb        & 0xFF) / 255.0f);
  float l_ = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
  float m_ = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
  float s_ = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
  lOut = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
  aOut = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
  bOut = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

static inline float deltaEok(uint32_t c1, uint32_t c2) {
  float l1, a1, b1, l2, a2, b2;
  toOklab(c1, l1, a1, b1);
  toOklab(c2, l2, a2, b2);
  float dl = l1 - l2, da = a1 - a2, db = b1 - b2;
  return sqrtf(dl * dl + da * da + db * db);
}

// Oklab -> linear sRGB. Components may fall outside [0,1] — that's the gamut
// test, so don't clamp here.
static inline void oklabToLinear(float l, float a, float b, float &r, float &g, float &bl) {
  float l_ = l + 0.3963377774f * a + 0.2158037573f * b;
  float m_ = l - 0.1055613458f * a - 0.0638541728f * b;
  float s_ = l - 0.0894841775f * a - 1.2914855480f * b;
  float ll = l_ * l_ * l_, mm = m_ * m_ * m_, ss = s_ * s_ * s_;
  r  =  4.0767416621f * ll - 3.3077115913f * mm + 0.2309699292f * ss;
  g  = -1.2684380046f * ll + 2.6097574011f * mm - 0.3413193965f * ss;
  bl = -0.0041960863f * ll - 0.7034186147f * mm + 1.7076147010f * ss;
}

struct Oklch { float l, c, h; };   // h in degrees

static inline uint32_t oklabToRgb(float l, float a, float b) {
  float r, g, bl;
  oklabToLinear(l, a, b, r, g, bl);
  return rgbPack(srgbEncode(clampf(r, 0.0f, 1.0f)),
                 srgbEncode(clampf(g, 0.0f, 1.0f)),
                 srgbEncode(clampf(bl, 0.0f, 1.0f)));
}

static inline bool _oklchInGamut(float l, float c, float hRad) {
  float r, g, b;
  oklabToLinear(l, c * cosf(hRad), c * sinf(hRad), r, g, b);
  const float e = 1.0f / 512.0f;
  return r >= -e && r <= 1.0f + e && g >= -e && g <= 1.0f + e && b >= -e && b <= 1.0f + e;
}

// Oklch -> sRGB, gamut-mapped by reducing chroma until the colour is
// representable. This is what culori's clampChroma does, and several of the
// ported palettes need it — auto-albers reaches chroma 0.40, which is far
// outside sRGB. Naive clipping there turns distinct ramp steps into the same
// flat colour.
static inline uint32_t oklchToRgb(float l, float c, float hDeg) {
  const float hRad = hDeg * (float)M_PI / 180.0f;
  if (!_oklchInGamut(l, c, hRad)) {
    float lo = 0.0f, hi = c;
    for (int i = 0; i < 20; i++) {
      float mid = 0.5f * (lo + hi);
      if (_oklchInGamut(l, mid, hRad)) lo = mid; else hi = mid;
    }
    c = lo;
  }
  return oklabToRgb(l, c * cosf(hRad), c * sinf(hRad));
}
static inline uint32_t oklchToRgb(const Oklch &o) { return oklchToRgb(o.l, o.c, o.h); }

static inline Oklch rgbToOklch(uint32_t rgb) {
  float l, a, b;
  toOklab(rgb, l, a, b);
  float c = sqrtf(a * a + b * b);
  float h = atan2f(b, a) * 180.0f / (float)M_PI;
  if (h < 0.0f) h += 360.0f;
  return Oklch{ l, c, h };
}

// Nudge a colour's oklch lightness, holding hue and chroma. The clamp range
// matches the JS `shade()` helper.
static inline uint32_t shadeL(uint32_t rgb, float dl, float lo = 0.08f, float hi = 0.93f) {
  Oklch o = rgbToOklch(rgb);
  return oklchToRgb(clampf(o.l + dl, lo, hi), o.c, o.h);
}

// ---------------------------------------------------------------------------
// RGB565 output
// ---------------------------------------------------------------------------
static inline uint16_t to565(uint32_t rgb) {
  return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline uint32_t from565(uint16_t c) {
  uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return (r << 16) | (g << 8) | b;
}

static inline uint16_t blend565(uint32_t a, uint32_t b, float t) {
  float ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
  float br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
  uint8_t r  = (uint8_t)(ar + (br - ar) * t + 0.5f);
  uint8_t g  = (uint8_t)(ag + (bg - ag) * t + 0.5f);
  uint8_t bl = (uint8_t)(ab + (bb - ab) * t + 0.5f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

// 565 gives 32 levels of red and blue. Flat fills are fine; a long gradient
// bands visibly. Dithering at the point of conversion costs a table lookup and
// removes the banding.
static const uint8_t BAYER4[4][4] = {
  {  0,  8,  2, 10 },
  { 12,  4, 14,  6 },
  {  3, 11,  1,  9 },
  { 15,  7, 13,  5 },
};

static inline uint16_t to565Dither(uint32_t rgb, int x, int y) {
  // Threshold in [-0.5, 0.5) of a destination step, scaled per channel.
  const float t = (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
  float r = (float)((rgb >> 16) & 0xFF) + t * 8.0f;    // 8 = 256/32 levels
  float g = (float)((rgb >>  8) & 0xFF) + t * 4.0f;    // 4 = 256/64 levels
  float b = (float)( rgb        & 0xFF) + t * 8.0f;
  uint32_t ri = (uint32_t)clampf(r, 0.0f, 255.0f);
  uint32_t gi = (uint32_t)clampf(g, 0.0f, 255.0f);
  uint32_t bi = (uint32_t)clampf(b, 0.0f, 255.0f);
  return (uint16_t)(((ri & 0xF8) << 8) | ((gi & 0xFC) << 3) | (bi >> 3));
}

// Lightest entry of a palette, by oklab L. The JS `lightest()` helper.
static inline uint32_t paletteLightest(const uint32_t *p, int n) {
  uint32_t best = p[0];
  float bestL = -1.0f;
  for (int i = 0; i < n; i++) {
    float l, a, b;
    toOklab(p[i], l, a, b);
    if (l > bestL) { bestL = l; best = p[i]; }
  }
  return best;
}
