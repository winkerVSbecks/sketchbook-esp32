// ============================================================================
// touch.h — tap gesture from a capacitive touch controller, where a board
// has one
// ============================================================================
// The board header supplies TOUCH_I2C_ADDR (-1 = no touch controller); the
// controller shares the IMU's I2C bus, and like imu.h this drives it through
// Wire directly — LovyanGFX's Touch_* classes use their own I2C layer, and
// two drivers on one bus fight.
//
// FT3168 protocol (from the demo package's Arduino_FT3x68): finger count in
// the low nibble of register 0x02, no init writes required. That is all a
// tap needs — coordinates stay unread until a sketch wants them.
//
//   touchBegin()     probe; false means no tap gesture on this board
//   tapDetected()    one event per press, debounced — the finger going down,
//                    not a full down-up cycle
//   touchPoint(&x,&y) true while a finger is down, with its position in panel
//                    coordinates. The FT3x68 and CST816 families share the
//                    same register layout (0x02 count, 0x03..0x06 X/Y), so
//                    one burst read serves both boards.
//
// tapDetected() deliberately has no SDL stand-in: clicks are already spoken
// for (imu.h reads any click as a shake, platform.h reads the bottom strip as
// BOOT), and on the desktop reseeding is relaunching the binary.
//
// touchPoint() DOES have one — the mouse while its button is held — because a
// touch-driven sketch can't be iterated on at all without it. A sketch built
// on touchPoint() therefore shouldn't also include imu.h's click-as-shake or
// poll buttonPressed()'s bottom strip, and should derive its own tap gesture
// from press/release timing rather than mixing in tapDetected().
// ============================================================================
#pragma once

#include "platform.h"

#if defined(ARDUINO)

#include <Wire.h>

static bool _touchUp = false;

static inline bool touchBegin() {
  if (TOUCH_I2C_ADDR < 0) return false;
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, 400000);   // shared bus; re-begin is a no-op
  Wire.beginTransmission((uint8_t)TOUCH_I2C_ADDR);
  const bool acked = Wire.endTransmission() == 0;
  // CST816-family controllers auto-sleep and NACK while untouched, so on a
  // board that carries one a failed boot-time probe means "asleep", not
  // "absent" — stay armed and let tapDetected()'s reads settle it.
  _touchUp = acked || TOUCH_NACKS_WHEN_IDLE;
  if (acked)          Serial.printf("touch at 0x%02X\n", TOUCH_I2C_ADDR);
  else if (_touchUp)  Serial.printf("touch at 0x%02X asleep; polling anyway\n", TOUCH_I2C_ADDR);
  else                Serial.printf("no touch controller at 0x%02X\n", TOUCH_I2C_ADDR);
  return _touchUp;
}

static inline bool tapDetected() {
  if (!_touchUp) return false;

  static bool     was  = false;
  static uint32_t last = 0;

  Wire.beginTransmission((uint8_t)TOUCH_I2C_ADDR);
  Wire.write(0x02);                               // FT3x68 finger count
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 1) != 1) return false;
  const bool down = (Wire.read() & 0x0F) > 0;

  const bool edge = down && !was;
  was = down;
  if (!edge) return false;

  const uint32_t now = millis();
  if (now - last < 400) return false;
  last = now;
  return true;
}

// Finger position, in panel coordinates. Returns true while a finger is down.
// One burst read of registers 0x02..0x06: touch count, then XH/XL/YH/YL with
// the high nibbles masked — the same layout on the FT3x68 (a18) and the
// CST816D (t2). A NACK means "no finger" on an auto-sleeping CST816, so it
// reads as up rather than as an error.
static inline bool touchPoint(int *x, int *y) {
  if (!_touchUp) return false;

  Wire.beginTransmission((uint8_t)TOUCH_I2C_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 5) != 5) return false;

  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();
  if ((b[0] & 0x0F) == 0) return false;

  int px = ((b[1] & 0x0F) << 8) | b[2];
  int py = ((b[3] & 0x0F) << 8) | b[4];
  if (px < 0) px = 0; if (px >= W) px = W - 1;
  if (py < 0) py = 0; if (py >= H) py = H - 1;
  *x = px;
  *y = py;
  return true;
}

#else

static inline bool touchBegin()  { return false; }
static inline bool tapDetected() { return false; }

#if defined(SKETCH_HEADLESS)

// No window, no mouse — headless captures render a sketch's untouched state.
static inline bool touchPoint(int *, int *) { return false; }

#else

// SDL stand-in: the mouse while its button is held. Panel_sdl's getTouch()
// already maps the window's 2x scaling back to panel coordinates.
static inline bool touchPoint(int *x, int *y) {
  int32_t tx, ty;
  if (!lcd.getTouch(&tx, &ty)) return false;
  *x = (int)tx;
  *y = (int)ty;
  return true;
}

#endif

#endif
