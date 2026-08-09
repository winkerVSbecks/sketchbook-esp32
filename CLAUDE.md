# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A generative-art sketchbook for the **Waveshare ESP32-S3-LCD-1.47B** (ESP32-S3R8, 172x320 ST7789 IPS over SPI, 8MB octal PSRAM). Sketches are written in the style of canvas-sketch/p5 pieces and ported to C++ on LovyanGFX.

One `.cpp` per sketch in `src/`, shared header-only modules in `src/shared/`:

| Sketch | Env prefix | Motion |
|---|---|---|
| `src/main.cpp` | `native` / `esp32` / `arc_shot` | still |
| `src/arc_tiles_shake.cpp` | `shake_*` | still, but shaking the board (QMI8658 IMU) redraws it; click the window on SDL |
| `src/collapse.cpp` | `collapse_*` | animated, 6s loop |
| `src/trochoidal_wave.cpp` | `wave_*` | animated, 4s loop |
| `src/terminal_charts.cpp` | `charts_*` | animated, 8s scroll loop |
| `src/skeleton_line.cpp` | `skeleton_*` | animated, 8s loop |
| `src/switcher.cpp` | `switcher_*` | all of the above except `main.cpp`, in one binary; BOOT cycles |

Each env sets `build_src_filter` to pick exactly one file. **A new sketch needs its own env trio plus a filter** — without one, PlatformIO compiles every `.cpp` in `src/` into a single binary and the duplicate `setup()`/`loop()` fail to link. Headers in `src/shared/` are never compiled directly, so they don't need filtering.

## The two buttons

**No sketch regenerates on a timer.** A composition holds until you ask for another, and the two buttons ask different questions:

| Button | Does |
|---|---|
| BOOT (GPIO0) | next sketch, wrapping — switcher only. A click in the bottom strip of the SDL window stands in |
| RESET | reboot, redraw the *same* sketch from a new seed |

RESET has no code behind it and can't have any: it's wired to EN, so software never sees the press. Everything it does falls out of the reboot — every `setup()` calls `generate(newSeed())`. **A new sketch gets this for free and must not add a timer to "help".**

Two things make that work, both in `platform.h`:

- **`newSeed()` mixes `esp_random()` with a boot counter in NVS.** `esp_random()` alone isn't enough: with the RF subsystem off, the bootloader's entropy source is switched off again before the app starts, so one boot's sequence isn't promised to differ from the last one's — and a RESET that returns the composition you pressed the button to escape is the one failure this feature has. The counter is read once per boot, not per call, or the switcher would write flash on every press.
- **`persistGet`/`persistPut` keep the switcher's roster position in NVS**, so RESET resumes where you were instead of dropping back to sketch 1 — otherwise the buttons fight, since you couldn't reseed without losing your place. It has to be flash: RESET pulls EN, which is a power-on reset and takes RTC memory with it. The stored index is clamped on read, because it outlives the binary that wrote it.

Off-device both persist calls are no-ops. **SDL has no RESET stand-in** — relaunch the binary, which is why its `main()` seeds `rand()` from the clock. The headless `main()` deliberately does the opposite and seeds from `argv`, so captures reproduce.

## The switcher

`src/switcher.cpp` hosts five sketches in one binary and cycles with BOOT (GPIO0), or a click in the bottom strip of the SDL window. The strip is deliberately not the whole window: `imu.h` already reads a click *anywhere* as a shake, and a binary carrying both needs the gestures distinguishable.

It works by including the `shared/` headers at global scope first, then `#include`ing each sketch's `.cpp` inside its own `namespace`. Every shared header is `#pragma once`, so a sketch's own re-includes are no-ops and it binds to the global definitions — which means one `lcd`, one `cv`, and **one** 110KB framebuffer shared by all five rather than five that wouldn't fit. Only the sketches' own symbols land in the namespaces, which is exactly the colliding set (all five define `generate()`, four define `LOOP_MS`, two define `rects[]` at different types).

