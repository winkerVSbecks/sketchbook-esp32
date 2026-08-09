// ============================================================================
// noise.h — 4D simplex noise, the field behind canvas-sketch-util's noise4D
// ============================================================================
// Ported from simplex-noise 2.4.0 (Jonas Wagner's packaging of Gustavson's
// reference implementation), which is what canvas-sketch-util/random wraps.
// The JS call
//
//     Random.noise4D(x, y, z, w, frequency, amplitude)
//
// is just `amplitude * simplex.noise4D(x*frequency, ...)`, so a ported sketch
// folds the frequency into its own coordinates and calls simplexNoise4D()
// directly. Output covers roughly [-1, 1].
//
// Seeding mirrors canvas-sketch-util: `Random.setSeed()` rebuilds the noise
// permutation table from the same stream the sketch then draws its own numbers
// from, so noiseSeed() must be called immediately after rngSeed() — it burns
// 255 rngNext() calls, and moving it changes every number that follows.
//
// The permutation is seeded from mulberry32 rather than ARC4, so the field is
// *a* simplex field with the same statistics, not the browser's field. See the
// note at the top of prng.h.
// ============================================================================
#pragma once

#include <math.h>
#include <stdint.h>
#include "prng.h"

static const float NOISE_F4 = 0.30901699437494745f;   // (sqrt(5) - 1) / 4
static const float NOISE_G4 = 0.13819660112501052f;   // (5 - sqrt(5)) / 20

// The 32 gradients of the 4D case: every vector with one zero component and
// the other three ±1.
static const int8_t NOISE_GRAD4[128] = {
   0,  1,  1,  1,    0,  1,  1, -1,    0,  1, -1,  1,    0,  1, -1, -1,
   0, -1,  1,  1,    0, -1,  1, -1,    0, -1, -1,  1,    0, -1, -1, -1,
   1,  0,  1,  1,    1,  0,  1, -1,    1,  0, -1,  1,    1,  0, -1, -1,
  -1,  0,  1,  1,   -1,  0,  1, -1,   -1,  0, -1,  1,   -1,  0, -1, -1,
   1,  1,  0,  1,    1,  1,  0, -1,    1, -1,  0,  1,    1, -1,  0, -1,
  -1,  1,  0,  1,   -1,  1,  0, -1,   -1, -1,  0,  1,   -1, -1,  0, -1,
   1,  1,  1,  0,    1,  1, -1,  0,    1, -1,  1,  0,    1, -1, -1,  0,
  -1,  1,  1,  0,   -1,  1, -1,  0,   -1, -1,  1,  0,   -1, -1, -1,  0,
};

// Doubled so the four nested lookups can add +1 to any index without wrapping.
static uint8_t noisePerm[512];

// Fisher-Yates over 0..255, exactly the JS buildPermutationTable walk.
static inline void noiseSeed() {
  uint8_t p[256];
  for (int i = 0; i < 256; i++) p[i] = (uint8_t)i;
  for (int i = 0; i < 255; i++) {
    int r = i + (int)(rngNext() * (float)(256 - i));
    if (r > 255) r = 255;                    // rngNext() < 1, but round anyway
    const uint8_t t = p[i];
    p[i] = p[r];
    p[r] = t;
  }
  for (int i = 0; i < 512; i++) noisePerm[i] = p[i & 255];
}

static inline float _noiseCorner(int gi, float x, float y, float z, float w) {
  const int8_t *g = &NOISE_GRAD4[gi];
  return g[0] * x + g[1] * y + g[2] * z + g[3] * w;
}

