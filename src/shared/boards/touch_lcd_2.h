// ============================================================================
// boards/touch_lcd_2.h — Waveshare ESP32-S3-Touch-LCD-2
// ============================================================================
// 240x320 ST7789T3 IPS over SPI, CST816D touch, ESP32-S3R8, 8MB octal PSRAM,
// 16MB flash, QMI8658 IMU. No PMU — rails are hardwired (ETA6098 charger +
// TMI3112H buck), so unlike the AMOLED-1.8 a black panel here is never a
// power-rail problem. BOOT and RESET buttons both exist, so RESET-reseed
// works as on the 1.47B and tap is a bonus gesture, not a stand-in.
//
// Pins verified from ESP32-S3-Touch-LCD-2-Demo.zip (bsp_lv_port.h, bsp_i2c.h,
// bsp_cst816.h) and the schematic PDF — same rule as the other boards: the
// demo zip is the source of truth, not the wiki (which has no pinout table).
// Full research notes: .claude/skills/port-canvas-sketch-to-esp32/references/
// board-touch-lcd-2.md.
// ============================================================================
#pragma once

static const int W = 240;
static const int H = 320;

// 240*320*2 = 153,600 bytes. That might squeeze into internal SRAM standalone,
// but the switcher's roster statics leave nowhere near a contiguous 150KB by
// panelBegin() time — the kind of margin that fails at boot rather than
// degrading. PSRAM placement is the a18's proven pattern: pushSprite reads
// scan-shaped, which the cache handles well; only per-pixel work on cv slows.
static const bool FB_IN_PSRAM = true;

// BOOT idles high and shorts to ground when pressed. RESET is wired to EN.
// (The schematic has an unpopulated resistor (R16) that would tie the panel's
// reset line to GPIO0 as well; shipped boards leave it NC.)
static const int PIN_BOOT = 0;

// One I2C bus shared by the CST816D touch and the QMI8658 IMU — the same two
// GPIOs as the 1.47B's IMU bus, coincidentally. TP_INT is GPIO46, unused:
// touch.h polls. (The camera connector has its own separate I2C on 16/21.)
static const int PIN_IMU_SDA = 48;
static const int PIN_IMU_SCL = 47;

// CST816D capacitive touch, for touch.h. Same finger-count register (0x02) as
// the a18's FT3168, but the CST816 family auto-sleeps and NACKs I2C reads
// while untouched — a NACK means "no finger", not "chip missing", which is
// what TOUCH_NACKS_WHEN_IDLE tells touchBegin().
static const int  TOUCH_I2C_ADDR       = 0x15;
static const bool TOUCH_NACKS_WHEN_IDLE = true;

#if defined(ARDUINO)

// No power-management chip: the panel is powered whenever the board is.
static inline void boardPowerBegin() {}

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      // Waveshare's demos ship 80MHz on these same GPIO-matrix pins; start at
      // the repo's proven 40 and raise it only if a sketch needs the ~15ms
      // full-frame push instead of ~31ms.
      c.freq_write = 40000000; c.freq_read = 16000000;
      // The LCD flex has no SDO line. MISO=40 exists on this SPI bus but
      // belongs to the TF card slot, which nothing here uses.
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 39; c.pin_mosi = 38; c.pin_miso = -1; c.pin_dc = 42;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      // No reset pin: the panel reset is an RC power-on circuit on the flex.
      c.pin_cs = 45; c.pin_rst = -1; c.pin_busy = -1;
      // The glass is the ST7789's full 240x320 GRAM — the one board so far
      // with no window offset (1.47B: 34, a18: 16).
      c.memory_width = 240; c.memory_height = 320;
      c.panel_width  = W;   c.panel_height  = H;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      // GPIO1 driving an NPN transistor, active high; the demos PWM it at
      // 5kHz. Not GPIO46 (that's TP_INT here) — every board puts its
      // backlight somewhere new.
      c.pin_bl = 1; c.invert = false; c.freq = 5000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

#endif  // ARDUINO
