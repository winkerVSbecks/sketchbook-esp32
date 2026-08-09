---
name: flash-sketch-to-device
description: Get a sketch from this repo running on the physical Waveshare ESP32-S3-LCD-1.47B board, via PlatformIO or the Arduino IDE, and confirm it actually works. Use whenever the user wants to flash, upload, deploy, or "put this on the board", asks how to run a sketch on the device or on hardware, is picking Arduino IDE Tools settings for this board, or is debugging a failed upload, a blank panel, missing serial output, or an on-device behaviour that works fine in the SDL build.
---

# Flashing a sketch to the ESP32-S3-LCD-1.47B

Two routes exist and they are not equivalent. Pick deliberately.

| | PlatformIO | Arduino IDE |
|---|---|---|
| Board config | in `platformio.ini`, diffable | clicked into menus, invisible to git |
| Source | `src/*.cpp` directly | needs a mirrored sketch folder |
| Use when | this is the repo's default | the user is already in the IDE, or PlatformIO's toolchain isn't installed |

**Default to PlatformIO.** Only use the Arduino IDE when the user asks for it or is already working there.

## Before flashing: iterate on SDL

The SDL target is the test loop. A device flash takes ~1 minute plus a reconnect; SDL takes seconds and catches every logic bug. Flash to confirm physical properties — panel orientation, colour, backlight, sensors — not to find bugs you could have found on the desktop.

```bash
pio run -e shake_native -t exec    # or -e native
```

## Route 1: PlatformIO

Use `scripts/flash.sh` rather than composing the commands by hand:

```bash
scripts/flash.sh                        # list environments and their sources
scripts/flash.sh arc_tiles_shake.cpp    # resolve to its esp32 env, build + upload
scripts/flash.sh shake_esp32            # or name the environment directly
scripts/flash.sh <target> --build       # build only, don't touch the board
```

It resolves the environment from `pio project config`, refuses ambiguous or unknown targets, and checks for a port before uploading. That resolution step is the whole point: environments carry a `build_src_filter`, so a wrong `-e` still builds and uploads perfectly happily — just the wrong sketch, with no error. Never hardcode env names; they change as sketches are added.

The first ESP32 build downloads the espressif32 toolchain (~1GB, several minutes). It's a one-time cost, but don't kick it off silently while the user waits — say it's happening.

```bash
pio device monitor -b 115200       # serial output; tap RESET to catch boot lines
```

## Route 2: Arduino IDE

### Never paste a sketch into a bare `.ino`

The Arduino builder auto-generates function prototypes and injects them near the top of the file — above the type definitions they reference. A sketch with a struct used in a function signature fails with:

```
error: 'GridCell' does not name a type
```

pointing at the *definition*, not the injected prototype, which makes it look like a scoping bug in otherwise-valid C++.

The fix is a sketch folder holding a one-line `.ino` plus the real code in a header, since headers are exempt from prototype injection and `#include` keeps it one translation unit:

```
~/Documents/Arduino/<name>/
├── <name>.ino     →  #include "sketch.h"
└── sketch.h       →  cp of the repo's src/<sketch>.cpp
```

The copy drifts. Re-copy before every flash, and note the destination is `sketch.h`, not the `.ino`.

### Board and Tools settings

Select **esp32 → Waveshare ESP32-S3-LCD-1.47** (present in ESP32 core 3.3.11+). Don't reach for "ESP32S3 Dev Module" — the real definition already carries the correct flash size, octal PSRAM, and USB IDs.

Every default is correct except one:

| Setting | Value | |
|---|---|---|
| **USB CDC On Boot** | **Enabled** | ← the only change. Equivalent to `-DARDUINO_USB_CDC_ON_BOOT=1`; without it `Serial.print()` never reaches USB |
| PSRAM | OPI PSRAM / Enabled | default — the S3**R8** is octal, QSPI won't boot |
| Flash Size | 16MB | default |
| USB Mode | Hardware CDC and JTAG | default |
| Upload Speed | 921600 | drop to 115200 if uploads fail |

