// ============================================================================
// harmony.h — the part of pro-color-harmonies this sketchbook actually uses
// ============================================================================
// Port of `ColorPaletteGenerator.generate()` from the `pro-color-harmonies` npm
// package, which the JS sketches drive from a random oklch base colour.
//
// Only the `tintsShades` harmony is here, and it is complete: all five styles,
// the Bezold-Brücke hue compensation, and the same [0.01, 0.99] / [0, 0.37]
// clamps. That is the harmony the sketches select.
//
// The other five (analogous / complementary / triadic / tetradic /
// splitComplementary) are NOT ported and fall back to tintsShades. They aren't
// hard, they're just bulky: each one reads a hue table and a variation table
// keyed by (harmony, style), then runs the result through `enhancePalette`,
// which reads two more tables — about 2000 lines of literal numbers in the
// published bundle. Nothing in this repo asks for them. If one is ever needed,
// the tables are in
//   sketchbook-ssam/node_modules/pro-color-harmonies/dist/pro-color-harmonies.es.js
// under getAnalogousHues / getAnalogousVariations / getChromaNarrative /
// getColorHierarchy and friends.
//
// Also not ported: the sine / wave / zap / block post-modifiers. The library
// applies them after generation; the sketches pass 0 for all four (deliberately
// — they're commented out in the JS), and at 0 `applyModifiers` is the
// identity.
//
// Output is Oklch, not RGB. Run it through `oklchToRgb()` from color.h, which
// gamut-maps by chroma reduction the way culori's clampChroma does — the
// tintsShades ramp reaches L 0.98 at the base chroma, which is far outside
// sRGB, and naive clipping there flattens the top of the ramp.
// ============================================================================
#pragma once

#include <math.h>
#include <stdint.h>

#include "color.h"

// ---------------------------------------------------------------------------
// Library primitives
// ---------------------------------------------------------------------------
enum HarmonyStyle : uint8_t {
  HS_DEFAULT = 0,   // the library has no `default` case, so this *is* HS_SQUARE
  HS_SQUARE,
  HS_TRIANGLE,
  HS_CIRCLE,
  HS_DIAMOND,
  HS_COUNT,
};

enum HarmonyType : uint8_t {
  HARM_ANALOGOUS = 0,
  HARM_COMPLEMENTARY,
  HARM_TRIADIC,
  HARM_TETRADIC,
  HARM_SPLIT_COMPLEMENTARY,
  HARM_TINTS_SHADES,
  HARM_COUNT,
};

static const char *HARMONY_STYLE_NAMES[HS_COUNT] = {
  "default", "square", "triangle", "circle", "diamond",
};

static inline float harmonyNormalizeHue(float h) {
  const float r = fmodf(h, 360.0f);
  return r < 0.0f ? r + 360.0f : r;
}

// clampOKLCH — OKLCH_LIMITS in the library.
static inline Oklch harmonyClampOklch(float l, float c, float h) {
  return Oklch{ clampf(l, 0.01f, 0.99f), clampf(c, 0.0f, 0.37f), h };
}

static inline float harmonyLerp(float t, float a, float b) { return a + t * (b - a); }

// Bezold-Brücke: perceived hue drifts as lightness changes, so a ramp built by
// holding hue constant doesn't read as one hue. This nudges it back, by an
// amount proportional to the lightness step and signed per hue band.
static inline float bezoldBruckeShift(float hue, float dl) {
  if (hue >= 220.0f && hue <= 280.0f) return dl * -8.0f;
  if (hue >=  60.0f && hue <=  90.0f) return dl * -5.0f;
  if ((hue >= 0.0f && hue <= 30.0f) || hue >= 330.0f) return dl * 3.0f;
  if (hue >= 150.0f && hue <= 200.0f) return dl * -4.0f;
  return 0.0f;
}

// ---------------------------------------------------------------------------
// tintsShades
// ---------------------------------------------------------------------------
// Six fixed lightness stops through the base hue. Note what that means: only
// the base's *chroma and hue* reach the output for default/square/circle — the
// base lightness is read solely by triangle and diamond, which use it to decide
// which stops are tints and which are shades.
static const int   HARMONY_N = 6;
static const float HARMONY_STOPS[HARMONY_N] = { 0.02f, 0.25f, 0.38f, 0.62f, 0.84f, 0.98f };

static void generateTintsAndShades(const Oklch &base, HarmonyStyle style, Oklch *out) {
  for (int i = 0; i < HARMONY_N; i++) {
    const float stop = HARMONY_STOPS[i];
    float l = stop, c = base.c, h = base.h;

    switch (style) {
      case HS_TRIANGLE: {
        const float dl = stop - base.l;
        const float ad = fabsf(dl);
        const float mul = (stop < base.l) ? 1.0f + ad * 0.4f
                                          : (1.0f - ad * 0.8f > 0.2f ? 1.0f - ad * 0.8f : 0.2f);
        h = harmonyNormalizeHue(base.h + bezoldBruckeShift(base.h, dl));
        c = base.c * mul;
        break;
      }
      case HS_CIRCLE: {
        const float k = powf(1.0f - stop, 1.5f) * 0.8f + 0.2f;
        c = clampf(base.c * k * 1.2f, 0.0f, 0.37f);
        h = harmonyNormalizeHue(base.h + (stop - 0.5f) * 10.0f);
        break;
      }
      case HS_DIAMOND:
        c = (stop < base.l) ? harmonyLerp((base.l - stop) / base.l, base.c, base.c * 0.5f)
                            : harmonyLerp((stop - base.l) / (1.0f - base.l), base.c, 0.0f);
        break;

      case HS_DEFAULT:   // the library's switch has no `default` arm, so these
      case HS_SQUARE:    // two land on the same untouched hue and chroma
      default:
        break;
    }

    out[i] = harmonyClampOklch(l, c, h);
  }
}

// ---------------------------------------------------------------------------
// Entry point — ColorPaletteGenerator.generate()
// ---------------------------------------------------------------------------
// Writes HARMONY_N colours. Returns false when `type` fell back to tintsShades
// because it isn't ported; callers that care can say so on the console.
static inline bool harmonyGenerate(const Oklch &base, HarmonyType type,
                                   HarmonyStyle style, Oklch *out) {
  generateTintsAndShades(base, style, out);
  return type == HARM_TINTS_SHADES;
}
