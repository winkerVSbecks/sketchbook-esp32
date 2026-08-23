// ============================================================================
// boards/amoled_18.h — Waveshare ESP32-S3-Touch-AMOLED-1.8, V1 revision
// ============================================================================
// 368x448 SH8601 AMOLED over QSPI, FT3168 touch, ESP32-S3R8, 8MB octal PSRAM,
// 16MB flash, QMI8658 IMU, AXP2101 PMU, PCF85063 RTC, ES8311 codec.
//
// Pins come from the Waveshare demo package's pin_config.h
// (ESP32-S3-Touch-AMOLED-1.8-Demo.zip, Arduino-v3.3.5/libraries/Mylibrary/) —
// same rule as the 1.47B: the demo zip is the source of truth, not the wiki.
//
// This header is written for the V1 revision (SH8601 panel + FT3168 touch),
// which is the unit in hand — confirmed by arc tiles rendering on hardware.
// A V2 exists with a CO5300 panel + CST820 touch; LovyanGFX 1.2.27 ships
// Panel_CO5300 ready-made, so if a V2 board stays black on this config, swap
// the panel subclass below for lgfx::Panel_CO5300 rather than debugging init
// commands. The two revisions are told apart on a live board by the touch
// chip's I2C address: FT3168 answers at 0x38, CST820 at 0x15.
//
// LovyanGFX's Panel_AMOLED family is gated behind LGFX_USE_QSPI, which the
// library defines itself on IDF >= 4.4 (platforms/esp32/common.hpp) — the
// guard below is a tripwire for toolchain changes, not a flag to set.
// ============================================================================
#pragma once

static const int W = 368;
static const int H = 448;

// 368*448*2 = 322KB — bigger than the S3's entire usable internal heap, so
// the framebuffer lives in PSRAM here. This only works with octal PSRAM
// actually up, hence board_build.arduino.memory_type = qio_opi in the env.
// pushSprite reads it scan-shaped, which the PSRAM cache handles well; the
// cost that changes is per-pixel work on cv, not the push.
static const bool FB_IN_PSRAM = true;

// BOOT as on the 1.47B. The PWR button belongs to the AXP2101 (power on/off,
// long-press hard cut) and is not a GPIO — leave it alone.
static const int PIN_BOOT = 0;

// One I2C bus shared by the FT3168 touch, QMI8658 IMU, AXP2101 PMU, and
// PCF85063 RTC (pin_config.h calls it IIC_SDA/IIC_SCL; TP_INT is GPIO21,
// unused — touch.h polls the finger count instead).
static const int PIN_IMU_SDA = 15;
static const int PIN_IMU_SCL = 14;

// FT3168 capacitive touch, for touch.h. Tap stands in for the RESET button
// this board doesn't have: tap the glass, get a new composition.
static const int TOUCH_I2C_ADDR = 0x38;

#if defined(ARDUINO)

#if !defined(LGFX_USE_QSPI)
  #error "LovyanGFX did not define LGFX_USE_QSPI - Panel_AMOLED is compiled out on this toolchain"
#endif

#include <Wire.h>

// ---------------------------------------------------------------------------
// AXP2101 power rails
// ---------------------------------------------------------------------------
// The panel and peripherals hang off PMU rails that are OFF at a cold power-on
// — the factory firmware enables them, which is why the panel "just worked"
// through every warm reset until the first real unplug, and then didn't.
// This replicates the rail set from the demo package's port_axp2101.cpp
// (DC1/DC3 3.3V, ALDO1 1.8V, ALDO2 2.8V, ALDO3 3.3V, ALDO4 3.0V, BLDO1/2
// 3.3V), additively: voltages are set and rails enabled, nothing is ever
// disabled, so the rail the ESP32 itself runs on (DC1) cannot be interrupted.
static inline bool _axpWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static inline uint8_t _axpRead(uint8_t reg) {
  Wire.beginTransmission(0x34);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(0x34, 1) != 1) return 0;
  return Wire.read();
}

