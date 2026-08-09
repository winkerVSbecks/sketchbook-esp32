# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A generative-art sketchbook for the **Waveshare ESP32-S3-LCD-1.47B** (ESP32-S3R8, 172x320 ST7789 IPS over SPI, 8MB octal PSRAM). Sketches are written in the style of canvas-sketch/p5 pieces and ported to C++ on LovyanGFX.

Two sketches live in `src/`, one per pair of environments:

| Sketch | Envs | Advances on |
|---|---|---|
| `src/main.cpp` | `native`, `esp32` | an 8s timer |
| `src/arc_tiles_shake.cpp` | `shake_native`, `shake_esp32` | shaking the board (QMI8658 IMU); click the window on SDL |

Each env sets `build_src_filter` to pick exactly one file. **A new sketch needs its own env pair plus a filter** — without one, PlatformIO compiles every `.cpp` in `src/` into a single binary and the duplicate `setup()`/`loop()` fail to link.

## Commands

```bash
pio run -e native -t exec        # build + run in an SDL2 window on the Mac (2x scaled)
pio run -e shake_native -t exec  # same, shake sketch (click the window to "shake")
pio run -e esp32 -t upload       # build + flash the board
pio run -e shake_esp32 -t upload # build + flash the shake sketch
pio device monitor -b 115200     # serial output from the board
```

There are no tests. The SDL target *is* the test loop — iterate there, flash only to confirm on real hardware.

Requirements: `sdl2` from Homebrew (the native env shells out to `sdl2-config`). The espressif32 toolchain is installed and all four environments build; `shake_esp32` has been flashed and verified on hardware.

If the board won't flash: hold BOOT, tap RESET, release BOOT.

## Dual-target architecture

`src/main.cpp` is a single file that compiles for both desktop and device. The split is `#if defined(ARDUINO)`, and it appears in three places:

1. **Platform shims** — on SDL, `esp_random()` wraps `rand()`, `Serial` is a struct forwarding to `printf`, and `millis`/`delay` come from `lgfx::` (LovyanGFX ships them for SDL in `sdl/common.hpp`).
2. **The `LGFX` panel class** — `Panel_ST7789` + `Bus_SPI` + `Light_PWM` on device, `Panel_sdl` with `setScaling(2,2)` on desktop. Both expose the same `lcd` object, so nothing downstream cares.
3. **Entry point** — Arduino calls `setup()`/`loop()`; SDL needs the `user_func` + `lgfx::Panel_sdl::main()` pair at the bottom of the file.

Sketch logic between those blocks is platform-agnostic and should stay that way. When adding a platform-specific API, add a shim rather than sprinkling `#if` through the sketch body.

## Rendering model

One full-frame `LGFX_Sprite cv` (172 × 320 × 2 bytes = 110KB) allocated with `setPsram(false)` so it lives in fast internal SRAM. Everything draws into `cv`; `cv.pushSprite(0, 0)` blits. Never draw straight to `lcd`.

## Hardware facts that are easy to get wrong

- **Backlight is GPIO46** on the "B" revision. The older non-B board uses GPIO48, and nearly every config you'll find online (TFT_eSPI setups especially) is for the non-B.
- Panel pins: `SPI2_HOST`, sclk=40, mosi=45, dc=41, cs=42, rst=39. 40MHz write. GPIO40/45 are *not* the S3's IOMUX SPI pins, so 80MHz routes through the GPIO matrix and may corrupt pixels — drop back if it does.
- The panel is a 172-wide window into 240x320 controller memory: `offset_x = 34`, `invert = true`.
- `ARDUINO_USB_CDC_ON_BOOT=1` (set in `platformio.ini`) is what makes `Serial.print()` reach USB.
- **QMI8658 IMU is on I2C SDA=GPIO48, SCL=GPIO47** (400kHz), address 0x6B. The wiki's pinout tables cover only LCD/RGB/TF and omit this; the source is `ESP32-S3-LCD-1.47B-Demo.zip`, where the Arduino (`LVGL_Arduino/I2C_Driver.h`) and ESP-IDF (`main/I2C_Driver/I2C_Driver.h`) demos agree. Do **not** use the ESP32 core's `waveshare_esp32_s3_lcd_147` variant values (SDA=8/SCL=9) — that's generic boilerplate, not this board, and it silently fails the WHO_AM_I probe.
- Still unverified: BAT_ADC. Other pins: WS2812 on GPIO38, BOOT button on GPIO0, TF card SDIO CLK=14 CMD=15 D0=16 D1=18 D2=17 D3=21.

