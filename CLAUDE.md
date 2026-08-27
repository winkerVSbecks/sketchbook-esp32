# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A generative-art sketchbook for small ESP32-S3 display boards. Sketches are written in the style of canvas-sketch/p5 pieces and ported to C++ on LovyanGFX. Boards so far:

| Tag | Board | Panel |
|---|---|---|
| `147` | Waveshare ESP32-S3-LCD-1.47B | 172x320 ST7789 IPS over SPI (ESP32-S3R8, 8MB octal PSRAM) |
| `a18` | Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1) | 368x448 SH8601 AMOLED over QSPI (ESP32-S3R8, 8MB octal PSRAM, FT3168 touch) |
| `t2` | Waveshare ESP32-S3-Touch-LCD-2 | 240x320 ST7789T3 IPS over SPI (ESP32-S3R8, 8MB octal PSRAM, CST816D touch) |
| `rk` | ELECROW CrowPanel 1.28" HMI Rotary Display | 240x240 round GC9A01 IPS over SPI (ESP32-S3R8, 8MB octal PSRAM, CST816D touch, 30-detent encoder ring + knob press) |

One `.cpp` per sketch in `src/`, shared header-only modules in `src/shared/`. Environments are named `<sketch>_<board>` for the device build, plus `_native` (SDL window) and `_shot` (headless PNG capture) suffixes:

| Sketch | Env | Motion |
|---|---|---|
| `src/main.cpp` | `arc_147*`, `arc_a18*` | still |
| `src/arc_tiles_shake.cpp` | `shake_147*` | still, but shaking the board (QMI8658 IMU) redraws it; click the window on SDL |
| `src/collapse.cpp` | `collapse_147*` | animated, 6s loop |
| `src/trochoidal_wave.cpp` | `wave_147*` | animated, 4s loop |
| `src/terminal_charts.cpp` | `charts_147*` | animated, 8s scroll loop |
| `src/skeleton_line.cpp` | `skeleton_147*` | animated, 8s loop |
| `src/jali_truchet.cpp` | `jali_147*` | still, but tilt slides the lattice (parallax); shake or BOOT redraws it. SDL: drag = tilt, bottom-strip click = redraw |
| `src/switcher.cpp` | `switcher_147*`, `switcher_a18*`, `switcher_t2*` | all of the above except `main.cpp` and `jali_truchet.cpp`, in one binary; BOOT cycles |
| `src/paint.cpp` | `paint_t2*` | still; drag paints, tap cycles the brush color (touch-first, so t2 only) |
| `src/zhi.cpp` | `zhi_t2*` | still; finger's y scrubs the breathing grids, tap redraws (touch-first, so t2 only) |
| `src/switcher_kids.cpp` | `kids_t2*` | paint + zhi in one binary; BOOT swaps, the glass belongs to the active sketch — this switcher must not read taps (see its header) |
| `src/zhi_knob.cpp` | `zhi_rk*` | still; turning the knob drives the playhead (one revolution = one loop of the breath), tap redraws (knob-first, so rk only). SDL: drag = knob |
| `src/isolines.cpp` | `iso_rk*` | still; knob drives the playhead through a noise-field contour loop (one revolution = one loop, wrap seamless), tap redraws (knob-first, so rk only). SDL: drag = knob |
| `src/circle_moire.cpp` | `moire_rk*` | still; knob slides the stripe moire (one revolution = one period, wrap seamless). No randomness in the piece, so no tap and RESET changes nothing (knob-first, so rk only). SDL: drag = knob |

Each env sets `build_src_filter` to pick exactly one file. **A new sketch needs its own env set (device + native + shot, per board it supports) plus a filter** — without one, PlatformIO compiles every `.cpp` in `src/` into a single binary and the duplicate `setup()`/`loop()` fail to link. Headers in `src/shared/` are never compiled directly, so they don't need filtering.

## Boards

A board is a header in `src/shared/boards/` supplying `W`/`H`, the device `LGFX` panel class, `FB_IN_PSRAM`, and the board's pins (`PIN_BOOT`, `PIN_IMU_*`). `platformio.ini` selects it with a `-DBOARD_*` flag via the `native_<tag>` / `shot_<tag>` / `esp32_<tag>` sections; an unflagged build defaults to the 1.47B so stray compiles and IDE indexers keep working. Sketches never include a board header or mention a board name — they draw in terms of `W` and `H`.