To compile without the GUI (useful for verifying a change actually builds):

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" compile --fqbn "esp32:esp32:waveshare_esp32_s3_lcd_147:CDCOnBoot=cdc" \
  --build-path /tmp/build ~/Documents/Arduino/<name>
```

Pass `--build-path`. Without it the CLI shares `~/Library/Caches/arduino/sketches/` with the IDE's background indexer, and concurrent access produces a spurious `fatal error: opening dependency file ... .libsdetect.d: No such file or directory`. A *different* file named on each run is the tell that it's a race, not a real error. In the GUI the fix is **Sketch → Clean**.

## Verifying it worked

**Ask the user; don't script the serial capture.** They are sitting in front of the board and can read the monitor instantly. Automated capture is slow and brittle — pyserial generally isn't installed, the boot lines are usually gone by the time a monitor attaches, and for a gesture-driven sketch nothing can produce the input anyway, since someone has to physically shake the board.

Ask a question that names the exact string to look for, not "did it work?". Everything that doesn't need hardware — SDL runs, compiles, build output — should still be verified directly, without asking.

Serial at 115200 is the only real feedback channel — the panel shows output, not state.

For a sensor-driven sketch, confirm the sensor was found *before* debugging the gesture. These are different failures with different fixes:

```
QMI8658 at 0x6B        ← sensor present; any problem is threshold or logic
QMI8658 not found      ← wiring/pins; no amount of threshold tuning will help
```

## Troubleshooting

**No port in `/dev/cu.*` or upload fails** — hold **BOOT**, tap **RESET**, release **BOOT** to force download mode. Native USB re-enumerates after each upload, so the port often needs reselecting.

**Blank screen, board otherwise alive** — backlight is **GPIO46** on the B revision. GPIO48 is the non-B, and nearly every config online is for the non-B.

**Works on SDL, wrong on device** — a physical panel property, not logic. See the four-symptom table in `port-canvas-sketch-to-esp32/references/board-and-toolchain.md`: photo negative → `invert`, red/blue swapped → `rgb_order`, shifted with a garbage stripe → `offset_x` must be 34.

**Compiles in the Arduino IDE but fails under PlatformIO** — the two use different Arduino core versions: the IDE is on 3.x, while PlatformIO's `espressif32` platform ships core **2.0.17**. Core 2.x's `binary.h` `#define`s `B0`, `B1`, `B01`, `B10` … as binary literals, so an innocuous local like `float B1` explodes with:

```
binary.h:31: error: expected unqualified-id before numeric constant
```

reported against `binary.h`, with cascading "not declared in this scope" errors on the *following* names. Rename the identifier (lowercase is easiest); don't `#undef`. Any single-capital-letter-plus-binary-digits name is a landmine here.

**Sensor not found** — don't trust the ESP32 core's variant `pins_arduino.h` for peripherals; its `SDA`/`SCL` are generic boilerplate. The authoritative source is Waveshare's demo package, which is not the same as the wiki (whose pinout tables omit the IMU entirely):

```bash
curl -sLA "Mozilla/5.0" -o demo.zip \
  https://files.waveshare.com/wiki/ESP32-S3-LCD-1.47B/ESP32-S3-LCD-1.47B-Demo.zip
unzip -q demo.zip -d demo && grep -rn "SDA\|SCL" demo/Arduino/examples/*/I2C_Driver.h
```

Verified from that package: **I2C SDA=GPIO48, SCL=GPIO47**, QMI8658 at **0x6B**.

## Don't claim it works because it compiled

A clean build proves the code is valid C++, nothing more. Pin assignments, sensor presence, and panel geometry are only ever confirmed by the board itself. When you haven't seen hardware output, say so plainly and tell the user which serial line will confirm it — don't let a successful compile stand in for a working device.
