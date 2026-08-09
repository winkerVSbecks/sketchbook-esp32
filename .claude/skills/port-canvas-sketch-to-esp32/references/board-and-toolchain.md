# Board and toolchain reference

## Contents

- [Board and toolchain reference](#board-and-toolchain-reference)
  - [Contents](#contents)
  - [Hardware facts](#hardware-facts)
  - [Pin map](#pin-map)
  - [platformio.ini](#platformioini)
  - [Platform shims](#platform-shims)
  - [Panel config: ESP32](#panel-config-esp32)
  - [Panel config: SDL](#panel-config-sdl)
  - [SDL entry point](#sdl-entry-point)
  - [Building and flashing](#building-and-flashing)
  - [Panel config checklist](#panel-config-checklist)
  - [Arduino IDE alternative](#arduino-ide-alternative)

## Hardware facts

Waveshare **ESP32-S3-LCD-1.47B** (the "B" revision — the non-B board differs,
and most configs online are for the non-B):

- ESP32-S3R8, dual-core Xtensa LX7 @ 240MHz, single-precision FPU
- 512KB internal SRAM, 8MB **octal** PSRAM, 16MB flash (W25Q128)
- 1.47" IPS, **ST7789**, 172×320, RGB565 over 4-wire SPI
- QMI8658 6-axis IMU on I2C **SDA=48, SCL=47**, address **0x6B**, 400kHz
  (verified from `ESP32-S3-LCD-1.47B-Demo.zip`; the wiki's pinout tables omit
  the IMU entirely, and the ESP32 core's variant `pins_arduino.h` gives generic
  boilerplate SDA=8/SCL=9 that silently fails the WHO_AM_I probe)
- WS2812 RGB LED, TF card slot (SDIO), Li-ion charging, BOOT + RESET buttons
- Native USB (no CH340) — no serial driver needed

Key consequence: a 16-bit framebuffer is 172 × 320 × 2 = **110,080 bytes**,
which fits in internal SRAM. You get a real full-frame backbuffer with no
tearing and without paying PSRAM latency. Keep WiFi off and there's headroom.

## Pin map

From the ESP32-S3-LCD-1.47B schematic.

| LCD    | GPIO   |     | TF card | GPIO |     | Other  | GPIO |
| ------ | ------ | --- | ------- | ---- | --- | ------ | ---- |
| MOSI   | 45     |     | CLK     | 14   |     | WS2812 | 38   |
| SCLK   | 40     |     | CMD     | 15   |     | BOOT   | 0    |
| CS     | 42     |     | D0      | 16   |     |        |      |
| DC     | 41     |     | D1      | 18   |     |        |      |
| RST    | 39     |     | D2      | 17   |     |        |      |
| **BL** | **46** |     | D3      | 21   |     |        |      |

**The backlight is GPIO46 on the B revision.** The older non-B board uses
GPIO48, and nearly every blog post and TFT_eSPI config online is for the non-B.
A dark screen on an otherwise-working board is usually this.

The ST7789's GRAM is 240 wide but the glass is 172, so `offset_x = 34`.
Getting that wrong gives a shifted image with a garbage stripe.

## platformio.ini

Two environments, one source tree. This is the main reason to use PlatformIO
over the Arduino IDE — the board config becomes a diffable file.

```ini
[env:native]
platform = native
build_type = debug
lib_deps = lovyan03/LovyanGFX
build_flags =
    -O0
    -xc++
    -std=c++14
    !sdl2-config --cflags --libs

[env:esp32]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
lib_deps = lovyan03/LovyanGFX
build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1
```

Use `!sdl2-config --cflags --libs` rather than hardcoded paths. LovyanGFX's own
example ini hardcodes `/usr/local/include/SDL2` and MSYS2 paths, which breaks on
Apple Silicon where Homebrew lives at `/opt/homebrew`.

Don't work inside LovyanGFX's `examples_for_PC/` directory. Its
`lib_extra_dirs = ../../` makes the dependency finder crawl the whole repo,
which is very slow, and it builds their demo rather than the user's source.

Requires `brew install sdl2` (or the platform equivalent). Source goes at
`src/main.cpp`.

## Platform shims

The `native` platform has no Arduino runtime. LovyanGFX supplies `millis` and
`delay` for SDL in `sdl/common.hpp` — use those rather than writing your own.

```cpp
#if defined(ARDUINO)

  #include <Arduino.h>

#else

  #include <lgfx/v1/platforms/sdl/Panel_sdl.hpp>
  #include <cstdio>
  #include <cstdlib>

  using lgfx::millis;
  using lgfx::delay;

  static uint32_t esp_random() {
    return ((uint32_t)rand() << 16) ^ (uint32_t)rand();
  }

  struct SerialShim {
    void begin(int) {}
    template <typename... A> void printf(const char *f, A... a) { std::printf(f, a...); }
    void println(const char *s = "") { std::printf("%s\n", s); }
  };
  static SerialShim Serial;

#endif
```

## Panel config: ESP32

```cpp
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
      c.memory_width = 240; c.memory_height = 320;
      c.panel_width  = 172; c.panel_height  = 320;
      c.offset_x = 34; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 46; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
```

GPIO40 and GPIO45 are not the S3's IOMUX SPI pins, so the bus routes through
the GPIO matrix. 40MHz is reliable; 80MHz may or may not be, depending on the
unit. Start at 40 and try 80 only if you need the bandwidth.

## Panel config: SDL

Note `offset_x = 0` — the 34px offset is a physical artefact of the glass being
narrower than the controller's GRAM. Applying it in simulation double-shifts.

```cpp
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_sdl _panel;
public:
  LGFX(void) {
    { auto c = _panel.config();
      c.memory_width = 172; c.panel_width  = 172;
      c.memory_height = 320; c.panel_height = 320;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.bus_shared = false;
      _panel.config(c);
      _panel.setScaling(2, 2);            // otherwise it's postage-stamp sized
      _panel.setWindowTitle("sketch");
    }
    setPanel(&_panel);
  }
};
```

## SDL entry point

LovyanGFX owns the SDL event pump and calls into your `setup`/`loop`.

```cpp
#if !defined(ARDUINO)
#if defined(SDL_h_)

__attribute__((weak))
int user_func(bool *running) {
  setup();
  do { loop(); } while (*running);
  return 0;
}

int main(int, char **) {
  return lgfx::Panel_sdl::main(user_func);
}

#endif
#endif
```

`Panel_sdl::main` takes an optional FPS argument in some versions — if the
compiler complains about arity, add `, 60`. If linking fails with "undefined
symbol: _main", `SDL_h_`wasn't defined at that point; move the`Panel_sdl.hpp`include above`<LovyanGFX.hpp>`.

## Building and flashing

```bash
pio run -e native -t exec          # build and run the SDL window
pio run -e esp32 -t upload         # build and flash the board
pio device monitor                 # serial output
```

The first native build compiles all of LovyanGFX and takes a minute or two;
after that only your source recompiles. The `-Wvla-cxx-extension` warnings from
LovyanGFX are harmless.

If upload fails or the port doesn't appear: hold **BOOT**, tap **RESET**,
release **BOOT** to force download mode, then press RESET to run. Native USB
re-enumerates after each upload, so the port may need reselecting.

## Panel config checklist

Emulators can't validate these — they're physical properties of the specific
panel. When the first render looks wrong, it's almost always one of four
one-line fixes:

| Symptom                           | Fix                             |
| --------------------------------- | ------------------------------- |
| Photo negative                    | `cfg.invert = false`            |
| Red and blue swapped              | `cfg.rgb_order = true`          |
| Shifted image with garbage stripe | `cfg.offset_x` isn't 34         |
| Dark screen, panel clearly alive  | `cfg.pin_bl` must be 46, not 48 |

## Arduino IDE alternative

See the `flash-sketch-to-device` skill for the full Arduino IDE route. Two
things that bite immediately:

**Renaming the source to `.ino` does not work.** The Arduino builder injects
generated prototypes above the type definitions they reference, so any function
taking a struct fails with `'GridCell' does not name a type`, pointing at the
definition rather than the injected line. Use a one-line `.ino` (`#include
"sketch.h"`) plus the real code in `sketch.h` — headers are exempt.

**Select the real board**, `esp32 → Waveshare ESP32-S3-LCD-1.47` (core 3.3.11+),
not "ESP32S3 Dev Module". It already sets 16MB flash, octal PSRAM and the USB
IDs. The only default that needs changing is **USB CDC On Boot → Enabled**;
native USB means `Serial` goes nowhere without it.

Board package: `esp32 by Espressif Systems` ≥ 3.0.2. Library: LovyanGFX.
