// ============================================================================
// switcher_knob.cpp — the knob sketches in one binary, the knob's
// full-press cycles between them
// ============================================================================
// A roster for the rotary board: zhi knob (breathing rect grids), isolines
// (chromatic noise contours), circle moire (knob-slid stripe rings), chill
// wave (paper squiggle on riso ink). All of them live on the ring and the
// glass, which forces the design difference from switcher.cpp:
//
//   knob press (GPIO41)  next sketch, wrapping. On SDL, click the bottom
//                        strip of the window (the drag is already the knob).
//   RESET                reboot into the *same* sketch, fresh composition.
//   the glass            belongs to the active sketch, untouched by this
//                        file — every sketch but moire reads a tap as
//                        reseed, so a switcher-level tapDetected() here
//                        would race theirs and eat every other reseed. Same
//                        reasoning as switcher_kids.cpp, different gesture.
//   the ring             also the sketch's: each one reads encoderRev() as
//                        its own playhead.
//
// BOOT exists on this board but sits on the underside — the knob press is the
// switch gesture because it's the one control your hand is already on. No
// hosted sketch reads the press, so it's free. knobPressed() lives in
// encoder.h (not here): only knob boards define PIN_KNOB_BTN, so a non-knob
// board fails the compile, which is the support matrix working.
//
// The roster position persists under its own NVS key ("rkswitch", not
// "sketch" or "kidsketch") so this app and the others don't resume into each
// other's index when reflashed over one another.
//
// Hosting works exactly as switcher.cpp documents: shared headers included at
// global scope first (the union of what the sketches use), then each
// sketch's .cpp inside its own namespace — all of them define generate(),
// renderAll(), playhead, knobPlayhead(); the namespaces are what keep those
// apart while everything binds to one lcd, one cv, one 115KB framebuffer.
// The sketches are unmodified and still build standalone from their own envs.
//
//   pio run -e switcher_rk_native -t exec   # SDL; drag = knob, bottom strip = next
//   pio run -e switcher_rk -t upload        # board; knob press cycles, RESET reseeds
// ============================================================================
#define SKETCH_TITLE  "knob switcher"
#define SKETCH_FRAMES 8

// Step 1 — shared modules at global scope: the union of the sketches'
// includes, so their own re-includes are no-ops and they all bind to the one
// set of globals.
#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/noise.h"
#include "shared/color.h"
#include "shared/touch.h"
#include "shared/encoder.h"

// Step 2 — the sketches, each in a namespace. SKETCH_TITLE/SKETCH_FRAMES are
// #undef'd first because each defines them for platform.h, which has already
// been processed and consumed ours.
#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_zhi {
#include "zhi_knob.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_iso {
#include "isolines.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_moire {
#include "circle_moire.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_chill {
#include "chill_wave.cpp"
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
  { "zhi knob",     sk_zhi::setup,   sk_zhi::loop   },
  { "isolines",     sk_iso::setup,   sk_iso::loop   },
  { "circle moire", sk_moire::setup, sk_moire::loop },
  { "chill wave",   sk_chill::setup, sk_chill::loop },
};
static const int N_SKETCHES = (int)(sizeof(SKETCHES) / sizeof(SKETCHES[0]));

static int active = 0;

// Run the incoming sketch's setup(), exactly as a standalone build would. Its
// panelBegin() is idempotent, its encoderBegin()/touchBegin() re-begins are
// no-ops, and it draws fresh from newSeed() — so every entry is a new
// composition (except moire, which has no randomness and says so).
static void enter(int i) {
  active = i;
  Serial.printf("\n======== [%d/%d] %s ========\n", i + 1, N_SKETCHES, SKETCHES[i].name);
  SKETCHES[i].setup();
}

// Advance and write the position down, so RESET resumes here. Flash, once per
// press — see persistPut in platform.h.
static void advance() {
  const int next = (active + 1) % N_SKETCHES;
  persistPut("rkswitch", (uint32_t)next);
  enter(next);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  knobButtonBegin();

  Serial.printf("\nknob switcher: %d sketches, knob press cycles, RESET reseeds, "
                "ring and glass are the sketch's\n", N_SKETCHES);
  for (int i = 0; i < N_SKETCHES; i++) Serial.printf("  %d. %s\n", i + 1, SKETCHES[i].name);

  // Clamped, not trusted: the stored index outlives the binary that wrote it.
  const int saved = (int)persistGet("rkswitch", 0);
  enter(saved >= 0 && saved < N_SKETCHES ? saved : 0);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // A capture has no knob, so cycle on frames or iterations — the thing worth
  // checking is that all three sketches still render from inside their
  // namespaces. Each one presents every headless loop (they sweep their own
  // playheads), so the frame count is the real driver; the iteration bound is
  // the escape if one ever goes quiet.
  static const int SHOT_FRAMES_EACH = 2;
  static const int SHOT_ITERS_MAX   = 200;
  static int       iters            = 0;
  static int       framesAtEntry    = 0;
  if (++iters >= SHOT_ITERS_MAX || _frameNo - framesAtEntry >= SHOT_FRAMES_EACH) {
    iters         = 0;
    framesAtEntry = _frameNo;
    advance();
    return;
  }
#endif

  // The press is the only input this file owns. The ring and the glass —
  // each sketch's playhead read, zhi's and iso's tap-as-reseed — are the
  // sketch's.
  if (knobPressed()) {
    advance();
    return;
  }
  SKETCHES[active].loop();
}
