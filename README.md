# Minimal e-ink display

A Zephyr RTOS photo frame: it cycles through pictures stored in flash on an
e-paper panel, powering the SoC off between changes.

- Zephyr 4.5.0, Zephyr SDK 1.0.1
- Board: Seeed XIAO ESP32C6 (`xiao_esp32c6/esp32c6/hpcore`)

## Displays

The display is chosen at build time with `-DSHIELD=<shield>`. The shield sets
the resolution, the colour format of the pictures and the driver.

| Shield | Panel | Size | Colours |
|--------|-------|------|---------|
| `xiao_epaper_4in2_v2` | Waveshare 4.2inch e-Paper Module V2 (SSD1683) | 400x300 | black/white |
| `xiao_despi_c579_gdey0579f51` | Good Display GDEY0579F51 on a DESPI-C579 | 792x272 | black/white/yellow/red |
| `xiao_despi_c579_gdey0579f52` | Good Display GDEY0579F52 on a DESPI-C579 | 792x272 | black/white/yellow/red |

The GDEY0579F52 can use its faster refresh by adding `fast-refresh;` to the
panel node in a devicetree overlay.

## Wiring

All displays use the same XIAO pins.

| Waveshare pin | DESPI-C579 pin | XIAO pin | ESP32-C6 GPIO |
|---------------|----------------|----------|---------------|
| VCC           | 3.3V           | 3V3      |               |
| GND           | GND            | GND      |               |
| DIN           | SDI            | D10      | 18 (SPI MOSI) |
| CLK           | SCK            | D8       | 19 (SPI SCK)  |
| CS            | CS             | D3       | 21            |
| DC            | D/C            | D2       | 2             |
| RST           | RES            | D1       | 1             |
| BUSY          | BUSY           | D0       | 0             |

## Setup

This repository is the west manifest. It fetches only Zephyr and the Espressif
HAL. Zephyr 4.5 needs Python 3.12 or newer.

```sh
# in the directory that contains this repository and the Python venv
source .venv/bin/activate
west init -l minimal-eink-display
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt \
            -r modules/hal/espressif/zephyr/requirements.txt \
            -r minimal-eink-display/scripts/requirements.txt
```

## Pictures

Put JPG/PNG files in `pictures/` (ignored by git). At build time
`scripts/gen_image_pack.py` scales each one to the panel, dithers it to the
panel's colours, compresses it and the result is linked into the firmware. Pictures are shown in
filename order. With an empty folder a placeholder frame is built instead; on
the four-colour panels it is a test card with colour bars and corner markers.

Every build also writes a preview of each converted picture to
`build/image_preview/`, at the panel's resolution and in roughly the tones of
real e-paper (greyish white, muted colours). The exact look of a panel varies.

## Build and flash

```sh
cd minimal-eink-display
west build -b xiao_esp32c6/esp32c6/hpcore app -- -DSHIELD=xiao_epaper_4in2_v2
west flash
west espressif monitor
```

While the SoC is powered off its USB port is gone. To flash again, hold BOOT,
tap RESET, release BOOT.

## Configuration

Set in `app/prj.conf` or on the command line (`-- -DCONFIG_...=...`).

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_APP_SLIDE_INTERVAL_SEC` | 300 | Time each picture is shown |
| `CONFIG_APP_DEEP_SLEEP` | y | Power off between pictures; `n` stays awake and keeps the USB console |
| `CONFIG_IMAGE_PACK_SOURCE_DIR` | `pictures` | Source folder, relative to the repository root |
| `CONFIG_IMAGE_PACK_FIT_COVER` / `_CONTAIN` | cover | Crop to fill, or letterbox |
| `CONFIG_IMAGE_PACK_DITHER_FS` / `_NONE` | Floyd-Steinberg | Dithering |

## Layout

| Path | Content |
|------|---------|
| `app/` | Application logic only: the slideshow loop |
| `boards/shields/` | Panel wiring, driver settings, picture size and format |
| `drivers/display/` | Driver for the GDEY0579F51/F52 panels |
| `dts/bindings/display/` | Devicetree bindings for that driver |
| `lib/image_pack/` | Build-time picture conversion and the frame decoder |
| `lib/timed_poweroff/` | Power off and wake after a delay (ESP32 backend) |
| `scripts/gen_image_pack.py` | Picture converter |
| `tests/host/` | Host test of the converter against the C decoder |
| `west.yml` | West manifest |

Host test: `python tests/host/test_image_pack.py`

## Known limits

- Waveshare 4.2inch: Zephyr's SSD16xx driver clears the panel when it
  initialises, so each change is a flash to white followed by the picture. It
  also has no panel sleep command, so the panel controller idles between
  refreshes while the SoC is powered off.
- GDEY0579F51/F52: a refresh takes about 20 s. The panels are specified for
  0 to 40 degC; the driver does not read the panel temperature.
- Switching shields needs a clean build (`west build -p`).