Native and headless are per-board too, because `W`/`H` come from the board header and a sketch has to be previewed and captured at the resolution it ships on. **Board support is opt-in per sketch: write a sketch's envs for a board only once it actually composes well at those dimensions — the env list is the support matrix**, greppable, not a promise.

`FB_IN_PSRAM` exists because framebuffer placement is a board fact: the 1.47B's 110KB frame fits in internal SRAM (fast per-pixel work), while a larger panel's frame may not fit internally at all and must go to PSRAM. `panelBegin()` reads it; nothing else should.

The AMOLED-1.8 specifics, verified on hardware (arc tiles renders correctly):

- **The unit in hand is a V1** (SH8601 panel + FT3168 touch). A V2 exists with a CO5300 panel + CST820 touch; if a board stays black on this config, swap the panel subclass in `boards/amoled_18.h` for `lgfx::Panel_CO5300` (shipped by LovyanGFX) before debugging init commands. On a live board the touch chip's I2C address tells them apart: FT3168 at 0x38, CST820 at 0x15.
- Its 322KB frame forces `FB_IN_PSRAM = true`, which is why `esp32_a18` sets `board_build.arduino.memory_type = qio_opi` — the devkitc-1 board definition defaults to quad PSRAM, these are octal, and quad-init fails silently. Suspicion worth confirming someday: `esp32_147` has the same mismatch, so `psramAlloc()` on the 1.47B probably falls back to internal heap.
- The panel driver is a tiny `Panel_SH8601_AMOLED18` subclass of LovyanGFX's `Panel_AMOLED` (the `Panel_RM690B0` pattern), QSPI on SCLK=11, data 4/5/6/7, CS=12, no reset pin. Brightness is a panel command (0x51), not a backlight pin. Pins come from the demo zip's `Mylibrary/pin_config.h`.
- **The glass maps controller columns 16..383, so `offset_x = 16`** — measured on hardware with `board_diag.cpp`'s ruler after Waveshare's demos (which ship 0) reproduced the same 16px black bar. Don't re-derive it from their code. Mounting: with USB down, the framebuffer's x axis runs vertically (x=W at the USB edge) and y runs right-to-left — remember this when mapping tilt or touch.
- **The QMI8658 needs a soft reset (0x60 <- 0xB0) before configuring or its output registers freeze** — WHO_AM_I answers, reads ack, values never change. `imu.h` does the reset now; the 1.47B happened not to need it. Found because `imuTilt()` printed the same values regardless of orientation.
- A full 368x448 QSPI push measures ~24ms on hardware (~40fps ceiling). Skeleton line's full frame is ~165ms at this resolution — the roster runs, but the animated sketches want per-board tuning before they feel right.
- `src/board_diag.cpp` (`diag_a18`) is the bringup diagnostic: corner squares for orientation/mirroring, a labeled ruler that reads out the panel's x offset, and live accel raw bytes + shake prints on serial. Flash it whenever a new board's geometry or IMU is in doubt.
- Same shared I2C bus for everything: SDA=15, SCL=14 (touch, QMI8658, AXP2101 PMU, PCF85063 RTC). Touch interrupt is GPIO21, unused — `touch.h` polls the FT3168's finger count at 0x38 instead.
- **Buttons: BOOT and PWR only — there is no RESET.** PWR belongs to the AXP2101 (power on/off), not a GPIO; leave it alone. The reseed gesture is a tap on the glass (`touch.h`), which the switcher turns into a re-entry of the active sketch.
- **The panel's power rails are OFF at a cold power-on.** They hang off the AXP2101, whose OTP defaults leave them disabled — the factory firmware enabled them, so the panel "just worked" through every warm reset until the first real unplug, then came up black while the ESP32 (on DC1, which does default on) enumerated fine. `boardPowerBegin()` in `boards/amoled_18.h` now replicates the demo's rail set (DC1/DC3 3.3V, ALDO1 1.8, ALDO2 2.8, ALDO3 3.3, ALDO4 3.0, BLDO1/2 3.3) additively — it never disables a rail, so it can't interrupt the one the chip runs on. `panelBegin()` calls it on every board; the 1.47B's is a no-op. **A black panel on a running board (USB enumerates, serial prints) is this, not the panel driver.**
- **Before porting a dirty-rect sketch** (`collapse.cpp`): these AMOLED controllers have even-coordinate alignment constraints on partial writes (documented for `Panel_RM690B0`; unverified for SH8601). Full-frame pushes are immune, which is all anything does so far.
- `switcher_a18` hosts all five roster sketches; headless capture confirms each renders at 368x448. Known composition debt: `collapse.cpp` sizes its grid for 172x320 and doesn't cover the AMOLED's canvas — it paints its background only under its own grid, so the previous sketch's pixels stay visible around it. The switcher's internal-SRAM statics grow to 241KB at this resolution, which is fine *here* because the sprite is in PSRAM — don't read the 1.47B's 128KB budget discussion as applying to this board.

