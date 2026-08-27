// ============================================================================
// boards/rotary_128.h — ELECROW CrowPanel 1.28" HMI Rotary Display (DHE38128D)
// ============================================================================
// 240x240 GC9A01 round IPS over SPI, CST816D touch, ESP32-S3R8, 8MB octal
// PSRAM, 16MB flash — and the point of the board: a 30-detent quadrature
// encoder ring (EC3501 C15H30P3) around the glass, with a full-press switch
// under the knob. No IMU, no PMU chip, no battery.
//
// Pins verified from the vendor's own demo, which conveniently uses LovyanGFX
// itself (example/Arduino/RotaryScreen_1_28/RotaryScreen_1_28.ino in
// github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-
// IPS-Round-Touch-Knob-Screen) — the demo .ino is the source of truth; the
// wiki's library/core versions are stale against it. Encoder resolution from
// the EC3501 datasheet in the same repo. Full research notes:
// .claude/skills/port-canvas-sketch-to-esp32/references/board-rotary-128.md.
// ============================================================================
#pragma once

static const int W = 240;
static const int H = 240;

// 240*240*2 = 115,200 bytes — the 1.47B's 110KB story, not the t2's:
// it fits in internal SRAM with room left for a sketch's statics, and
// internal SRAM is what keeps per-pixel work on cv cheap. (The vendor demo
// proves PSRAM framebuffers also render fine here, so flipping this is safe
// if a roster ever crowds internal out.)
static const bool FB_IN_PSRAM = false;

// BOOT and RESET buttons exist (on the underside; RESET is wired to EN as
// always). The knob's own full-press is a separate GPIO, below.
static const int PIN_BOOT = 0;

// Touch I2C. The PIN_IMU_* names are what platform.h/touch.h call the board's
// I2C bus on every board; there is no IMU behind them here — the CST816D is
// alone on the bus. (An expansion I2C connector exists on 38/39, unused.)
static const int PIN_IMU_SDA = 6;
static const int PIN_IMU_SCL = 7;

// CST816D capacitive touch, same chip and address as the t2, same auto-sleep
// quirk: it NACKs while untouched, so a failed boot probe means "asleep",
// not "absent". Its reset line is a real GPIO here (13), pulsed in
// boardPowerBegin() — the a18/t2 flexes reset themselves, this one doesn't.
static const int  TOUCH_I2C_ADDR        = 0x15;
static const bool TOUCH_NACKS_WHEN_IDLE = true;

// The encoder ring, for encoder.h: quadrature A/B on plain GPIOs (the board
// carries external pullups; the vendor demo polls with pinMode(INPUT)).
//
// ENC_COUNTS_PER_REV is 4x-decode edges per mechanical revolution. The
// EC3501 C15H30P3 datasheet: 30 detents at 12°, "output signal is 1 pulse
// per 2 detents" — 15 quadrature cycles, so 60 edges per revolution, two per
// detent. (The datasheet's series table also says "30 pulses/360°",
// contradicting its own note; the model code C15H30 breaks the tie. If one
// physical turn ever sweeps TWO loops of a sketch, that tie-break was wrong:
// make this 120.)
static const int PIN_ENC_A          = 45;
static const int PIN_ENC_B          = 42;
static const int ENC_COUNTS_PER_REV = 60;

// The knob's full-press switch: idles high (pull-up), pressed = LOW. Not
// PIN_BOOT — a sketch that wants "press the knob" reads this deliberately.
static const int PIN_KNOB_BTN = 41;

#if defined(ARDUINO)

// No PMU chip, but not a no-op either: the vendor demo drives GPIO1 and
// GPIO2 HIGH before touching the panel — undocumented rail/enable lines
// (the schematic PDF would say which; the demo proves they must be high).
// Replicated additively, like the a18's rail set. The CST816D's reset
// (GPIO13) is pulsed here too, so the chip is awake before touchBegin()
// probes it — its datasheet wants ~50ms out of reset.
static inline void boardPowerBegin() {
  pinMode(1, OUTPUT);  digitalWrite(1, HIGH);
  pinMode(2, OUTPUT);  digitalWrite(2, HIGH);
  pinMode(13, OUTPUT);
  digitalWrite(13, LOW);
  delay(10);
  digitalWrite(13, HIGH);
  delay(60);
}

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      // The vendor demo ships 80MHz on these pins (SCLK=10 is not the S3's
      // IOMUX FSPI clock, so that's 80 through the GPIO matrix, DMA and all).
      // Start at the repo's proven 40 — a full frame is ~23ms there, and the
      // headroom is demonstrably real if a sketch wants it.
      c.freq_write = 40000000; c.freq_read = 20000000;
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 10; c.pin_mosi = 11; c.pin_miso = -1; c.pin_dc = 3;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      // A real reset pin, for once (the a18 and t2 both lack one).
      c.pin_cs = 9; c.pin_rst = 14; c.pin_busy = -1;
      // The glass is the GC9A01's full 240x240 GRAM — zero offset, though
      // the visible area is the inscribed circle: the corners exist in
      // memory and are cropped by the bezel.
      c.memory_width = 240; c.memory_height = 240;
      c.panel_width  = W;   c.panel_height  = H;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      // GPIO46 PWM, non-inverted (the demo's 50% duty lights the panel) —
      // the same backlight pin as the 1.47B, coincidentally.
      c.pin_bl = 46; c.invert = false; c.freq = 5000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

#endif  // ARDUINO