## Porting constraints

These come from `ws_s3_lcd_147b_sketch.ino`'s PORTING NOTES and apply to every sketch here:

- **Floats, never doubles.** The S3 has a single-precision FPU; `double` is software-emulated and ~20x slower. Write `2.0f`, and use `sinf`/`cosf`/`powf`/`sqrtf`/`fmodf`. A mechanical port from JS (where every number is a double) lands silently in the slow path.
- **No allocation in the draw loop.** Fixed-size arrays with a count, everything built in `setup()`.
- **Frame budget.** A full `pushSprite` is ~22ms at 40MHz, so ~45fps is the ceiling before you've drawn anything. Dirty-rect pushes are the real win for sketches that only change rectangular regions.
- **RGB565 bands.** 32 levels of red/blue. Flat panels are fine; long gradients need a Bayer dither at the point of conversion.
- **No paths.** Rectangles, circles, triangles, lines, bitmaps. `fillSmoothCircle`/`fillSmoothRoundRect`/`drawWideLine` are anti-aliased; `fillRect` is not. Arbitrary AA shapes require either analytic coverage (see `drawCell` in `main.cpp`) or supersampling into PSRAM and box-downsampling.

## Conventions carried across sketches

- **mulberry32 PRNG**, seeded, so a sketch can be prototyped in JS and reproduced bit-for-bit on device.
- **Two different color metrics for two different questions.** WCAG contrast ratio answers "is this legible against that background" — it's luminance-only and blind to hue, so it will happily rate blue vs rust at ~1.0. Oklab ΔE answers "can I tell these two apart". `buildPalette()` uses WCAG against the background and ΔE between foregrounds; using WCAG for both is what collapsed the `bless` palette to a single color.
- Palettes are ported from a `colors/found.ts` in the sibling JS work.

## Loose files

`arc_tiles_147b.ino` and `ws_s3_lcd_147b_sketch.ino` sit at the repo root and are **not compiled by PlatformIO** (only `src/` is). They're standalone Arduino IDE copies:

- `arc_tiles_147b.ino` was a byte-identical copy of `src/main.cpp` and has since drifted (`main.cpp` gained `MAX_COLORS`). Re-copy it or leave it alone deliberately; don't edit it as if it were live code.
- `ws_s3_lcd_147b_sketch.ino` is an earlier, separate sketch (sliding-blind system) that carries a canvas-2d-shaped shim — `ctxSave`/`ctxRestore`/`translate`/`clipRect`/`fillStyle` over a `GState` stack — plus an `oklch(L, C, h) -> RGB565` converter. Worth lifting from when a new sketch wants ctx-style structure.

## Arduino IDE mirrors

These sketches also run from the Arduino IDE, which needs `foo/foo.ino`:

```bash
cp src/main.cpp              ~/Documents/Arduino/arc_tiles_147b/sketch.h
cp src/arc_tiles_shake.cpp   ~/Documents/Arduino/arc_tiles_shake/sketch.h
```

**Don't paste a sketch straight into a `.ino`.** The Arduino builder auto-generates function prototypes and injects them above the type definitions they reference, so `drawCell(const GridCell&, ...)` fails with `'GridCell' does not name a type` — pointing at the definition, not the injected line. Each sketch folder is therefore a one-line `.ino` (`#include "sketch.h"`) plus the real code in `sketch.h`; headers are exempt from prototype injection, and the `#include` keeps it a single translation unit.

Board: **Waveshare ESP32-S3-LCD-1.47** (in the ESP32 core, 3.3.11+). Every Tools default is right except **USB CDC On Boot → Enabled** — that's the IDE equivalent of `-DARDUINO_USB_CDC_ON_BOOT=1`, and without it `Serial.print()` never reaches USB.

If the IDE throws `fatal error: opening dependency file ... .libsdetect.d`, that's its background indexer racing another build over `~/Library/Caches/arduino/sketches/` — **Sketch → Clean** and rebuild. A different file named each time is the tell.
