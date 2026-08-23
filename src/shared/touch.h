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
//
// There is deliberately no SDL stand-in: clicks are already spoken for
// (imu.h reads any click as a shake, platform.h reads the bottom strip as
// BOOT), and on the desktop reseeding is relaunching the binary.
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
  _touchUp = Wire.endTransmission() == 0;
  if (_touchUp) Serial.printf("touch at 0x%02X\n", TOUCH_I2C_ADDR);
  else          Serial.printf("no touch controller at 0x%02X\n", TOUCH_I2C_ADDR);
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

#else

static inline bool touchBegin()  { return false; }
static inline bool tapDetected() { return false; }

#endif
