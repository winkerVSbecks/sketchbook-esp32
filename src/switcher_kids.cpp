// ============================================================================
// switcher_kids.cpp — the two touch toys in one binary, BOOT swaps between them
// ============================================================================
// A roster for small hands: paint (drag draws, tap changes the crayon) and zhi
// (finger scrubs the breathing checkerboards, tap deals a new one). Both
// sketches live entirely on the glass, which forces the one design difference
// from switcher.cpp:
//
//   BOOT (GPIO0)   swap to the other sketch. On SDL, click the bottom strip.
//   RESET          reboot into the *same* sketch, fresh composition.
//   the glass      belongs to the active sketch, untouched by this file.
//
// The main switcher also reads a tap as "reseed" — that would be a bug here.
// Paint's tap cycles the brush color; a switcher-level tapDetected() racing
// paint's own press/release timing would wipe a kid's drawing on every crayon
// change. So this file polls only the button and forwards every frame to the
// sketch; each sketch's setup() already calls touchBegin() itself.
//
// Swapping away from paint discards the drawing — the incoming setup() paints
// over the shared framebuffer, and coming back re-runs paint's setup(), which
// starts a fresh canvas by design. That is the roster model working as
// documented, not a leak; if it stings, don't press BOOT mid-masterpiece.
//
// The roster position persists under its own NVS key ("kidsketch", not the
// main switcher's "sketch") so the two apps don't resume into each other's
// index when reflashed over one another.
//
// Hosting works exactly as switcher.cpp documents: shared headers included at
// global scope first (the union of what the two sketches use), then each
// sketch's .cpp inside its own namespace. Both are unmodified and still build
// standalone from their own envs.
//
//   pio run -e kids_t2_native -t exec    # SDL; drag/click = finger, bottom strip = BOOT
//   pio run -e kids_t2 -t upload         # board; BOOT swaps, RESET reseeds
// ============================================================================
#define SKETCH_TITLE  "kids"
#define SKETCH_FRAMES 4

// Step 1 — shared modules at global scope: the union of the two sketches'
// includes, so their own re-includes are no-ops and both bind to one lcd, one
// cv, one framebuffer.
#include "shared/platform.h"
#include "shared/prng.h"
#include "shared/color.h"
#include "shared/palettes.h"
#include "shared/touch.h"

// Step 2 — the sketches, each in a namespace. SKETCH_TITLE/SKETCH_FRAMES are
// #undef'd first because both define them for platform.h, which has already
// been processed and consumed ours.
#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_paint {
#include "paint.cpp"
}

#undef SKETCH_TITLE
#undef SKETCH_FRAMES
namespace sk_zhi {
#include "zhi.cpp"
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
  { "paint", sk_paint::setup, sk_paint::loop },
  { "zhi",   sk_zhi::setup,   sk_zhi::loop   },
};
static const int N_SKETCHES = (int)(sizeof(SKETCHES) / sizeof(SKETCHES[0]));

static int active = 0;

// Run the incoming sketch's setup(), exactly as a standalone build would. Its
// panelBegin() is idempotent, its touchBegin() re-begin is a no-op, and it
// draws fresh from newSeed() — so every entry is a new composition.
static void enter(int i) {
  active = i;
  Serial.printf("\n======== [%d/%d] %s ========\n", i + 1, N_SKETCHES, SKETCHES[i].name);
  SKETCHES[i].setup();
}

// Advance and write the position down, so RESET resumes here. Flash, once per
// press — see persistPut in platform.h.
static void advance() {
  const int next = (active + 1) % N_SKETCHES;
  persistPut("kidsketch", (uint32_t)next);
  enter(next);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  buttonBegin();

  Serial.printf("\nkids: %d sketches, BOOT swaps, RESET reseeds, glass is the sketch's\n",
                N_SKETCHES);
  for (int i = 0; i < N_SKETCHES; i++) Serial.printf("  %d. %s\n", i + 1, SKETCHES[i].name);

  // Clamped, not trusted: the stored index outlives the binary that wrote it.
  const int saved = (int)persistGet("kidsketch", 0);
  enter(saved >= 0 && saved < N_SKETCHES ? saved : 0);
}

void loop() {
#if defined(SKETCH_HEADLESS)
  // A capture has no button, so cycle on frames or iterations — the thing
  // worth checking is that both sketches still render from inside their
  // namespaces. Paint only presents when its scripted gesture marks the
  // canvas, so the iteration bound is the escape if a phase of the script
  // goes quiet.
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

  // The button is the only input this file owns. Everything on the glass —
  // paint's tap-vs-drag timer, zhi's tap-vs-scrub slop — is the sketch's.
  if (buttonPressed()) {
    advance();
    return;
  }
  SKETCHES[active].loop();
}
