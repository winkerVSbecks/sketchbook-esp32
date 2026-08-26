# Board reference: Waveshare ESP32-S3-Touch-LCD-2

The 2" capacitive-touch board (240×320). **Brought up and hardware-verified**
(switcher flashed; geometry, BOOT, tap, and RESET confirmed on first boot) —
the live config is `src/shared/boards/touch_lcd_2.h`, tag `t2`. This file is
the underlying research, kept so facts trace to sources rather than the wiki.
Sources: `ESP32-S3-Touch-LCD-2-Demo.zip` (Arduino + ESP-IDF demos) and
`ESP32-S3-Touch-LCD-2-SchDoc.pdf`, both under
`https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2/`. The wiki itself has
no pinout table — the demo zip and schematic are the authorities, same as the
other two boards.

## Hardware facts

- ESP32-S3R8, 8MB **octal** PSRAM (demo IDF sdkconfig: `CONFIG_SPIRAM_MODE_OCT=y`),
  16MB flash (W25Q128). Same silicon as the other two boards, so the
  `memory_type = qio_opi` requirement applies here too.
- 2" IPS, **ST7789T3**, 240×320 over 4-wire SPI. The glass is the controller's
  full GRAM: **`offset_x = 0`, `offset_y = 0`** — no window offset on this
  board, unlike the 1.47B's 34 and the a18's 16.
- **CST816D** capacitive touch at **0x15**, on the shared I2C bus. All demos
  poll it; none use the interrupt.
- **QMI8658** IMU at **0x6B** on the same bus.
- Battery: ETA6098 charger + TMI3112H buck. **No I2C PMU** — rails are
  hardwired and always on. No `boardPowerBegin()` work here; a black panel is
  not a rail problem on this board.
- Buttons: **BOOT (GPIO0) and RESET (EN) both exist.** The RESET-reseed
  gesture works exactly as on the 1.47B; touch tap can be a bonus gesture,
  not a necessity as it is on the a18.
- Camera interface (OV2640/OV5640, not included) on its own I2C bus
  (TWI_SDA=21, TWI_CLK=16) — irrelevant to sketches, but those pins are spoken
  for.

## Pin map

Verified from the demo zip's BSP headers and cross-checked against the
schematic. LCD and SD **share one SPI bus**.

| LCD | GPIO | | Touch/IMU (I2C) | GPIO | | Other | GPIO |
|---|---|---|---|---|---|---|---|
| SCLK | 39 | | SDA | 48 | | BOOT | 0 |
| MOSI | 38 | | SCL | 47 | | BAT_ADC | 5 |
| DC | 42 | | TP_INT | 46 (unused, poll) | | IMU INT1 | 3 (unused) |
| CS | 45 | | | | | SD CS | 41 |
| RST | — (none) | | | | | SD MISO | 40 |
| **BL** | **1** | | | | | | |

Notes that will bite if missed:

- **`pin_rst = -1`.** The panel reset is an RC power-on circuit on the flex;
  the schematic has a footprint (R16, unpopulated) to tie it to GPIO0, but
  shipped boards leave it NC. Software reset only.
- **Backlight GPIO1 drives an NPN transistor** — active high, and the demos
  run it as LEDC PWM at 5kHz. `Light_PWM` with `pin_bl = 1` is the LovyanGFX
  shape. Unlike the a18 there is no brightness command; unlike the 1.47B
  it's GPIO1, a pin most configs never suspect.
- **The I2C bus is SDA=48/SCL=47 — the same two GPIOs as the 1.47B's IMU
  bus**, coincidentally, so `imu.h`'s existing wiring assumptions transfer.
  Touch, IMU share it; keep `touch.h`'s polled approach (no LovyanGFX touch
  layer fighting `Wire`).
- **BAT_ADC = GPIO5** behind a 200K/100K divider: `volts = raw * 3.3f / 4096 * 3`.
- The demos clock the LCD SPI at **80MHz** and ship that way, even though
  38/39/40 route through the GPIO matrix. Start at 40MHz per repo convention;
  80 is demonstrably survivable on this board if the bandwidth is needed.

## Panel config facts (from the ESP-IDF demo's init)

- `invert = true` (IPS; `esp_lcd_panel_invert_color(panel, true)`)
- RGB element order (LovyanGFX `rgb_order = false`), no mirror, no swap_xy
- Native orientation is portrait 240×320; the demos use software rotation for
  landscape. Which way the USB port faces per rotation is **unverified** —
  that's a `board_diag.cpp` job at bringup, as it was for the a18.
