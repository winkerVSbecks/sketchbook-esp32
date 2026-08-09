// ============================================================================
// prng.h — mulberry32, with the canvas-sketch-util/random surface
// ============================================================================
// The JS sketches use canvas-sketch-util/random, which is ARC4 (seedrandom).
// Reimplementing ARC4 here isn't worth it, so seeds don't carry across by
// default. If a piece needs to reproduce bit-for-bit between browser and
// board, put mulberry32 in the JS sketch — six lines there beats a few hundred
// here.
//
// The helper names mirror the JS API deliberately (`rngRangeFloor` for
// `Random.rangeFloor`, and so on) so a ported line reads like its original.
// ============================================================================
#pragma once

#include <stdint.h>

static uint32_t rngState = 1;

static inline void rngSeed(uint32_t s) { rngState = s ? s : 1; }

// Random.value() — [0, 1)
static inline float rngNext() {
  rngState += 0x6D2B79F5u;
  uint32_t t = rngState;
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return (float)((t ^ (t >> 14)) >> 8) / 16777216.0f;
}

// Random.range(min, max)
static inline float rngRange(float lo, float hi) { return lo + (hi - lo) * rngNext(); }

// Random.rangeFloor(min, max) — [min, max)
static inline int rngRangeFloor(int lo, int hi) {
  if (hi <= lo) return lo;
  int v = lo + (int)(rngNext() * (float)(hi - lo));
  return v >= hi ? hi - 1 : v;
}

// Index into an n-element array.
static inline int rngIndex(int n) {
  int i = (int)(rngNext() * (float)n);
  return i >= n ? n - 1 : i;
}

// Random.chance(p)
static inline bool rngChance(float p = 0.5f) { return rngNext() < p; }

// Random.boolean()
static inline bool rngBoolean() { return rngNext() > 0.5f; }

// Random.sign()
static inline int rngSign() { return rngBoolean() ? 1 : -1; }

// Random.pick(array)
template <typename T>
static inline T rngPick(const T *a, int n) { return a[rngIndex(n)]; }

// Random.shuffle(array) — in place, Fisher-Yates, same walk order as the JS.
template <typename T>
static inline void rngShuffle(T *a, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = rngIndex(i + 1);
    T t = a[i]; a[i] = a[j]; a[j] = t;
  }
}