Two consequences worth knowing:

- **The sketches are unmodified and still build standalone.** Nothing in them refers to the switcher.
- **A new sketch is not automatically in it.** Add a `namespace` block and a roster entry, or it silently stays out. RAM is the limit to watch: five sketches' file-scope statics already come to 128KB of the 320KB, and the sprite needs 110KB of what's left.

That 128KB is the number to defend. `skeleton_line.cpp` arrived with a 55KB coverage plane, which as a static would have taken the roster to 183KB — leaving the 110KB sprite hunting for a contiguous block in the ~119KB still free at `panelBegin()` time, which is the kind of margin that fails at boot rather than degrading. It uses `psramAlloc()` instead and costs the switcher 5.5KB. **A new sketch with a buffer this size should do the same** rather than spend internal SRAM, but only if it walks that buffer in spans — PSRAM goes through the data cache, so scan-shaped access is nearly free and random access is not.

`panelBegin()` is idempotent for this reason — a switch re-runs the incoming sketch's `setup()`, and re-initialising the SPI bus and churning a 110KB allocation each time would fragment the heap. That re-run is also what makes every entry a fresh composition: `setup()` calls `generate(newSeed())`.

Sketches keep their function-local statics across a switch (playhead cursors, frame counters). That's harmless now that nothing regenerates on a timer — a stale cursor is only a phase offset into a cyclic animation, with no deadline left for it to trip. It was not harmless before: `terminal_charts.cpp` compared `millis() / LOOP_MS` against a counter that started at 0, and since the switcher's clock keeps running while other sketches draw, re-entry always read as overdue and threw away the composition `setup()` had just built.

## Commands

```bash
pio run -e native -t exec         # build + run in an SDL2 window on the Mac (2x scaled)
pio run -e esp32 -t upload        # build + flash the board
pio device monitor -b 115200      # serial output from the board

pio run -e collapse_shot          # headless capture build...
.pio/build/collapse_shot/program OUTDIR SEED FRAMES     # ...then run it directly
```

Substitute the env prefix from the table for any other sketch.

There are no tests. The SDL target *is* the test loop — iterate there, flash only to confirm on real hardware.

Requirements: `sdl2` from Homebrew (the native env shells out to `sdl2-config`). The espressif32 toolchain is installed and every environment builds; `shake_esp32` has been flashed and verified on hardware. **The four animated sketches have never been on the board** — their device frame times are arithmetic, not measurement, and each prints a per-phase breakdown to serial so one upload settles it.

If the board won't flash: hold BOOT, tap RESET, release BOOT.

## Shared modules

`src/shared/` is header-only, so a sketch just `#include`s what it needs and stays a single translation unit. A sketch defines only `setup()` and `loop()`; everything below is supplied.

| Header | What it carries |
|---|---|
| `platform.h` | shims, both `LGFX` panel classes, `lcd`, `cv`, `W`/`H`, `panelBegin()`, `present()`, `wallMicros()`, `psramAlloc()`, `newSeed()`, `persistGet`/`persistPut`, `buttonBegin`/`buttonPressed`, and all three entry points |
| `color.h` | sRGB/oklab/oklch, `wcagContrast`, `deltaEok`, `to565`, `blend565`, `to565Dither`, `shadeL`, `paletteLightest` |
| `prng.h` | mulberry32 behind the canvas-sketch-util names (`rngRange`, `rngRangeFloor`, `rngChance`, `rngPick`, `rngShuffle`) |
| `noise.h` | 4D simplex noise — the field behind `Random.noise4D`. Call `noiseSeed()` straight after `rngSeed()`; it burns 255 draws, exactly as `Random.setSeed` does |
| `palettes.h` | clrs / auto-albers / mindful / found as `0xRRGGBB` tables, plus `randomPalette()` |
| `subtractive.h` | `generateSubtractiveColors()` — rampensau's hue ramp read as RYB-cube coordinates, i.e. `subtractive-color.ts` |
| `harmony.h` | the `tintsShades` half of `pro-color-harmonies`, output in Oklch |
| `dither.h` | Atkinson error diffusion + nearest-neighbour expand |
| `termfont.h` | 5x8 bitmap font: box-drawing, block, and randomart glyphs |
| `imu.h` | QMI8658 shake detection, with a window-click stand-in on SDL |

