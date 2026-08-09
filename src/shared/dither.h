// ============================================================================
// dither.h — Atkinson error diffusion, ported byte-for-byte from the JS
// ============================================================================
// This is a port of sketchbook-ssam/src/dither.ts `dither_atkinson`, and it is
// deliberately a port of what that function *does* rather than what its
// options claim. The JS sketches call it as
//
//     dither(data, { greyscaleMethod: 'none', ditherMethod: 'atkinson' })
//
// which sets `drawColour = true`, and under `drawColour` the function:
//
//   * walks the RGBA buffer 4 bytes at a time, so `currentPixel` only ever
//     lands on a RED byte
//   * thresholds and error-diffuses that red byte
//   * skips the `data[i+1] = data[i+2] = data[i]` line that would copy the
//     result into green and blue
//
// Net effect: **the red channel becomes 1-bit, green and blue pass through
// untouched.** Not a greyscale dither, not a per-channel colour dither — a red
// separation with the other two channels riding along at full depth. That is
// the look the sketches have been tuned against, so it is what
// ditherAtkinsonPlane() reproduces: call it on the red plane only for the JS
// behaviour, on all three for the per-channel dither the option names imply.
//
// Two JS details are preserved on purpose:
//
//   * The error kernel is applied by flat index with no x bounds check, so
//     error pushed off the right edge lands on the left edge of the next row.
//     That wrap is visible in the output and is part of the look.
//   * Writes past the end of the buffer are dropped. JS gets that free from
//     typed arrays; here it is an explicit bounds check, because in C++ it is
//     a stack smash rather than a no-op.
//
// The JS loop also runs one iteration past the end (`currentPixel <=
// imageLength`), which reads `undefined`, computes a `NaN` error and writes
// nowhere. Not reproduced — it has no effect on the image.
// ============================================================================
#pragma once

#include <stdint.h>
#include "platform.h"     // W, cv

// Atkinson error diffusion over one 8-bit plane, in place.
//
//        X   1/8 1/8
//   1/8 1/8  1/8
//        1/8
//
// Only 6/8 of the error is propagated; the missing 2/8 is what gives Atkinson
// its high contrast compared to Floyd-Steinberg.
static void ditherAtkinsonPlane(uint8_t *p, int w, int h) {
  const int n      = w * h;
  const int off[6] = { 1, 2, w - 1, w, w + 1, 2 * w };

  for (int i = 0; i < n; i++) {
    const int v   = p[i];
    const int out = v <= 128 ? 0 : 255;
    const int err = (v - out) / 8;   // truncates toward zero, like parseInt()
    p[i] = (uint8_t)out;

    for (int k = 0; k < 6; k++) {
      const int j = i + off[k];
      if (j >= n) continue;          // JS drops these; here they would corrupt
      const int t = p[j] + err;      // Uint8ClampedArray semantics
      p[j] = (uint8_t)(t < 0 ? 0 : (t > 255 ? 255 : t));
    }
  }
}

// Blow a small image back up into the full-frame sprite with nearest-neighbour
// sampling — the C++ half of `drawImage` with `imageSmoothingEnabled = false`.
// Chunky is the point: without it the dither pattern is invisible at 172x320.
//
// Covers columns [0, sw*scale) and rows [0, sh*scale); anything past that is
// left alone, so pick a scale that divides W and H.
static void ditherExpandToSprite(const uint8_t *r, const uint8_t *g, const uint8_t *b,
                                 int sw, int sh, int scale) {
  static RGBColor row[W];

  for (int oy = 0; oy < sh; oy++) {
    const int base = oy * sw;
    int x = 0;
    for (int ox = 0; ox < sw; ox++) {
      const RGBColor c(r[base + ox], g[base + ox], b[base + ox]);
      for (int k = 0; k < scale; k++) row[x++] = c;
    }
    for (int k = 0; k < scale; k++) cv.pushImage(0, oy * scale + k, x, 1, row);
  }
}
