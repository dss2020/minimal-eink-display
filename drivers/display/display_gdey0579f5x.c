/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Good Display GDEY0579F51 / GDEY0579F52: 5.79 inch, 792x272,
 * black/white/yellow/red e-paper panels.
 *
 * Each panel has two controllers behind one chip select. The master drives
 * the left half and the slave the right half, 396 columns each. The select
 * command routes what follows to one of them or to both.
 *
 * The panels must be re-initialised for every refresh and put to sleep
 * afterwards, so the driver keeps the picture in RAM and sends all of it
 * when a refresh is due.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gdey0579f5x, CONFIG_DISPLAY_LOG_LEVEL);

#define CMD_PSR    0x00
#define CMD_POF    0x02
#define CMD_PON    0x04
#define CMD_DSLP   0x07
#define CMD_DTM    0x10
#define CMD_DRF    0x12
#define CMD_SELECT 0xA2

#define SELECT_BOTH   0x00
#define SELECT_MASTER 0x01
#define SELECT_SLAVE  0x02

#define DSLP_CHECK 0xA5

/* Pixel values of the panel, also the palette index of PIXEL_FORMAT_I_4. */
#define COLOR_BLACK  0
#define COLOR_WHITE  1
#define COLOR_YELLOW 2
#define COLOR_RED    3
#define NUM_COLORS   4

#define PIXELS_PER_BYTE 4

#define RESET_PULSE_MS     40
#define RESET_WAIT_MS      50
#define BUSY_POLL_MS       10
#define BUSY_SETTLE_MS     20
#define BUSY_TIMEOUT_MS    5000
#define REFRESH_TIMEOUT_MS 60000

/*
 * Init sequences are a list of items: a count of data bytes, the command
 * and its data. Two counts are reserved for waiting.
 */
#define SEQ_WAIT_READY 0xFF /* wait until the panel is not busy */
#define SEQ_DELAY      0xFE /* followed by a delay in ms */

#define SEQ_SIZE_396 0x01, 0x8C /* columns per controller */
#define SEQ_SIZE_272 0x01, 0x10 /* rows */

static const uint8_t seq_f51[] __maybe_unused = {
	1, CMD_SELECT, SELECT_MASTER,
	2, CMD_PSR, 0x07, 0x29,
	1, CMD_SELECT, SELECT_SLAVE,
	2, CMD_PSR, 0x03, 0x29,
	1, CMD_SELECT, SELECT_BOTH,
	4, 0x06, 0x32, 0x32, 0x32, 0x32,                 /* booster soft start */
	4, 0x61, SEQ_SIZE_396, SEQ_SIZE_272,             /* resolution */
	1, 0x50, 0x97,                                   /* white border */
	1, 0xE9, 0x01,
	1, 0xE0, 0x01,
	0, CMD_PON,
	SEQ_WAIT_READY,
};

/* Everything after the waveform selection is common to full and fast mode. */
#define SEQ_F52_COMMON                                                                             \
	1, CMD_SELECT, SELECT_MASTER,                                                              \
	2, CMD_PSR, 0x07, 0x29,                                                                    \
	1, CMD_SELECT, SELECT_BOTH,                                                                \
	1, CMD_SELECT, SELECT_SLAVE,                                                               \
	2, CMD_PSR, 0x03, 0x29,                                                                    \
	1, CMD_SELECT, SELECT_BOTH,                                                                \
	6, 0x01, 0x07, 0x00, 0x26, 0x78, 0x24, 0x26,     /* power setting */                       \
	3, 0x06, 0xC0, 0xC0, 0xC0,                       /* booster soft start */                  \
	1, 0x50, 0x97,                                   /* white border */                        \
	4, 0x61, SEQ_SIZE_396, SEQ_SIZE_272,             /* resolution */                          \
	4, 0x65, 0x00, 0x00, 0x00, 0x00,                 /* gate/source start */                   \
	1, 0xE3, 0x77,                                   /* power saving */                        \
	1, 0xFF, 0xA5,                                                                             \
	8, 0xEF, 1, 30, 6, 30, 14, 28, 18, 16,           /* PWM */                                 \
	1, 0xDB, 0x00,                                                                             \
	1, 0xCF, 0x00,                                                                             \
	1, 0xDF, 0x00,                                                                             \
	1, 0xFD, 0x01,                                                                             \
	1, 0xE8, 0x03,                                                                             \
	1, 0xDC, 0x00,                                                                             \
	1, 0xDD, 10,                                                                               \
	1, 0xDE, 60,                                                                               \
	1, 0xFF, 0xE3,                                                                             \
	1, 0xE9, 0x01,                                                                             \
	0, CMD_PON,                                                                                \
	SEQ_WAIT_READY