- The LCD flex has no SDO line: `readable = false`, `pin_miso = -1`,
  `spi_3wire = true` — unless the SD card is in play (it never is in this
  repo), in which case the bus is shared and MISO=40 matters.

Sketch LGFX class for `src/shared/boards/`:

```cpp
class LGFX_TouchLCD2 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX_TouchLCD2(void) {
    { auto c = _bus.config();
      c.spi_host = SPI2_HOST; c.spi_mode = 0;
      c.freq_write = 40000000;            // demos ship 80MHz; try it later
      c.spi_3wire = true; c.use_lock = true; c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 39; c.pin_mosi = 38; c.pin_miso = -1; c.pin_dc = 42;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = 45; c.pin_rst = -1; c.pin_busy = -1;
      c.memory_width = 240; c.memory_height = 320;
      c.panel_width  = 240; c.panel_height  = 320;
      c.offset_x = 0; c.offset_y = 0; c.offset_rotation = 0;
      c.readable = false; c.invert = true; c.rgb_order = false;
      c.dlen_16bit = false; c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = 1; c.invert = false; c.freq = 5000; c.pwm_channel = 7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
```

## Touch: CST816D, and its one famous quirk

Register layout matches what `touch.h` already reads on the FT3168: finger
count at reg **0x02**, coordinates at 0x03–0x06, chip ID at 0xA7. So
`TOUCH_I2C_ADDR 0x15` in the board header may be all `touch.h` needs — but
verify on hardware, because **CST816-family chips auto-sleep and commonly NACK
I2C reads while untouched**. A NACK from 0x15 means "no finger", not "chip
missing". A probe-at-boot that treats NACK as absence will wrongly conclude
the chip isn't there; probe by ID register after a touch, or just treat the
address as present and NACKs as zero fingers.

On a live board the address also disambiguates: CST816D at 0x15, whereas the
a18's FT3168 sits at 0x38.

## Framebuffer arithmetic

240 × 320 × 2 = **153,600 bytes** — bigger than the 1.47B's 110KB, much
smaller than the a18's 322KB. It plausibly fits internal SRAM for a
standalone sketch, but the switcher's roster statics (128KB at 172×320,
more at this resolution) make internal placement the kind of margin that
fails at boot. Decide `FB_IN_PSRAM` at bringup by measuring free heap at
`panelBegin()` time; the safe default is `true`, and the a18 proved PSRAM
placement renders fine.

Full-frame push cost: ~31ms at 40MHz, ~15.5ms at 80MHz. At 40MHz that's a
~32fps ceiling — between the 1.47B (~45fps) and the a18 (~40fps), and another
reason the 80MHz experiment is worth running here.

## Toolchain

PlatformIO: `board = esp32-s3-devkitc-1` plus
`board_build.arduino.memory_type = qio_opi` (octal PSRAM — same fix the a18
needed; quad-init fails silently) and the usual `-DBOARD_HAS_PSRAM
-DARDUINO_USB_CDC_ON_BOOT=1`.

Arduino IDE: **there is no dedicated Waveshare variant for this board** — the
wiki itself says to select **ESP32S3 Dev Module**. That means, unlike the
1.47B, the defaults are wrong and three settings must be set by hand:
USB CDC On Boot → Enabled, PSRAM → **OPI PSRAM**, Flash Size → 16MB.

Demo library stack (for reading their code, not for use here): Arduino_GFX
(`Arduino_ST7789` + `Arduino_ESP32SPI`), lvgl, FastIMU, and a small
`bsp_cst816` touch driver — the last one is the file worth reading before
writing touch code.

## Bringup order (mirrors what worked for the a18)

1. Board header in `src/shared/boards/` (W=240, H=320, the LGFX class above,
   `FB_IN_PSRAM`, `PIN_BOOT 0`, `PIN_IMU_SDA 48`, `PIN_IMU_SCL 47`,
   `TOUCH_I2C_ADDR 0x15`), a `-DBOARD_*` flag, and per-board env sections in
   `platformio.ini`.
2. Flash `board_diag.cpp` first: confirms orientation/mirroring, reads the
   panel offset from the ruler (expected 0 — verify anyway), prints IMU raw
   bytes. The QMI8658 soft reset (0x60 ← 0xB0) is already in `imu.h`; the a18
   needed it, this one may or may not.
3. Only then port a sketch env. Board support stays opt-in per sketch — a
   composition has to work at 240×320 to earn an env.