static float simplexNoise4D(float x, float y, float z, float w) {
  const uint8_t *perm = noisePerm;

  // Skew into the lattice of 24-cell simplices and find the cell origin.
  const float s = (x + y + z + w) * NOISE_F4;
  const int   i = (int)floorf(x + s);
  const int   j = (int)floorf(y + s);
  const int   k = (int)floorf(z + s);
  const int   l = (int)floorf(w + s);
  const float t = (float)(i + j + k + l) * NOISE_G4;

  const float x0 = x - ((float)i - t);
  const float y0 = y - ((float)j - t);
  const float z0 = z - ((float)k - t);
  const float w0 = w - ((float)l - t);

  // Rank the four offsets by magnitude; the ordering picks one of 24 simplices
  // and the corner offsets fall out of the ranks by thresholding.
  int rx = 0, ry = 0, rz = 0, rw = 0;
  if (x0 > y0) rx++; else ry++;
  if (x0 > z0) rx++; else rz++;
  if (x0 > w0) rx++; else rw++;
  if (y0 > z0) ry++; else rz++;
  if (y0 > w0) ry++; else rw++;
  if (z0 > w0) rz++; else rw++;

  const int i1 = rx >= 3, j1 = ry >= 3, k1 = rz >= 3, l1 = rw >= 3;
  const int i2 = rx >= 2, j2 = ry >= 2, k2 = rz >= 2, l2 = rw >= 2;
  const int i3 = rx >= 1, j3 = ry >= 1, k3 = rz >= 1, l3 = rw >= 1;

  const float g1 = NOISE_G4, g2 = 2.0f * NOISE_G4;
  const float g3 = 3.0f * NOISE_G4, g4 = 4.0f * NOISE_G4;

  const float x1 = x0 - i1 + g1, y1 = y0 - j1 + g1, z1 = z0 - k1 + g1, w1 = w0 - l1 + g1;
  const float x2 = x0 - i2 + g2, y2 = y0 - j2 + g2, z2 = z0 - k2 + g2, w2 = w0 - l2 + g2;
  const float x3 = x0 - i3 + g3, y3 = y0 - j3 + g3, z3 = z0 - k3 + g3, w3 = w0 - l3 + g3;
  const float x4 = x0 - 1.0f + g4, y4 = y0 - 1.0f + g4;
  const float z4 = z0 - 1.0f + g4, w4 = w0 - 1.0f + g4;

  const int ii = i & 255, jj = j & 255, kk = k & 255, ll = l & 255;

  float n = 0.0f;
  float t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0 - w0 * w0;
  if (t0 > 0.0f) {
    const int gi = (perm[ii + perm[jj + perm[kk + perm[ll]]]] % 32) * 4;
    t0 *= t0;
    n += t0 * t0 * _noiseCorner(gi, x0, y0, z0, w0);
  }
  float t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1 - w1 * w1;
  if (t1 > 0.0f) {
    const int gi = (perm[ii + i1 + perm[jj + j1 + perm[kk + k1 + perm[ll + l1]]]] % 32) * 4;
    t1 *= t1;
    n += t1 * t1 * _noiseCorner(gi, x1, y1, z1, w1);
  }
  float t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2 - w2 * w2;
  if (t2 > 0.0f) {
    const int gi = (perm[ii + i2 + perm[jj + j2 + perm[kk + k2 + perm[ll + l2]]]] % 32) * 4;
    t2 *= t2;
    n += t2 * t2 * _noiseCorner(gi, x2, y2, z2, w2);
  }
  float t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3 - w3 * w3;
  if (t3 > 0.0f) {
    const int gi = (perm[ii + i3 + perm[jj + j3 + perm[kk + k3 + perm[ll + l3]]]] % 32) * 4;
    t3 *= t3;
    n += t3 * t3 * _noiseCorner(gi, x3, y3, z3, w3);
  }
  float t4 = 0.6f - x4 * x4 - y4 * y4 - z4 * z4 - w4 * w4;
  if (t4 > 0.0f) {
    const int gi = (perm[ii + 1 + perm[jj + 1 + perm[kk + 1 + perm[ll + 1]]]] % 32) * 4;
    t4 *= t4;
    n += t4 * t4 * _noiseCorner(gi, x4, y4, z4, w4);
  }
  return 27.0f * n;
}