Set `SKETCH_TITLE` (and optionally `SKETCH_FRAMES`) *before* including `platform.h`.

**Don't sprinkle `#if defined(ARDUINO)` through a sketch body.** If a sketch needs a platform-specific API, add a shim to `platform.h`. Three ports independently wrote their own `micros()` wrapper before that rule was enforced — that's the smell.

## Rendering model

One full-frame `LGFX_Sprite cv` (172 × 320 × 2 bytes = 110KB) allocated with `setPsram(false)` so it lives in fast internal SRAM. Everything draws into `cv`; `present()` blits. Never draw straight to `lcd`, and call `present()` rather than `cv.pushSprite(0, 0)` — that indirection is what lets the headless build capture a frame.

A full-frame push costs ~22ms at 40MHz SPI, capping you near 45fps before you've drawn anything. Sketches that only change part of the screen should push dirty rects directly instead (`collapse.cpp` pushes one rect per moving blind, ~8ms), but must still route the whole frame through `present()` on the headless path, or captures come out blank.

## Headless capture

`platform.h` builds a third target per sketch. Under `-DSKETCH_HEADLESS` it allocates the sprite, skips `lcd.init()` entirely, and turns `present()` into a PNG write — stored-deflate, hand-rolled, no zlib and no image tooling. An SDL window can only be judged by a human; PNGs on disk can be read back and inspected, which is how a port gets checked.

**Count the PNGs, don't just check the exit code.** `_frameNo` counts frames rendered, not files written, so it has to advance even when a write fails or a run would never hit its target. `present()` prints `FAILED to write ...` on that path — but the run still ends with status 0, so an unwritable output directory looks exactly like success from the shell.

In that build `millis()` is a **virtual clock**: it advances only via `delay()` plus a fixed 16ms per `loop()`. That's what keeps a capture off the real clock — but it means `millis()` cannot time a frame. Use `wallMicros()` for that.

**A capture is only reproducible if nothing feeds real time back into `delay()`.** Some sketches pace themselves with `delay(FRAME_MS - spent)` where `spent` comes from `wallMicros()` — so how fast the *host* rendered decides how far the virtual clock moves, and the playhead lands somewhere new on every run. Measured over five runs of each unchanged binary:

| Deterministic — a PNG diff is a real regression test | Not — a PNG diff is noise |
|---|---|
| `arc_shot`, `shake_shot`, `collapse_shot`, `charts_shot` | `wave_shot`, `skeleton_shot`, `switcher_shot` |

`switcher_shot` is in the right-hand column only because it hosts wave and skeleton; its own logic is deterministic. **Check which column you're in before believing a diff** — single runs of the right-hand three agree often enough to look like proof and then disagree on the next run. For those, assert on something timing-independent instead: comparing `======== [n/5]` entry lines against `Seed:` lines is what actually shows no sketch regenerates behind your back. `collapse.cpp` shows the fix for the underlying problem — a constant `delay(LOOP_MS / SKETCH_FRAMES - 16)` on the headless path.

## Hardware facts that are easy to get wrong

