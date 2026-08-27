# Board reference: ELECROW CrowPanel 1.28" HMI Rotary Display (DHE38128D)

The round knob board: 240×240 IPS in a rotating encoder bezel. **Brought up
and hardware-verified** (`zhi_rk` flashed Aug 2026, first boot clean — panel
lit, knob direction correct, one turn = one loop, tap reseeds) — the live
config is `src/shared/boards/rotary_128.h`, tag `rk`. This file is the
underlying research, kept so facts trace to sources.

Sources, in order of authority:

- Demo code: `example/Arduino/RotaryScreen_1_28/RotaryScreen_1_28.ino` in
  <https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen>
  (branch `master`). **The demo uses LovyanGFX itself**, so the panel config
  below is lifted, not translated.
- Encoder datasheet: `Datasheet/EC3501 C15H30P3-规格书2.pdf` in the same repo.
- Wiki: <https://www.elecrow.com/wiki/CrowPanel_1.28inch-HMI_ESP32_Rotary_Display.html> —
  **its library/core versions are stale against the shipped demo** (says LVGL
  8.3.11 / core 2.0.14; the .ino uses LVGL v9 and core 3.x APIs). Trust the
  code.
- Product page: <https://www.elecrow.com/crowpanel-1-28inch-hmi-esp32-rotary-display-240-240-ips-round-touch-knob-screen.html>

Family gotcha: ELECROW sells 1.28" (this one, 240×240), 1.46" (360×360) and
2.1" (480×480) rotary displays with different panels and pins. Configs do not
transfer between them.

## Hardware facts

- **ESP32-S3R8** (bare chip, in-package PSRAM), **16MB flash**, **8MB octal
  PSRAM** — Arduino setting is explicitly "OPI PSRAM", so the
  `memory_type = qio_opi` requirement applies, same as the a18/t2.
- 1.28" round IPS, **GC9A01**, 240×240 over 4-wire SPI. Zero panel offset —
  the glass is the controller's full GRAM; the visible area is the inscribed
  circle, corners exist in memory and are cropped by the bezel.
- **CST816D** capacitive touch at **0x15**, on its own I2C bus (SDA=6, SCL=7 —
  not shared with anything; the expansion I2C connector is a separate bus on
  38/39). Same auto-sleep/NACK-while-idle behaviour as the t2's.
- **Quadrature encoder ring** (EC3501 C15H30P3) on plain GPIOs A=45, B=42,
  with a **full-press switch on GPIO41** (pull-up, active LOW).
- **No IMU, no PMU chip, no battery** (USB-C 5V in only). WS2812 ring of 5
  LEDs on GPIO48 (unused by sketches so far). "Power LED" on GPIO40.
- BOOT and RESET buttons exist; BOOT's GPIO is undocumented — GPIO0 by S3
  convention (unverified).

## Pin map

Verified from the demo `.ino` unless noted.

| LCD | GPIO | | Touch (I2C) | GPIO | | Knob | GPIO |
|---|---|---|---|---|---|---|---|
| SCLK | 10 | | SDA | 6 | | ENC A | 45 |
| MOSI | 11 | | SCL | 7 | | ENC B | 42 |
| DC | 3 | | TP_INT | 5 (unused, poll) | | PRESS | 41 |
| CS | 9 | | TP_RST | **13** | | BOOT | 0 (inferred) |
| RST | 14 | | | | | WS2812 | 48 (×5) |
| **BL** | **46** | | | | | PWR LED | 40 |

Notes that will bite if missed:

- **GPIO1 and GPIO2 must be driven HIGH before the panel works.** The demo's
  `setup()` does it first thing; neither wiki nor readme says what they gate
  (rails or enables — the schematic PDF in the repo would say). Replicated
  additively in `boardPowerBegin()`. A black panel on a running board: check
  these before the panel driver.
- **The touch reset is a real GPIO (13)** — unlike the a18/t2, whose flexes
  reset themselves. Pulse it low→high and give the CST816D ~50ms before
  probing, or the boot probe finds nothing and it looks like the t2's
  auto-sleep when it isn't.