/*
 * The vendor code reads the panel temperature first and skips the 0xA5 step
 * below 0 degC. The panel is only specified for 0 to 40 degC and reading
 * needs a bidirectional data line, so that case is left out.
 */
static const uint8_t seq_f52_full[] __maybe_unused = {
	1, 0xE0, 0x01,
	SEQ_DELAY, 10,
	0, 0xA5,
	SEQ_DELAY, 10,
	SEQ_WAIT_READY,
	SEQ_F52_COMMON,
};

static const uint8_t seq_f52_fast[] __maybe_unused = {
	1, 0xE6, 60,
	1, 0xE0, 0x03,
	SEQ_DELAY, 2,
	0, 0xA5,
	SEQ_DELAY, 10,
	SEQ_WAIT_READY,
	SEQ_F52_COMMON,
};

struct gdey0579f5x_config {
	const struct device *mipi_dev;
	struct mipi_dbi_config dbi_config;
	struct gpio_dt_spec busy_gpio;
	uint16_t width;
	uint16_t height;
	const uint8_t *init_seq;
	size_t init_seq_len;
	/* Picture as the controllers take it: master half, then slave half. */
	uint8_t *fb;
};

struct gdey0579f5x_data {
	bool blanking_on;
};

static const struct display_palette_color palette[NUM_COLORS] = {
	[COLOR_BLACK] = {.r = 0, .g = 0, .b = 0, .a = 0xFF},
	[COLOR_WHITE] = {.r = 0xFF, .g = 0xFF, .b = 0xFF, .a = 0xFF},
	[COLOR_YELLOW] = {.r = 0xFF, .g = 0xEB, .b = 0, .a = 0xFF},
	[COLOR_RED] = {.r = 0xD7, .g = 0x19, .b = 0x19, .a = 0xFF},
};

BUILD_ASSERT(CONFIG_DISPLAY_COLOR_PALETTE_MAX_SIZE >= NUM_COLORS);

static inline size_t half_row_bytes(const struct gdey0579f5x_config *config)
{
	return config->width / 2 / PIXELS_PER_BYTE;
}

static inline size_t half_bytes(const struct gdey0579f5x_config *config)
{
	return half_row_bytes(config) * config->height;
}

static inline int write_cmd(const struct device *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
	const struct gdey0579f5x_config *config = dev->config;

	return mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);
}

static inline int write_cmd_uint8(const struct device *dev, uint8_t cmd, uint8_t data)
{
	return write_cmd(dev, cmd, &data, 1);
}

static int wait_ready(const struct device *dev, uint32_t timeout_ms)
{
	const struct gdey0579f5x_config *config = dev->config;
	int64_t end = k_uptime_get() + timeout_ms;
	int busy;

	/* The panel takes a moment to assert BUSY after a command. */
	k_msleep(BUSY_SETTLE_MS);

	while ((busy = gpio_pin_get_dt(&config->busy_gpio)) != 0) {
		if (busy < 0) {
			return busy;
		}
		if (k_uptime_get() > end) {
			LOG_ERR("Panel still busy after %u ms", timeout_ms);
			return -ETIMEDOUT;
		}
		k_msleep(BUSY_POLL_MS);
	}

	return 0;
}

