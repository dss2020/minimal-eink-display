# Minimal e-ink display

A Zephyr RTOS photo frame: it cycles through pictures stored in flash on a
400x300 black/white e-paper panel, powering the SoC off between changes.

- Zephyr 4.5.0, Zephyr SDK 1.0.1
- Board: Seeed XIAO ESP32C6 (`xiao_esp32c6/esp32c6/hpcore`)
- Display: Waveshare 4.2inch e-Paper Module **V2** (SSD1683), shield `xiao_epaper_4in2_v2`

## Wiring

| Module pin | XIAO pin | ESP32-C6 GPIO |
|------------|----------|---------------|
| VCC        | 3V3      |               |
| GND        | GND      |               |
| DIN        | D10      | 18 (SPI MOSI) |
| CLK        | D8       | 19 (SPI SCK)  |
| CS         | D3       | 21            |
| DC         | D2       | 2             |
| RST        | D1       | 1             |
| BUSY       | D0       | 0             |

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
`scripts/gen_image_pack.py` scales each one to the panel, dithers it to 1 bit,
compresses it and the result is linked into the firmware. Pictures are shown in
filename order. With an empty folder a placeholder frame is built instead.

The converted frames can be checked without hardware in
`build/modules/minimal-eink-display/lib/image_pack/preview/`.

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
| `boards/shields/xiao_epaper_4in2_v2/` | Panel wiring and driver settings |
| `lib/image_pack/` | Build-time picture conversion and the frame decoder |
| `lib/timed_poweroff/` | Power off and wake after a delay (ESP32 backend) |
| `scripts/gen_image_pack.py` | Picture converter |
| `tests/host/` | Host test of the converter against the C decoder |
| `west.yml` | West manifest |

Host test: `python tests/host/test_image_pack.py`

## Known limits

- Zephyr's SSD16xx driver clears the panel when it initialises, so each change
  is a flash to white followed by the picture.
- The driver has no panel sleep command; the panel controller idles between
  refreshes while the SoC is powered off.
- Black/white only.
