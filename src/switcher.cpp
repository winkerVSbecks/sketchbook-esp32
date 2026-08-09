// ============================================================================
// switcher.cpp — every sketch in one binary, BOOT cycles between them
// ============================================================================
// Press BOOT (GPIO0) to advance to the next sketch; it wraps around. On SDL,
// click the bottom strip of the window.
//
// The obstacle this file works around: every sketch in src/ is a complete
// translation unit with its own setup(), loop(), and a pile of file-scope
// statics — and they collide hard. All five hosted here define generate();
// four define LOOP_MS; two define rects[], at different types. That is
// exactly why platformio.ini gives each sketch a build_src_filter that
// compiles precisely one of them. Hosting several at once means solving those
// collisions.
//
// src/main.cpp is deliberately not in the roster: it is arc tiles on a timer,
// and arc_tiles_shake.cpp is the same composition with a shake trigger and an
// 8s timer of its own as the no-IMU fallback. It still builds standalone from
// the native/esp32/arc_shot envs.
//
// The fix is to give each sketch its own namespace, without editing any of
// them:
//
//   1. Include the shared/ modules here, at GLOBAL scope, first.
//   2. Include each sketch's .cpp inside `namespace sk_x { }`.
//
// Every header in shared/ is #pragma once, so when a sketch re-includes them
// from inside its namespace the second include is a no-op and the sketch binds
// to the global definitions from step 1. Everything platform.h declares is
// `static`, so step 1 also means there is exactly one `lcd`, one `cv`, and one
// 110KB framebuffer for all five sketches to share — rather than five of each,
// which would not fit in SRAM. Only the sketches' own symbols land inside the
// namespaces, which is precisely the set that was colliding.
//
// Sharing the framebuffer is not the whole RAM story: skeleton_line.cpp's 55KB
// coverage plane would, as a static, leave too little contiguous internal SRAM
// for that framebuffer to allocate. It takes psramAlloc() instead — see the
// note above `ab` in that file.
//
// The sketches are unmodified and their own envs still build them standalone.
//
//   pio run -e switcher_native -t exec    # SDL; click low in the window
//   pio run -e switcher_esp32 -t upload   # board; press BOOT
// ============================================================================
#define SKETCH_TITLE "switcher"

// Step 1 — shared modules at global scope. This is the union of what the five
// sketches include; the order does not matter, only that it happens first.
#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/harmony.h"
#include "shared/dither.h"
#include "shared/termfont.h"
#include "shared/imu.h"
#include "shared/noise.h"
#include "shared/subtractive.h"
#include <string.h>

// Step 2 — the sketches. SKETCH_TITLE/SKETCH_FRAMES are #undef'd ahead of each
// because every sketch defines them for platform.h, which has already been
// processed and consumed ours.
#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_shake {
#include "arc_tiles_shake.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_collapse {
#include "collapse.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_wave {
#include "trochoidal_wave.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_charts {
#include "terminal_charts.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_skeleton {
#include "skeleton_line.cpp"
}

// ---------------------------------------------------------------------------
// The roster
// ---------------------------------------------------------------------------
struct Sketch {
  const char *name;
  void      (*setup)();
  void      (*loop)();
};

static const Sketch SKETCHES[] = {
  { "arc tiles (shake)", sk_shake::setup,    sk_shake::loop    },
  { "collapse",          sk_collapse::setup, sk_collapse::loop },
  { "trochoidal wave",   sk_wave::setup,     sk_wave::loop     },
  { "terminal charts",   sk_charts::setup,   sk_charts::loop   },
  { "skeleton line",     sk_skeleton::setup, sk_skeleton::loop },
};
static const int N_SKETCHES = (int)(sizeof(SKETCHES) / sizeof(SKETCHES[0]));

static int active = 0;

// Hand control to a sketch by running its setup(), exactly as a standalone
// build would. Its panelBegin() call is idempotent, so the shared sprite
// survives the switch and only the screen is cleared.
//
// Sketches keep function-local statics across a switch (playhead cursors,
// frame counters). That is harmless: each one compares against millis(), so a
// stale cursor reads as "this composition is long overdue" and the sketch
// regenerates on its first frame back — which is what a fresh entry should do
// anyway.
static void enter(int i) {
  active = i;
  Serial.printf("\n======== [%d/%d] %s ========\n", i + 1, N_SKETCHES, SKETCHES[i].name);
  SKETCHES[i].setup();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  buttonBegin();

  Serial.printf("\nswitcher: %d sketches, press BOOT to cycle\n", N_SKETCHES);
  for (int i = 0; i < N_SKETCHES; i++) Serial.printf("  %d. %s\n", i + 1, SKETCHES[i].name);

  enter(0);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // A capture run has no button, so it would otherwise only ever show sketch 0
  // — and the thing worth checking about this file is that all five still
  // render correctly from inside their namespaces.
  //
  // Advance on frames OR iterations, not frames alone: arc_tiles_shake draws
  // once and then waits for a shake that a capture can never deliver, so a
  // frames-only trigger deadlocks on it. The iteration bound is the escape.
  static const int SHOT_FRAMES_EACH = 2;
  static const int SHOT_ITERS_MAX   = 200;
  static int       iters            = 0;
  static int       framesAtEntry    = 0;
  if (++iters >= SHOT_ITERS_MAX || _frameNo - framesAtEntry >= SHOT_FRAMES_EACH) {
    iters         = 0;
    framesAtEntry = _frameNo;
    enter((active + 1) % N_SKETCHES);
    return;
  }
#endif

  // Polled once per frame rather than on a timer. Every sketch yields within
  // 5-40ms, so the longest a press can go unnoticed is one frame.
  if (buttonPressed()) {
    enter((active + 1) % N_SKETCHES);
    return;
  }
  SKETCHES[active].loop();
}