static inline void boardPowerBegin() {
  Wire.begin(PIN_IMU_SDA, PIN_IMU_SCL, 400000);

  // Boot prints die unheard while USB is still enumerating after a reset, and
  // this board has no RESET button to replay them once a monitor is attached.
  // Hold for a host briefly (bounded — standalone boots continue after 2s).
  for (int i = 0; i < 20 && !Serial; i++) delay(100);

  // Reg 0x03 carries the chip id; XPowersLib expects (v & 0xCF) == 0x4A.
  const uint8_t id = _axpRead(0x03);
  Serial.printf("AXP2101 id reg 0x03 = 0x%02X %s\n", id,
                (id & 0xCF) == 0x4A ? "(match)" : "(UNEXPECTED)");

  // Voltages first, enables after, so nothing switches on at a wrong level.
  bool ok = true;
  ok &= _axpWrite(0x82, 0x12);                 // DC1   3.3V (1.5V + n*0.1)
  ok &= _axpWrite(0x84, 0x69);                 // DC3   3.3V (range 3: 1.6V + (n-88)*0.1)
  ok &= _axpWrite(0x92, 0x0D);                 // ALDO1 1.8V (0.5V + n*0.1)
  ok &= _axpWrite(0x93, 0x17);                 // ALDO2 2.8V
  ok &= _axpWrite(0x94, 0x1C);                 // ALDO3 3.3V
  ok &= _axpWrite(0x95, 0x19);                 // ALDO4 3.0V
  ok &= _axpWrite(0x96, 0x1C);                 // BLDO1 3.3V
  ok &= _axpWrite(0x97, 0x1C);                 // BLDO2 3.3V
  ok &= _axpWrite(0x80, _axpRead(0x80) | 0x05);  // DC1 + DC3 on
  ok &= _axpWrite(0x90, _axpRead(0x90) | 0x3F);  // ALDO1-4 + BLDO1-2 on

  Serial.printf("AXP writes %s; readback 0x80=0x%02X 0x90=0x%02X volts=%02X %02X %02X %02X %02X %02X\n",
                ok ? "acked" : "FAILED",
                _axpRead(0x80), _axpRead(0x90),
                _axpRead(0x92), _axpRead(0x93), _axpRead(0x94),
                _axpRead(0x95), _axpRead(0x96), _axpRead(0x97));

  delay(100);                                  // let the rails settle
}

// SH8601 as a Panel_AMOLED subclass, the way Panel_RM690B0 does it for the
// T4-S3. The init list is Panel_SH8601Z's (the older standalone driver for
// the same controller); Panel_AMOLED supplies everything else — QSPI write
// framing, setWindow, brightness via command 0x51 (AMOLED: no backlight pin).
struct Panel_SH8601_AMOLED18 : public lgfx::Panel_AMOLED {
  Panel_SH8601_AMOLED18(void) {
    _cfg.memory_width  = _cfg.panel_width  = W;
    _cfg.memory_height = _cfg.panel_height = H;
    _write_depth = lgfx::color_depth_t::rgb565_2Byte;
    _read_depth  = lgfx::color_depth_t::rgb565_2Byte;
  }

  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
      0x11, 0 + CMD_INIT_DELAY, 150,              // sleep out
      0x44, 2 + CMD_INIT_DELAY, 0x01, 0x66, 1,    // set tear scanline
      0x35, 1 + CMD_INIT_DELAY, 0x00, 1,          // TE on
      0x3a, 1 + CMD_INIT_DELAY, 0x55, 1,          // 16bit/pixel
      0x53, 1 + CMD_INIT_DELAY, 0x20, 10,         // brightness ctrl on
      0x51, 1 + CMD_INIT_DELAY, 0x00, 10,         // brightness 0 while off
      0x29, 0 + CMD_INIT_DELAY, 10,               // display on
      0x51, 1 + CMD_INIT_DELAY, 0xff, 1,          // brightness max
      0xff, 0xff
    };
    switch (listno) {
      case 0:  return list0;
      default: return nullptr;
    }
  }
};

class LGFX : public lgfx::LGFX_Device {
  Panel_SH8601_AMOLED18 _panel;
  lgfx::Bus_SPI         _bus;
public:
  LGFX(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000; c.freq_read = 20000000;
      c.dma_channel = SPI_DMA_CH_AUTO;
      // QSPI: four data lines instead of mosi/miso.
      c.pin_sclk = 11;
      c.pin_io0 = 4; c.pin_io1 = 5; c.pin_io2 = 6; c.pin_io3 = 7;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 12; c.pin_rst = -1; c.pin_busy = -1;   // no reset pin wired
      // The glass maps controller columns 16..383: without this the first
      // 16px of the framebuffer's x axis fall off one edge and a 16px black
      // bar sits on the opposite one. Measured with board_diag.cpp's ruler on
      // hardware — Waveshare's own demos ship 0 and show the same bar, so
      // don't "correct" this from their code. Note the mounting: with USB
      // down, the fb x axis runs vertically (x=W at the USB edge) and fb y
      // runs right-to-left — tilt/touch mappings must account for it.
      c.offset_x = 16; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.bus_shared = false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};

#endif  // ARDUINO