static int run_sequence(const struct device *dev, const uint8_t *seq, size_t seq_len)
{
	size_t i = 0;
	int ret;

	while (i < seq_len) {
		uint8_t len = seq[i++];

		if (len == SEQ_WAIT_READY) {
			ret = wait_ready(dev, BUSY_TIMEOUT_MS);
		} else if (len == SEQ_DELAY) {
			k_msleep(seq[i++]);
			ret = 0;
		} else {
			uint8_t cmd = seq[i++];

			ret = write_cmd(dev, cmd, len ? &seq[i] : NULL, len);
			i += len;
		}

		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

/* Wake the panel, send the picture, refresh and put the panel back to sleep. */
static int refresh(const struct device *dev)
{
	const struct gdey0579f5x_config *config = dev->config;
	int ret;

	ret = mipi_dbi_reset(config->mipi_dev, RESET_PULSE_MS);
	if (ret < 0) {
		return ret;
	}
	k_msleep(RESET_WAIT_MS);

	ret = wait_ready(dev, BUSY_TIMEOUT_MS);
	if (ret < 0) {
		return ret;
	}

	ret = run_sequence(dev, config->init_seq, config->init_seq_len);
	if (ret < 0) {
		return ret;
	}

	ret = write_cmd_uint8(dev, CMD_SELECT, SELECT_MASTER);
	if (ret < 0) {
		return ret;
	}
	ret = write_cmd(dev, CMD_DTM, config->fb, half_bytes(config));
	if (ret < 0) {
		return ret;
	}

	ret = write_cmd_uint8(dev, CMD_SELECT, SELECT_SLAVE);
	if (ret < 0) {
		return ret;
	}
	ret = write_cmd(dev, CMD_DTM, config->fb + half_bytes(config), half_bytes(config));
	if (ret < 0) {
		return ret;
	}

	ret = write_cmd_uint8(dev, CMD_SELECT, SELECT_BOTH);
	if (ret < 0) {
		return ret;
	}
	ret = write_cmd_uint8(dev, CMD_DRF, 0x01);
	if (ret < 0) {
		return ret;
	}
	ret = wait_ready(dev, REFRESH_TIMEOUT_MS);
	if (ret < 0) {
		return ret;
	}

	ret = write_cmd_uint8(dev, CMD_POF, 0x00);
	if (ret < 0) {
		return ret;
	}
	ret = wait_ready(dev, BUSY_TIMEOUT_MS);
	if (ret < 0) {
		return ret;
	}

	return write_cmd_uint8(dev, CMD_DSLP, DSLP_CHECK);
}

static void set_pixel(const struct gdey0579f5x_config *config, uint16_t x, uint16_t y,
		      uint8_t color)
{
	const uint16_t half_width = config->width / 2;
	uint8_t *half = config->fb;
	unsigned int shift;
	size_t row;
	uint8_t *p;

	if (x >= half_width) {
		x -= half_width;
		half += half_bytes(config);
	}

	/*
	 * The controllers take the rows from the outside in: last row, first
	 * row, last but one, second, and so on.
	 */
	if (y < config->height / 2) {
		row = 2U * y + 1U;
	} else {
		row = 2U * (config->height - 1U - y);
	}

	p = &half[row * half_row_bytes(config) + x / PIXELS_PER_BYTE];
	shift = 6U - 2U * (x % PIXELS_PER_BYTE);
	*p = (*p & ~(0x3U << shift)) | ((color & 0x3U) << shift);
}

static int gdey0579f5x_write(const struct device *dev, const uint16_t x, const uint16_t y,
			     const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct gdey0579f5x_config *config = dev->config;
	struct gdey0579f5x_data *data = dev->data;
	const uint8_t *src = buf;
	const size_t row_bytes = DIV_ROUND_UP(desc->pitch, 2);

	if (buf == NULL || desc->pitch < desc->width || desc->width == 0 || desc->height == 0) {
		return -EINVAL;
	}

	if (x + desc->width > config->width || y + desc->height > config->height) {
		LOG_ERR("Write outside of the panel");
		return -EINVAL;
	}

	if (desc->buf_size < row_bytes * (desc->height - 1U) + DIV_ROUND_UP(desc->width, 2)) {
		LOG_ERR("Buffer too small");
		return -EINVAL;
	}

	for (uint16_t row = 0; row < desc->height; row++) {
		const uint8_t *line = &src[row * row_bytes];

		for (uint16_t col = 0; col < desc->width; col++) {
			/* PIXEL_FORMAT_I_4: left pixel in the high nibble. */
			uint8_t index = (col & 1U) ? (line[col / 2U] & 0x0F) : (line[col / 2U] >> 4);

			set_pixel(config, x + col, y + row, index);
		}
	}

	if (!data->blanking_on) {
		return refresh(dev);
	}

	return 0;
}

static int gdey0579f5x_blanking_on(const struct device *dev)
{
	struct gdey0579f5x_data *data = dev->data;

	data->blanking_on = true;

	return 0;
}

static int gdey0579f5x_blanking_off(const struct device *dev)
{
	struct gdey0579f5x_data *data = dev->data;

	if (!data->blanking_on) {
		return 0;
	}

	data->blanking_on = false;

	return refresh(dev);
}

static void gdey0579f5x_get_capabilities(const struct device *dev,
					 struct display_capabilities *caps)
{
	const struct gdey0579f5x_config *config = dev->config;

	memset(caps, 0, sizeof(*caps));
	memcpy(caps->color_palette, palette, sizeof(palette));
	caps->x_resolution = config->width;
	caps->y_resolution = config->height;
	caps->supported_pixel_formats = PIXEL_FORMAT_I_4;
	caps->current_pixel_format = PIXEL_FORMAT_I_4;
	caps->screen_info = SCREEN_INFO_EPD;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int gdey0579f5x_set_pixel_format(const struct device *dev,
					const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);

	return pf == PIXEL_FORMAT_I_4 ? 0 : -ENOTSUP;
}

static int gdey0579f5x_init(const struct device *dev)
{
	const struct gdey0579f5x_config *config = dev->config;
	int ret;

	if (!device_is_ready(config->mipi_dev)) {
		LOG_ERR("MIPI DBI device is not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&config->busy_gpio)) {
		LOG_ERR("BUSY GPIO is not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&config->busy_gpio, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	/* White. The panel itself is left alone until the first refresh. */
	memset(config->fb, COLOR_WHITE * 0x55, 2U * half_bytes(config));

	return 0;
}

static DEVICE_API(display, gdey0579f5x_api) = {
	.blanking_on = gdey0579f5x_blanking_on,
	.blanking_off = gdey0579f5x_blanking_off,
	.write = gdey0579f5x_write,
	.get_capabilities = gdey0579f5x_get_capabilities,
	.set_pixel_format = gdey0579f5x_set_pixel_format,
};

#define GDEY0579F5X_FB_SIZE(node_id)                                                               \
	(DT_PROP(node_id, width) / PIXELS_PER_BYTE * DT_PROP(node_id, height))

#define GDEY0579F5X_DEFINE(node_id, seq)                                                           \
	BUILD_ASSERT(DT_PROP(node_id, width) % (2 * PIXELS_PER_BYTE) == 0 &&                       \
		     DT_PROP(node_id, height) % 2 == 0,                                            \
		     "width must be a multiple of 8 and height even");                             \
	static uint8_t _CONCAT(gdey0579f5x_fb_, DT_DEP_ORD(node_id))[GDEY0579F5X_FB_SIZE(node_id)];\
	static const struct gdey0579f5x_config _CONCAT(gdey0579f5x_cfg_, DT_DEP_ORD(node_id)) = {  \
		.mipi_dev = DEVICE_DT_GET(DT_PARENT(node_id)),                                     \
		.dbi_config =                                                                      \
			{                                                                          \
				.mode = MIPI_DBI_MODE_SPI_4WIRE,                                   \
				.config = MIPI_DBI_SPI_CONFIG_DT(                                  \
					node_id, SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8), 0),         \
			},                                                                         \
		.busy_gpio = GPIO_DT_SPEC_GET(node_id, busy_gpios),                                \
		.width = DT_PROP(node_id, width),                                                  \
		.height = DT_PROP(node_id, height),                                                \
		.init_seq = seq,                                                                   \
		.init_seq_len = sizeof(seq),                                                       \
		.fb = _CONCAT(gdey0579f5x_fb_, DT_DEP_ORD(node_id)),                               \
	};                                                                                         \
	static struct gdey0579f5x_data _CONCAT(gdey0579f5x_data_, DT_DEP_ORD(node_id));            \
	DEVICE_DT_DEFINE(node_id, gdey0579f5x_init, NULL,                                          \
			 &_CONCAT(gdey0579f5x_data_, DT_DEP_ORD(node_id)),                         \
			 &_CONCAT(gdey0579f5x_cfg_, DT_DEP_ORD(node_id)), POST_KERNEL,             \
			 CONFIG_DISPLAY_INIT_PRIORITY, &gdey0579f5x_api);

#define GDEY0579F51_DEFINE(node_id) GDEY0579F5X_DEFINE(node_id, seq_f51)

#define GDEY0579F52_DEFINE(node_id)                                                                \
	COND_CODE_1(DT_PROP(node_id, fast_refresh),                                                \
		    (GDEY0579F5X_DEFINE(node_id, seq_f52_fast)),                                   \
		    (GDEY0579F5X_DEFINE(node_id, seq_f52_full)))

DT_FOREACH_STATUS_OKAY(gooddisplay_gdey0579f51, GDEY0579F51_DEFINE)
DT_FOREACH_STATUS_OKAY(gooddisplay_gdey0579f52, GDEY0579F52_DEFINE)