- **Backlight GPIO46, PWM, non-inverted** (the demo's 50% duty lights the
  panel) — the same pin as the 1.47B, coincidentally. Drive circuit
  (transistor vs direct) unverified.
- The demo ships **80MHz SPI write** with DMA and PSRAM draw buffers — and
  SCLK=10 is not the S3's IOMUX FSPI clock, so that's 80 through the GPIO
  matrix, vendor-proven. The repo config starts at 40MHz per convention
  (~23ms full frame, ~43fps ceiling); 80 (~12ms) is real headroom.
- Panel config from the demo, verbatim: `invert = true`, `rgb_order = false`,
  `offset_x/y = 0`, `offset_rotation = 0`, `spi_3wire = true`,
  `bus_shared = false`, `pin_rst = 14`. No `setRotation()` call, no touch
  coordinate transform — raw touch x/y map 1:1 to the panel at rotation 0.

## The encoder (what makes this board a board)

From the EC3501 C15H30P3 datasheet in the vendor repo:

- **30 detents** per revolution at 12°±2°, 360° endless rotation.
- **"Output signal is 1 pulse per 2 detents"** — 15 full quadrature cycles
  per revolution, matching the model code (C15 = 15 pulses, H30 = 30
  detents). So **one revolution = 60 edges under 4x decode**, two per detent.
  The same datasheet's series table says "30 pulses/360°", contradicting its
  own note; the model code breaks the tie. **If one physical turn sweeps two
  loops of a sketch on hardware, the tie-break was wrong: double
  `ENC_COUNTS_PER_REV` to 120.**
- Detents align with A-phase edges ("detent position will always be aligned
  with A-OFF or ON"), and the B signal is specified to oscillate at detent
  positions — which is why `encoder.h` decodes with the 16-state table
  (invalid transitions count 0) instead of the demo's rising-edge-of-A poll.
- Datasheet recommends an RC filter on the lines; whether the board carries
  one is unverified (schematic PDF, again).
- The demo reads it by **polling from a 2ms FreeRTOS task** (rising edge of
  A, direction from B) and classifies the press switch (GPIO41, 20ms
  debounce) into single/double clicks via `attachInterrupt(CHANGE)`.
  `encoder.h` uses IRAM interrupts on both lines instead — a render pass is
  tens of ms, and a flicked knob outruns any per-frame poll.

## Framebuffer arithmetic

240 × 240 × 2 = **115,200 bytes** — the 1.47B's situation (110KB internal),
not the t2's: it fits internal SRAM alongside a sketch's statics, so
`FB_IN_PSRAM = false` and per-pixel work stays cheap. The vendor demo runs
two full-frame LVGL buffers in PSRAM over 80MHz DMA, so PSRAM placement is
proven if a roster ever crowds internal SRAM out.

## Toolchain

PlatformIO: `board = esp32-s3-devkitc-1`, `board_build.arduino.memory_type =
qio_opi`, `board_upload.flash_size = 16MB`, plus the usual
`-DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1`. Same shape as `esp32_a18` /
`esp32_t2`.

Arduino IDE (wiki lesson1): board "ESP32S3 Dev Module", Flash 16MB, PSRAM
"OPI PSRAM", USB CDC On Boot Enabled, partition "Huge APP" (or the repo's
custom `elecrow_s3` scheme).

## Bringup order

1. ~~Board header + envs~~ — done (`rotary_128.h`, `[native_rk]`/`[shot_rk]`/
   `[esp32_rk]`).
2. ~~First flash~~ — done (`zhi_rk`, Aug 2026, first boot clean). Settled:
   the GPIO1/2 power enables work, panel geometry correct at rotation 0,
   touch probes after the GPIO13 reset pulse, encoder direction matches the
   knob, and **60 counts/rev confirmed** — one physical turn is one loop.
3. Still unverified: BOOT's GPIO, what GPIO1/2 actually gate, UART connector
   pins, backlight drive circuit, which way "up" faces relative to USB.
