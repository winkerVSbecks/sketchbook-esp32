// ============================================================================
// imu.h — shake gesture from the QMI8658, with a click stand-in on SDL
// ============================================================================
// Device: the QMI8658 6-axis IMU over I2C, on the pins the board header
// supplies (PIN_IMU_SDA / PIN_IMU_SCL — provenance and warnings live there,
// next to the numbers).
//
// Desktop: click the window.
//
//   imuBegin()          probe and configure; false means no shake gesture at
//                       all, so RESET is the only way to a new composition
//   shakeDetected(g)    true on a peak above `g` total acceleration
//   imuShakeDetected(g) the same, minus the SDL click stand-in — for sketches
//                       built on touchPoint(), whose clicks are already the
//                       finger. No desktop stand-in at all (the tapDetected()
//                       precedent): on SDL, relaunch the binary instead
//   imuTilt(gx, gy)     the gravity vector's screen-plane components, in g
// ============================================================================
#pragma once

#include "platform.h"

// Tilt fallback when there is no live accelerometer (probe failed, or a
// headless run): a slow deterministic circle, so tilt-driven pieces drift
// rather than freeze — and headless captures actually show the motion, spread
// across the virtual clock.
static const uint32_t TILT_DRIFT_MS = 8000;
static const float    TILT_DRIFT_G  = 0.35f;

static inline void _tiltDrift(float &gx, float &gy) {
  const float ph = (float)(millis() % TILT_DRIFT_MS) * (6.2831853f / (float)TILT_DRIFT_MS);
  gx = TILT_DRIFT_G * cosf(ph);
  gy = TILT_DRIFT_G * sinf(ph);
}

#if defined(ARDUINO)

#include <Wire.h>

static const uint8_t QMI_WHO_AM_I = 0x00;   // reads 0x05
static const uint8_t QMI_CTRL1    = 0x02;
static const uint8_t QMI_CTRL2    = 0x03;
static const uint8_t QMI_CTRL7    = 0x08;
static const uint8_t QMI_RESET    = 0x60;   // write 0xB0: soft reset
static const uint8_t QMI_AX_L     = 0x35;

static const float ACCEL_LSB_PER_G = 4096.0f;   // at +/-8g full scale

static uint8_t imuAddr = 0;

static bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool imuRead(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)imuAddr, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool imuBegin() {
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, 400000);

  const uint8_t candidates[2] = { 0x6B, 0x6A };   // SA0 pulled high / low
  for (int i = 0; i < 2; i++) {
    imuAddr = candidates[i];
    uint8_t who = 0;
    if (imuRead(QMI_WHO_AM_I, &who, 1) && who == 0x05) {
      // Soft reset before configuring — Waveshare's SensorLib always does
      // this, and on the AMOLED-1.8 it is load-bearing: without it the part
      // acks every read but its output registers never update (found frozen
      // on that board; the 1.47B happened not to care).
      imuWrite(QMI_RESET, 0xB0);
      delay(20);
      imuWrite(QMI_CTRL1, 0x40);   // auto-increment register address, little endian
      imuWrite(QMI_CTRL2, 0x25);   // accel +/-8g, 250Hz output rate
      imuWrite(QMI_CTRL7, 0x01);   // accel on, gyro off
      delay(20);
      Serial.printf("QMI8658 at 0x%02X\n", imuAddr);
      return true;
    }
  }

  // Nothing answered at either address. Scan the bus so the log says whether
  // the part is at an unexpected address or the pins are wrong entirely.
  imuAddr = 0;
  Serial.printf("QMI8658 not found - scanning SDA=%d SCL=%d: ", PIN_IMU_SDA, PIN_IMU_SCL);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("0x%02X ", a); found++; }
  }
  Serial.println(found ? "" : "(bus empty)");
  return false;
}

static bool shakeDetected(float shakeG) {
  if (!imuAddr) return false;

  uint8_t raw[6];
  if (!imuRead(QMI_AX_L, raw, 6)) return false;

  const float ax = (int16_t)(raw[0] | (raw[1] << 8)) / ACCEL_LSB_PER_G;
  const float ay = (int16_t)(raw[2] | (raw[3] << 8)) / ACCEL_LSB_PER_G;
  const float az = (int16_t)(raw[4] | (raw[5] << 8)) / ACCEL_LSB_PER_G;
  const float g  = sqrtf(ax * ax + ay * ay + az * az);

  if (g <= shakeG) return false;
  Serial.printf("shake %.2fg\n", g);
  return true;
}

// On device the click stand-in doesn't exist, so this IS shakeDetected.
static inline bool imuShakeDetected(float shakeG) { return shakeDetected(shakeG); }

// gx > 0 when the panel's right edge dips, gy > 0 when its bottom (USB) edge
// dips; both ~0 lying face-up, gy ~1 standing upright. The mapping below takes
// the QMI8658's X/Y axes straight — it has not been sighted on hardware yet,
// so if parallax tracks the wrong axis or runs backwards, flip these signs
// here, not in a sketch.
static const float TILT_SIGN_X = 1.0f;
static const float TILT_SIGN_Y = 1.0f;

static void imuTilt(float &gx, float &gy) {
  if (imuAddr) {
    uint8_t raw[6];
    if (imuRead(QMI_AX_L, raw, 6)) {
      gx = TILT_SIGN_X * (float)(int16_t)(raw[0] | (raw[1] << 8)) / ACCEL_LSB_PER_G;
      gy = TILT_SIGN_Y * (float)(int16_t)(raw[2] | (raw[3] << 8)) / ACCEL_LSB_PER_G;
      return;
    }
  }
  _tiltDrift(gx, gy);
}

#else

static bool imuBegin() { return true; }

// Mouse-down edge, so a held button doesn't machine-gun new seeds. Headless
// builds have no window to click, so they never fire — a capture is one
// composition, the one setup() drew.
static bool shakeDetected(float) {
#if defined(SKETCH_HEADLESS)
  return false;
#else
  static bool wasDown = false;
  int32_t tx, ty;
  const bool down  = lcd.getTouch(&tx, &ty);
  const bool fired = down && !wasDown;
  wasDown = down;
  return fired;
#endif
}

// No stand-in, deliberately: a touchPoint() sketch's clicks are its finger,
// so the click-as-shake above would fire on every stroke.
static inline bool imuShakeDetected(float) { return false; }

// Drag stands in for tilt: hold the mouse and pull away from the window
// centre; released, the tilt eases back to level. Headless has no window, so
// it drifts on the virtual clock instead.
static const float TILT_SDL_G = 0.6f;

static void imuTilt(float &gx, float &gy) {
#if defined(SKETCH_HEADLESS)
  _tiltDrift(gx, gy);
#else
  static float hx = 0.0f, hy = 0.0f;
  int32_t tx, ty;
  if (lcd.getTouch(&tx, &ty)) {
    hx = ((float)tx - W * 0.5f) / (W * 0.5f) * TILT_SDL_G;
    hy = ((float)ty - H * 0.5f) / (H * 0.5f) * TILT_SDL_G;
  } else {
    hx *= 0.90f;
    hy *= 0.90f;
  }
  gx = hx;
  gy = hy;
#endif
}

#endif
