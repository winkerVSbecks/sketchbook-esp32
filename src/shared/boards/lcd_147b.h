// ============================================================================
// boards/lcd_147b.h — Waveshare ESP32-S3-LCD-1.47B
// ============================================================================
// Everything platform.h needs that is a fact about this board rather than
// about a build target: the panel dimensions, the device panel class and its
// wiring, where the framebuffer belongs, and the pins for the buttons and the
// IMU. Included by platform.h only — a sketch never includes a board header
// itself, and never mentions a board name; it draws in terms of W and H.
//
// 172x320 ST7789 IPS over SPI, ESP32-S3R8, 8MB octal PSRAM, QMI8658 IMU.
// ============================================================================
#pragma once

static const int W = 172;
static const int H = 320;

// The 110KB full-frame sprite fits in internal SRAM with room to spare, and
// internal SRAM is what makes per-pixel work on it cheap — so it stays there.
// (The AMOLED-1.8's 322KB frame cannot fit internally and goes to PSRAM; this
// constant is what panelBegin() reads to tell the two apart.)
static const bool FB_IN_PSRAM = false;

// BOOT idles high through its pull-up and shorts to ground when pressed, so a
// press is a falling edge. RESET is wired to EN and software never sees it.
static const int PIN_BOOT = 0;

// QMI8658 I2C. The Waveshare wiki's pinout tables cover only the LCD, RGB LED
// and TF card, but ESP32-S3-LCD-1.47B-Demo.zip pins the bus down: SDA 48 /
// SCL 47, agreed on by both the Arduino (I2C_Driver.h) and ESP-IDF
// (I2C_Driver.h) demos.
//
// Do NOT take these from the ESP32 core's waveshare_esp32_s3_lcd_147 variant —
// its SDA=8/SCL=9 are generic boilerplate, not this board's wiring, and they
// fail the WHO_AM_I probe silently.
static const int PIN_IMU_SDA = 48;
static const int PIN_IMU_SCL = 47;

// No touch controller on this board — touch.h's tapDetected() is always
// false here, and RESET covers the reseed gesture instead.
static const int TOUCH_I2C_ADDR = -1;

#if defined(ARDUINO)

// This board has no power-management chip — the panel is powered whenever the
// board is. panelBegin() calls this on every board before lcd.init().
static inline void boardPowerBegin() {}

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 16000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 40; c.pin_mosi = 45; c.pin_miso = -1; c.pin_dc = 41;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 42; c.pin_rst = 39; c.pin_busy = -1;
      c.memory_width = 240; c.memory_height = 320;   // 172-wide window into 240x320 controller RAM
      c.panel_width  = W;   c.panel_height  = H;
      c.offset_x = 34; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      // GPIO46 on this "B" revision — the older non-B board (and nearly every
      // config online) uses GPIO48, which here is the IMU's SDA.
      c.pin_bl = 46; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

#endif  // ARDUINO
