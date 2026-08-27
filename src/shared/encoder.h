// ============================================================================
// encoder.h — cumulative rotation from a quadrature knob, where a board
// has one
// ============================================================================
// The board header supplies PIN_ENC_A / PIN_ENC_B and ENC_COUNTS_PER_REV
// (quadrature edge count for one full mechanical revolution — detents x 4 on
// the usual full-cycle-per-detent encoders). A board without a knob simply
// doesn't define them, and a sketch that includes this header fails to
// compile for that board — which is the support matrix working as intended,
// same as touch.h's TOUCH_I2C_ADDR = -1 but stricter, because a knob sketch
// without a knob isn't degraded, it's inert.
//
//   encoderBegin()   attach the decoder; call once from setup()
//   encoderCount()   cumulative signed quadrature count since boot
//   encoderRev()     the same, as float revolutions (count / ENC_COUNTS_PER_REV)
//
// Boards with a knob also wire a full-press switch under it (PIN_KNOB_BTN,
// idles high, pressed = LOW — deliberately not PIN_BOOT), so this header
// carries that too:
//
//   knobButtonBegin()  pin setup; call once from setup()
//   knobPressed()      one event per press, debounced — buttonPressed()'s
//                      contract, on the knob's switch instead of BOOT
//
// Like the encoder pins, PIN_KNOB_BTN only exists on knob boards, so a sketch
// reading the press fails to compile anywhere else — support matrix by
// compile error again.
//
// The decode is interrupt-driven, not polled: a render pass on these boards
// is tens of milliseconds, and a quick flick of the knob produces edges a
// per-frame poll would miss — dropped edges on a state-table decoder don't
// just lose steps, they can count the wrong direction. The ISR is IRAM and
// reads the GPIO input registers directly rather than through digitalRead(),
// so it stays valid even in the windows where flash cache is off (NVS
// writes).
//
// SDL stand-in: a vertical mouse drag, one window height per revolution —
// polled inside encoderRev(), so call it every loop. A sketch built on this
// shouldn't also read clicks as anything else (imu.h's click-as-shake,
// buttonPressed()'s bottom strip); the mouse is the knob. The one exception
// is knobPressed(), whose stand-in is a click that *starts* in the bottom
// strip of the window — the same strip-latch logic as buttonPressed(), just
// on the knob's symbol, because that symbol belongs to BOOT. So on SDL:
// drag = knob, bottom-strip click = knob press. Both polls call
// lcd.getTouch() independently, which is fine — it's a stateless query, and
// buttonPressed()/imu.h already coexist the same way — but each keeps its
// own static edge state. Headless builds read 0 forever (and knobPressed()
// false) and sweep their playhead themselves, exactly as zhi.cpp's capture
// path does for touch.
// ============================================================================
#pragma once

#include "platform.h"

#if defined(ARDUINO)

#include <soc/gpio_reg.h>

static volatile int32_t _encCount = 0;
static volatile uint8_t _encPrev  = 0;

// Direct input-register read, safe from an IRAM ISR (the S3 splits the input
// bits across two registers at GPIO32).
static inline uint32_t IRAM_ATTR _encLevel(int pin) {
  return pin < 32 ? (REG_READ(GPIO_IN_REG)  >> pin)        & 1u
                  : (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1u;
}

// Classic 4x quadrature state table, indexed by (previous AB << 2) | new AB.
// Invalid transitions (both lines flipping in one step) count 0 instead of
// guessing, so contact bounce degrades to a missed edge, not a reversal.
static const int8_t _ENC_STEP[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0,
};

static void IRAM_ATTR _encIsr() {
  const uint8_t now = (uint8_t)((_encLevel(PIN_ENC_A) << 1) | _encLevel(PIN_ENC_B));
  _encCount += _ENC_STEP[(_encPrev << 2) | now];
  _encPrev = now;
}

static inline void encoderBegin() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  _encPrev = (uint8_t)((_encLevel(PIN_ENC_A) << 1) | _encLevel(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), _encIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), _encIsr, CHANGE);
}

static inline int32_t encoderCount() {
  // A 32-bit aligned load is atomic on the Xtensa, so no critical section.
  return _encCount;
}

static inline float encoderRev() {
  return (float)encoderCount() / (float)ENC_COUNTS_PER_REV;
}

// The full-press: buttonPressed()'s falling-edge + debounce, verbatim, on
// PIN_KNOB_BTN. Polled, not interrupt — a press is a human-speed event.
static inline void knobButtonBegin() {
  pinMode(PIN_KNOB_BTN, INPUT_PULLUP);
}

static inline bool knobPressed() {
  static bool     was  = false;
  static uint32_t last = 0;
  const bool down = digitalRead(PIN_KNOB_BTN) == LOW;
  const bool edge = down && !was;
  was = down;
  if (!edge) return false;
  const uint32_t now = millis();
  if (now - last < BUTTON_DEBOUNCE_MS) return false;
  last = now;
  return true;
}

#elif defined(SKETCH_HEADLESS)

static inline void    encoderBegin() {}
static inline int32_t encoderCount() { return 0; }
static inline float   encoderRev()   { return 0.0f; }

static inline void knobButtonBegin() {}
static inline bool knobPressed()     { return false; }

#else

// SDL: the mouse while its button is held is the knob. Relative, not
// absolute — dragging accumulates rotation, so the knob's "keeps going past
// one turn" nature survives on the desktop. One window height of drag is one
// revolution.
static int32_t _encAccumPx = 0;

static inline void encoderBegin() {}

static inline int32_t encoderCount() {
  static bool    was  = false;
  static int32_t lastY = 0;
  int32_t tx, ty;
  const bool down = lcd.getTouch(&tx, &ty);
  if (down && was) _encAccumPx += ty - lastY;
  lastY = ty;
  was   = down;
  return _encAccumPx;
}

static inline float encoderRev() {
  return (float)encoderCount() / (float)H;
}

// The press stand-in: a click that *starts* in the bottom strip. This is
// buttonPressed()'s strip-latch logic replicated rather than called — that
// symbol belongs to BOOT, and a binary could host both gestures. The latch
// matters here too: the drag-as-knob can wander through the strip mid-turn,
// and without it that wander would read as a press.
static inline bool knobPressed() {
  static bool     was  = false;
  static uint32_t last = 0;
  static bool     rawWas = false, fromStrip = false;
  int32_t tx, ty;
  const bool rawDown = lcd.getTouch(&tx, &ty);
  if (rawDown && !rawWas) fromStrip = ty >= H - BUTTON_SDL_STRIP_H;
  rawWas = rawDown;
  const bool down = rawDown && fromStrip;
  const bool edge = down && !was;
  was = down;
  if (!edge) return false;
  const uint32_t now = millis();
  if (now - last < BUTTON_DEBOUNCE_MS) return false;
  last = now;
  return true;
}

static inline void knobButtonBegin() {}

#endif
