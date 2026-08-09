// ============================================================================
// subtractive.h — generateColors(), the RYB-wheel palette generator
// ============================================================================
// Ported from sketchbook-ssam/src/subtractive-color.ts, which is three pieces
// stacked:
//
//   1. rampensau's generateColorRamp() walks six evenly spaced steps along a
//      hue spiral, easing saturation as x^2 and lightness as x^1.1.
//   2. Each HSL step is converted to RGB *with the lightness inverted*
//      (`l: 1 - l`) ...
//   3. ... and that RGB triple is then read as a **red/yellow/blue coordinate**
//      and trilinearly interpolated through a hand-picked RYB cube whose
//      (0,0,0) corner is paper white and whose (1,1,1) corner is ink black.
//
// The double inversion is the point: the ramp's bright end lands on the white
// corner of the cube, so the palette behaves like pigment on paper rather than
// light on a screen. Hence "subtractive".
//
// Faithfully unclamped. `newOptions()` can hand rampensau a saturation of 1.3
// or a lightness of 1.12, so the intermediate HSL and RGB values run outside
// [0, 1] and culori's smoothstep folds them back rather than clipping. Clamping
// early gives visibly different colours; the browser only clamps at the very
// end, when the `rgb(...)` string is parsed, and so does this.
// ============================================================================
#pragma once

#include <math.h>
#include <stdint.h>
#include "color.h"
#include "prng.h"

static const int SUBTRACTIVE_N = 6;   // newOptions().total

// The RYB cube, in the corner order culori's trilerp() takes:
//   a000 a010 a100 a110 a001 a011 a101 a111
//   white red  yellow orange blue violet green black
// with tx = red, ty = yellow, tz = blue.
static const float RYB_CUBE[8][3] = {
  { 248.0f / 255.0f,          237.0f / 255.0f,          220.0f / 255.0f          },  // white
  { 0.8901960784313725f,      0.1411764705882353f,      0.12941176470588237f     },  // red
  { 0.9529411764705882f,      0.9019607843137255f,      0.0f                     },  // yellow
  { 0.9411764705882353f,      0.5568627450980392f,      0.10980392156862745f     },  // orange
  { 0.08627450980392157f,     0.6f,                     0.8549019607843137f      },  // blue
  { 0.47058823529411764f,     0.13333333333333333f,     0.6666666666666666f      },  // violet
  { 0.0f,                     0.5568627450980392f,      0.3568627450980392f      },  // green
  { 29.0f / 255.0f,           28.0f / 255.0f,           28.0f / 255.0f           },  // black
};

static inline float easingSmoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

static inline float lerpf(float a, float b, float t) { return a + t * (b - a); }

static inline float blerpf(float a00, float a01, float a10, float a11, float tx, float ty) {
  return lerpf(lerpf(a00, a01, tx), lerpf(a10, a11, tx), ty);
}

static inline float trilerpf(int ch, float tx, float ty, float tz) {
  return lerpf(blerpf(RYB_CUBE[0][ch], RYB_CUBE[1][ch], RYB_CUBE[2][ch], RYB_CUBE[3][ch], tx, ty),
               blerpf(RYB_CUBE[4][ch], RYB_CUBE[5][ch], RYB_CUBE[6][ch], RYB_CUBE[7][ch], tx, ty),
               tz);
}

// HSL -> RGB, culori's convertHslToRgb. Out-of-range s and l are passed
// through rather than clamped, which is what the JS does and what the folding
// in ryb2rgb() then depends on.
static inline void hslToRgbRaw(float h, float s, float l, float &r, float &g, float &b) {
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;                        // culori's normalizeHue

  const float m1 = l + s * (l < 0.5f ? l : 1.0f - l);
  const float m2 = m1 - (m1 - l) * 2.0f * fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
  const float lo = 2.0f * l - m1;

  switch ((int)floorf(h / 60.0f)) {
    case 0:  r = m1; g = m2; b = lo; break;
    case 1:  r = m2; g = m1; b = lo; break;
    case 2:  r = lo; g = m1; b = m2; break;
    case 3:  r = lo; g = m2; b = m1; break;
    case 4:  r = m2; g = lo; b = m1; break;
    case 5:  r = m1; g = lo; b = m2; break;
    default: r = lo; g = lo; b = lo; break;
  }
}

// Read (r, y, b) as coordinates in the RYB cube. Clamped on the way out, which
// is where the browser clamps too — a `rgb()` string with out-of-range
// components is clipped by the CSS parser, not by culori.
static inline uint32_t ryb2rgb(float r, float y, float b) {
  const float tx = easingSmoothstep(r);
  const float ty = easingSmoothstep(y);
  const float tz = easingSmoothstep(b);
  return rgbPack(trilerpf(0, tx, ty, tz), trilerpf(1, tx, ty, tz), trilerpf(2, tx, ty, tz));
}

// generateColors(). Fills `out` with SUBTRACTIVE_N colours.
//
// The RNG draws happen in the JS's evaluation order — object literals evaluate
// their properties top to bottom, and both ternaries consume exactly one
// chance() before their branch — so the stream advances the same way even
// though the values differ (mulberry32, not ARC4).
static inline void generateSubtractiveColors(uint32_t *out) {
  const float hStart  = rngRange(0.0f, 360.0f);
  const float hCycles = rngChance() ? rngRange(-1.25f, -0.25f) : rngRange(1.25f, 2.25f);

  float s0, s1;
  if (rngChance(0.7f)) { s0 = rngRange(0.2f, 1.2f); s1 = rngRange(0.25f, 1.3f); }
  else                 { s0 = 1.0f;                 s1 = rngNext();             }

  const float l0 = rngChance() ? rngRange(0.55f, 1.3f) : rngRange(0.88f, 1.12f);
  const float l1 = rngRange(0.0f, 0.4f);

  const float sDiff = s1 - s0, lDiff = l1 - l0;

  for (int i = 0; i < SUBTRACTIVE_N; i++) {
    const float relI = (float)i / (float)(SUBTRACTIVE_N - 1);

    // hEasing is identity and hStartCenter is 0.5, so the hue term collapses
    // to a straight spiral: hStart at the middle step, ±180*hCycles at the ends.
    const float h = fmodf(360.0f + hStart + (0.5f - relI) * (360.0f * hCycles), 360.0f);
    const float s = s0 + sDiff * relI * relI;               // sEasing: x^2
    const float l = l0 + lDiff * powf(relI, 1.1f);          // lEasing: x^1.1

    float r, g, b;
    hslToRgbRaw(h + 360.0f, s, 1.0f - l, r, g, b);          // hsl2farbrad's (h+360)%360
    out[i] = ryb2rgb(r, g, b);
  }
}