The Touch-LCD-2 specifics, verified on hardware (`switcher_t2` flashed; geometry, BOOT, tap, and RESET all confirmed on first boot):

- Pins and panel config live in `boards/touch_lcd_2.h`, sourced from `ESP32-S3-Touch-LCD-2-Demo.zip` + the schematic (the wiki has no pinout table). Full research: `.claude/skills/port-canvas-sketch-to-esp32/references/board-touch-lcd-2.md`.
- The friendly board: **zero panel offset** (the glass is the ST7789T3's full 240x320 GRAM), no reset pin (RC circuit on the flex), **BOOT and RESET both exist**, and **no PMU** — rails are hardwired, so a black panel here is never a power-rail problem. `boardPowerBegin()` is a no-op.
- **Backlight is GPIO1** via an NPN transistor, PWM. Not GPIO46 — that's TP_INT here, unused.
- I2C is SDA=48/SCL=47 — the same two GPIOs as the 1.47B's IMU bus, coincidentally. CST816D touch at 0x15, QMI8658 at 0x6B. **CST816s auto-sleep and NACK while untouched** — `TOUCH_NACKS_WHEN_IDLE = true` tells `touchBegin()` a failed boot probe means "asleep", not "absent". Only `no touch controller at 0x15` on serial is a real failure.
- `FB_IN_PSRAM = true`: the 150KB frame plus the switcher's statics won't reliably fit internal SRAM, and the a18 proved PSRAM placement renders fine. `esp32_t2` sets `qio_opi` for the same reason `esp32_a18` does.
- SPI runs at the repo's proven 40MHz (~31ms full-frame push, ~32fps ceiling). Waveshare's demos ship 80MHz on these same GPIO-matrix pins (~15ms), so that headroom is real if a sketch needs it — just unexercised here.

The CrowPanel 1.28" Rotary (rk) specifics, verified on hardware (`zhi_rk` flashed Aug 2026, first boot clean: panel, knob direction, one-turn-one-loop, and tap all confirmed; full research with sources: `.claude/skills/port-canvas-sketch-to-esp32/references/board-rotary-128.md`):

- Pins in `boards/rotary_128.h`, lifted from ELECROW's own demo — which uses LovyanGFX itself, so the GC9A01 config is verbatim, not translated. Zero panel offset; the visible area is the circle inscribed in the 240x240 GRAM, corners crop under the bezel.
- **GPIO1 and GPIO2 must be driven HIGH before the panel works** — undocumented enables the vendor demo sets first thing; `boardPowerBegin()` replicates them additively. A black panel here: check these before the panel driver.
- **The CST816D's reset is a real GPIO (13)**, unlike the a18/t2 flexes which reset themselves — `boardPowerBegin()` pulses it and waits ~60ms so the boot probe doesn't mistake a chip still in reset for the usual CST816 auto-sleep. Touch I2C is SDA=6/SCL=7 (carried as `PIN_IMU_*`; there is no IMU on this board).
- **The encoder ring is 30 detents, 15 quadrature cycles per revolution → `ENC_COUNTS_PER_REV = 60`** (EC3501 C15H30P3 datasheet; its own series table said 120 — the model code broke the tie, and hardware confirmed 60: one physical turn is one loop of zhi's breath). `encoder.h` decodes 4x with IRAM interrupts on A=45/B=42; the knob's full-press is GPIO41 (`PIN_KNOB_BTN`, active low), deliberately not `PIN_BOOT`.
- 115KB framebuffer, so `FB_IN_PSRAM = false` — the 1.47B's situation, not the t2's. Vendor demo proves PSRAM framebuffers + 80MHz SPI also work if a roster ever needs the internal SRAM back; the header ships the repo's proven 40MHz (~23ms/frame).

## The two buttons

**No sketch regenerates on a timer.** A composition holds until you ask for another, and the two buttons ask different questions:

| Button | Does |
|---|---|
| BOOT (GPIO0) | next sketch, wrapping — switcher only. A click in the bottom strip of the SDL window stands in. Standalone `jali_truchet` repurposes it as redraw, which it can only do because it isn't in the roster |
| RESET | reboot, redraw the *same* sketch from a new seed. **The AMOLED-1.8 has no RESET button** (only BOOT and PWR, and PWR belongs to the AXP2101) — there, a **tap on the glass** is the reseed gesture instead, via `touch.h`: the switcher re-enters the active sketch, whose `setup()` draws fresh from `newSeed()`, no reboot and no flash write. The Touch-LCD-2 has both: RESET reboots, tap reseeds without one. No SDL stand-in — clicks are already spoken for (shake + BOOT strip); relaunch the binary |

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

`src/switcher_kids.cpp` (`kids_t2*`) is a second, two-sketch roster built the same way — paint + zhi for small hands. One deliberate difference: **it polls only BOOT and never reads taps**, because both hosted sketches own the glass (paint's tap cycles the brush color; zhi classifies tap-vs-scrub itself), and a switcher-level `tapDetected()` racing paint's press/release timing would wipe a drawing on every crayon change. Its roster position persists under its own NVS key (`kidsketch`), so it and `switcher_t2` don't resume into each other's index when flashed over one another. Swapping away from paint discards the drawing (the incoming `setup()` repaints the shared framebuffer) — inherent to the roster model, not a bug.

## Commands

```bash
pio run -e arc_147_native -t exec       # build + run in an SDL2 window on the Mac (2x scaled)
pio run -e arc_147 -t upload            # build + flash the board (or scripts/flash.sh <sketch>)
pio device monitor -b 115200            # serial output from the board

pio run -e collapse_147_shot            # headless capture build...
.pio/build/collapse_147_shot/program OUTDIR SEED FRAMES     # ...then run it directly
```

Substitute any sketch/board pair from the table.

There are no tests. The SDL target *is* the test loop — iterate there, flash only to confirm on real hardware.

Requirements: `sdl2` from Homebrew (the native env shells out to `sdl2-config`). The espressif32 toolchain is installed and every environment builds; `shake_147`, `arc_a18`, and `switcher_t2` have been flashed and verified on hardware. **The four animated sketches have never been on the board standalone** — their device frame times are arithmetic, not measurement, and each prints a per-phase breakdown to serial so one upload settles it.

If the board won't flash: hold BOOT, tap RESET, release BOOT.

## Shared modules

`src/shared/` is header-only, so a sketch just `#include`s what it needs and stays a single translation unit. A sketch defines only `setup()` and `loop()`; everything below is supplied.

| Header | What it carries |
|---|---|
| `platform.h` | shims, the SDL panel class, board selection, `lcd`, `cv`, `panelBegin()`, `present()`, `wallMicros()`, `psramAlloc()`, `newSeed()`, `persistGet`/`persistPut`, `buttonBegin`/`buttonPressed`, and all three entry points |
| `boards/*.h` | one per board, included by `platform.h` only: `W`/`H`, the device `LGFX` panel class, `FB_IN_PSRAM`, `PIN_BOOT`, `PIN_IMU_*` |
| `color.h` | sRGB/oklab/oklch, `wcagContrast`, `deltaEok`, `to565`, `blend565`, `to565Dither`, `shadeL`, `paletteLightest` |
| `prng.h` | mulberry32 behind the canvas-sketch-util names (`rngRange`, `rngRangeFloor`, `rngChance`, `rngPick`, `rngShuffle`) |
| `noise.h` | 4D simplex noise — the field behind `Random.noise4D`. Call `noiseSeed()` straight after `rngSeed()`; it burns 255 draws, exactly as `Random.setSeed` does |
| `palettes.h` | clrs / auto-albers / mindful / found as `0xRRGGBB` tables, plus `randomPalette()` |
| `subtractive.h` | `generateSubtractiveColors()` — rampensau's hue ramp read as RYB-cube coordinates, i.e. `subtractive-color.ts` |
| `harmony.h` | the `tintsShades` half of `pro-color-harmonies`, output in Oklch |
| `dither.h` | Atkinson error diffusion + nearest-neighbour expand |
| `termfont.h` | 5x8 bitmap font: box-drawing, block, and randomart glyphs |
| `imu.h` | QMI8658 shake detection and `imuTilt()` (the gravity vector, for parallax), with click / drag stand-ins on SDL and a deterministic drift fallback |
| `encoder.h` | cumulative rotation from a quadrature knob (`encoderBegin`, `encoderCount`, `encoderRev`), for boards that define `PIN_ENC_A`/`PIN_ENC_B`/`ENC_COUNTS_PER_REV` — a board without them fails the compile, which is the support matrix working. IRAM interrupt decode on device (a per-frame poll drops edges on a flicked knob); SDL stand-in is a vertical mouse drag, one window height per revolution, so a knob sketch must not also read clicks as anything else. Headless reads 0 and sketches sweep their own playhead |
| `touch.h` | `tapDetected()` on boards with a touch controller (`TOUCH_I2C_ADDR` in the board header; FT3168 on the a18, CST816D on the t2). Polled finger count over the shared `Wire` bus — deliberately not LovyanGFX's touch layer, which would fight `Wire` for the peripheral. CST816s auto-sleep and NACK while untouched, so `TOUCH_NACKS_WHEN_IDLE` in the board header keeps a failed boot probe from disabling tap. No SDL stand-in |

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
| `arc_147_shot`, `shake_147_shot`, `collapse_147_shot`, `charts_147_shot` | `wave_147_shot`, `skeleton_147_shot`, `switcher_147_shot` |

`switcher_147_shot` is in the right-hand column only because it hosts wave and skeleton; its own logic is deterministic. **Check which column you're in before believing a diff** — single runs of the right-hand three agree often enough to look like proof and then disagree on the next run. For those, assert on something timing-independent instead: comparing `======== [n/5]` entry lines against `Seed:` lines is what actually shows no sketch regenerates behind your back. `collapse.cpp` shows the fix for the underlying problem — a constant `delay(LOOP_MS / SKETCH_FRAMES - 16)` on the headless path.

## Hardware facts that are easy to get wrong

- **Backlight is GPIO46** on the "B" revision. The older non-B board uses GPIO48, and nearly every config you'll find online (TFT_eSPI setups especially) is for the non-B.
- Panel pins: `SPI2_HOST`, sclk=40, mosi=45, dc=41, cs=42, rst=39. 40MHz write. GPIO40/45 are *not* the S3's IOMUX SPI pins, so 80MHz routes through the GPIO matrix and may corrupt pixels — drop back if it does.
- The panel is a 172-wide window into 240x320 controller memory: `offset_x = 34`, `invert = true`.
- `ARDUINO_USB_CDC_ON_BOOT=1` (set in `platformio.ini`) is what makes `Serial.print()` reach USB.
- **QMI8658 IMU is on I2C SDA=GPIO48, SCL=GPIO47** (400kHz), address 0x6B — carried as `PIN_IMU_*` in `boards/lcd_147b.h`. The wiki's pinout tables cover only LCD/RGB/TF and omit this; the source is `ESP32-S3-LCD-1.47B-Demo.zip`, where the Arduino (`LVGL_Arduino/I2C_Driver.h`) and ESP-IDF (`main/I2C_Driver/I2C_Driver.h`) demos agree. Do **not** use the ESP32 core's `waveshare_esp32_s3_lcd_147` variant values (SDA=8/SCL=9) — that's generic boilerplate, not this board, and it silently fails the WHO_AM_I probe.
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