- **Backlight is GPIO46** on the "B" revision. The older non-B board uses GPIO48, and nearly every config you'll find online (TFT_eSPI setups especially) is for the non-B.
- Panel pins: `SPI2_HOST`, sclk=40, mosi=45, dc=41, cs=42, rst=39. 40MHz write. GPIO40/45 are *not* the S3's IOMUX SPI pins, so 80MHz routes through the GPIO matrix and may corrupt pixels — drop back if it does.
- The panel is a 172-wide window into 240x320 controller memory: `offset_x = 34`, `invert = true`.
- `ARDUINO_USB_CDC_ON_BOOT=1` (set in `platformio.ini`) is what makes `Serial.print()` reach USB.
- **QMI8658 IMU is on I2C SDA=GPIO48, SCL=GPIO47** (400kHz), address 0x6B. The wiki's pinout tables cover only LCD/RGB/TF and omit this; the source is `ESP32-S3-LCD-1.47B-Demo.zip`, where the Arduino (`LVGL_Arduino/I2C_Driver.h`) and ESP-IDF (`main/I2C_Driver/I2C_Driver.h`) demos agree. Do **not** use the ESP32 core's `waveshare_esp32_s3_lcd_147` variant values (SDA=8/SCL=9) — that's generic boilerplate, not this board, and it silently fails the WHO_AM_I probe.
- **The glass corner radius is ~26px**, calibrated by eye on hardware. Deriving it from the spec (~2mm corner ÷ ~0.1mm pixel pitch) gives 20, which reads visibly square against the bezel — trust the board, not the arithmetic. A sketch that wants its composition to sit inside the curve should clip to a *concentric* rounded rect (`SCREEN_RADIUS - MARGIN`), not reuse 26 at an inset; equal radii at different insets give non-parallel curves. See `SCREEN_RADIUS`/`clipCover` in `src/arc_tiles_shake.cpp`.
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

- `arc_tiles_147b.ino` was a byte-identical copy of `src/main.cpp` and has drifted badly — `main.cpp` has since gained `MAX_COLORS` and then been split onto `src/shared/`, so the `.ino` is now a standalone snapshot of the pre-shared-module design. It still compiles on its own, which is the trap. Re-copy it or leave it alone deliberately; don't edit it as if it were live code.
- `ws_s3_lcd_147b_sketch.ino` is an earlier, separate sketch (sliding-blind system) that carries a canvas-2d-shaped shim — `ctxSave`/`ctxRestore`/`translate`/`clipRect`/`fillStyle` over a `GState` stack — plus an `oklch(L, C, h) -> RGB565` converter. Worth lifting from when a new sketch wants ctx-style structure.

## Arduino IDE mirrors

These sketches also run from the Arduino IDE, which needs `foo/foo.ino`. **Copy `src/shared/` alongside the sketch** — a sketch is no longer self-contained, and the IDE resolves `#include "shared/platform.h"` relative to `sketch.h`:

```bash
cp src/arc_tiles_shake.cpp ~/Documents/Arduino/arc_tiles_shake/sketch.h
cp -R src/shared           ~/Documents/Arduino/arc_tiles_shake/
```

A stale `shared/` next to a fresh `sketch.h` is the failure mode to watch for — it compiles, and misbehaves. Re-copy both together, every time.

**Don't paste a sketch straight into a `.ino`.** The Arduino builder auto-generates function prototypes and injects them above the type definitions they reference, so `drawCell(const GridCell&, ...)` fails with `'GridCell' does not name a type` — pointing at the definition, not the injected line. Each sketch folder is therefore a one-line `.ino` (`#include "sketch.h"`) plus the real code in `sketch.h`; headers are exempt from prototype injection, and the `#include` keeps it a single translation unit.

Board: **Waveshare ESP32-S3-LCD-1.47** (in the ESP32 core, 3.3.11+). Every Tools default is right except **USB CDC On Boot → Enabled** — that's the IDE equivalent of `-DARDUINO_USB_CDC_ON_BOOT=1`, and without it `Serial.print()` never reaches USB.

If the IDE throws `fatal error: opening dependency file ... .libsdetect.d`, that's its background indexer racing another build over `~/Library/Caches/arduino/sketches/` — **Sketch → Clean** and rebuild. A different file named each time is the tell.
