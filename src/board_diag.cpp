// ============================================================================
// board_diag.cpp — calibration pattern + live IMU readout for board bringup
// ============================================================================
// Not art. Flash this when a new board's panel geometry or IMU is in doubt.
//
// The pattern answers, in one look at the glass:
//   - orientation and mirroring: 24px corner squares, RED at the framebuffer
//     origin (0,0), GREEN at (W-1,0), BLUE at (0,H-1), YELLOW at (W-1,H-1)
//   - how many leading x pixels the glass discards: a ruler of tick lines
//     whose left edges sit at exactly x = 8,12,...,40, one per row, each
//     labeled. The first row with an intact tick = the panel's x offset.
//
// Serial (115200) prints tilt plus the raw accel bytes at 2Hz and any shake
// above 1.3g, so holding the board flat / on edge / giving it a flick
// verifies live accelerometer reads rather than just the WHO_AM_I probe.
// ============================================================================
#define SKETCH_TITLE "board diag"

#include "shared/platform.h"
#include "shared/imu.h"

static const int RING_STEP = 16;
static const int N_RINGS   = 5;
static const int CORNER    = 24;

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!panelBegin()) {
    Serial.println("FATAL: framebuffer allocation failed");
    return;
  }

  cv.fillSprite(TFT_BLACK);

  // Ruler along the x axis: for each k, a tick line whose left edge sits at
  // exactly x = k, with the number k drawn immediately after it on its own
  // row. The first row whose tick is intact measures how many leading x
  // pixels the glass discards, to 4px.
  cv.setTextSize(2);
  int row = 0;
  for (int k = 8; k <= 40; k += 4, row++) {
    const int y = 30 + row * 44;
    const uint16_t col = (k % 8 == 0) ? TFT_WHITE : TFT_CYAN;
    cv.fillRect(k, y, 3, 32, col);              // tick: left edge exactly at x=k
    cv.setTextColor(col);
    cv.drawNumber(k, 56, y + 8);                // label safely inside
  }

  cv.fillRect(0,          0,          CORNER, CORNER, TFT_RED);
  cv.fillRect(W - CORNER, 0,          CORNER, CORNER, TFT_GREEN);
  cv.fillRect(0,          H - CORNER, CORNER, CORNER, TFT_BLUE);
  cv.fillRect(W - CORNER, H - CORNER, CORNER, CORNER, TFT_YELLOW);

  present();

  Serial.printf("board diag: %dx%d  ruler ticks at x=8..40 step 4\n", W, H);
  Serial.println("corners: RED=(0,0) GREEN=(W,0) BLUE=(0,H) YELLOW=(W,H)");
  imuBegin();
}

void loop() {
  static uint32_t lastTilt = 0;
  const uint32_t now = millis();
  if (now - lastTilt >= 500) {
    lastTilt = now;
    float gx = 0.0f, gy = 0.0f;
    imuTilt(gx, gy);
#if defined(ARDUINO)
    uint8_t raw[6] = {0};
    const bool ok = imuRead(QMI_AX_L, raw, 6);
    Serial.printf("tilt gx=%+.2f gy=%+.2f  read=%d raw=%02X%02X %02X%02X %02X%02X\n",
                  gx, gy, ok, raw[1], raw[0], raw[3], raw[2], raw[5], raw[4]);
#else
    Serial.printf("tilt gx=%+.2f gy=%+.2f\n", gx, gy);
#endif
  }
  shakeDetected(1.3f);   // prints "shake X.XXg" itself on a hit
  delay(16);
}
