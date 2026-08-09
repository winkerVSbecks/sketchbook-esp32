// ============================================================================
// imu.h — shake gesture from the QMI8658, with a click stand-in on SDL
// ============================================================================
// Device: the QMI8658 6-axis IMU over I2C. The Waveshare wiki's pinout tables
// cover only the LCD, RGB LED and TF card, but ESP32-S3-LCD-1.47B-Demo.zip
// pins the bus down: I2C_SDA_PIN 48 / I2C_SCL_PIN 47, agreed on by both the
// Arduino (I2C_Driver.h) and ESP-IDF (I2C_Driver.h) demos.
//
// Do NOT take these from the ESP32 core's waveshare_esp32_s3_lcd_147 variant —
// its SDA=8/SCL=9 are generic boilerplate, not this board's wiring, and they
// fail the WHO_AM_I probe silently.
//
// Desktop: click the window.
//
//   imuBegin()          probe and configure; false means no shake gesture at
//                       all, so RESET is the only way to a new composition
//   shakeDetected(g)    true on a peak above `g` total acceleration
// ============================================================================
#pragma once

#include "platform.h"

#if defined(ARDUINO)

#include <Wire.h>

static const int PIN_IMU_SDA = 48;
static const int PIN_IMU_SCL = 47;

static const uint8_t QMI_WHO_AM_I = 0x00;   // reads 0x05
static const uint8_t QMI_CTRL1    = 0x02;
static const uint8_t QMI_CTRL2    = 0x03;
static const uint8_t QMI_CTRL7    = 0x08;
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

#endif
