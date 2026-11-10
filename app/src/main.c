/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slideshow: show the next stored picture, then wait for the configured
 * interval, either powered off or asleep in this thread.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/retention.h>
#include <zephyr/sys/util.h>

#include <eink/image_pack.h>
#include <eink/timed_poweroff.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define FRAME_STRIDE DIV_ROUND_UP(CONFIG_IMAGE_PACK_WIDTH * CONFIG_IMAGE_PACK_BPP, 8)
#define FRAME_SIZE   (FRAME_STRIDE * CONFIG_IMAGE_PACK_HEIGHT)

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint8_t frame[FRAME_SIZE];

/*
 * The ssd16xx driver starts a refresh and returns while the panel is still
 * busy. The panel must not lose its controller mid-refresh, so wait on its
 * busy line before powering off. Other drivers return when they are done.
 */
#define REFRESH_START_MS   100
#define REFRESH_POLL_MS    20
#define REFRESH_TIMEOUT_MS 30000

static const struct gpio_dt_spec busy =
	GPIO_DT_SPEC_GET_OR(DT_CHOSEN(zephyr_display), busy_gpios, {0});

static void wait_refresh_done(void)
{
	int64_t end = k_uptime_get() + REFRESH_TIMEOUT_MS;

	if (!IS_ENABLED(CONFIG_SSD16XX) || busy.port == NULL) {
		return;
	}

	k_msleep(REFRESH_START_MS);

	while (gpio_pin_get_dt(&busy) > 0) {
		if (k_uptime_get() > end) {
			LOG_WRN("Display still busy after %d ms", REFRESH_TIMEOUT_MS);
			return;
		}
		k_msleep(REFRESH_POLL_MS);
	}
}

/*
 * Position in the slideshow. Boards that power off between pictures provide
 * a "slideshow-state" retention area; otherwise it only has to live in RAM.
 */
#if DT_HAS_ALIAS(slideshow_state)
static const struct device *const state_dev = DEVICE_DT_GET(DT_ALIAS(slideshow_state));

static uint32_t state_load(void)
{
	uint32_t index;

	if (!device_is_ready(state_dev) || retention_is_valid(state_dev) != 1 ||
	    retention_read(state_dev, 0, (uint8_t *)&index, sizeof(index)) != 0) {
		return 0;
	}

	return index;
}

static void state_save(uint32_t index)
{
	int err = retention_write(state_dev, 0, (const uint8_t *)&index, sizeof(index));

	if (err != 0) {
		LOG_WRN("Could not save slideshow position (%d)", err);
	}
}
#else
static uint32_t ram_index;

static uint32_t state_load(void)
{
	return ram_index;
}

static void state_save(uint32_t index)
{
	ram_index = index;
}
#endif

/* Monochrome picture: one write of the whole frame. */
static int write_mono(const struct display_capabilities *caps)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = sizeof(frame),
		.width = CONFIG_IMAGE_PACK_WIDTH,
		.height = CONFIG_IMAGE_PACK_HEIGHT,
		.pitch = CONFIG_IMAGE_PACK_WIDTH,
	};

	/* Pictures are stored row by row, MSB first, 1 = white. */
	if ((caps->screen_info & SCREEN_INFO_MONO_VTILED) != 0 ||
	    (caps->screen_info & SCREEN_INFO_MONO_MSB_FIRST) == 0) {
		LOG_ERR("Display bit layout is not supported");
		return -ENOTSUP;
	}

	if (caps->current_pixel_format == PIXEL_FORMAT_MONO10) {
		for (size_t i = 0; i < sizeof(frame); i++) {
			frame[i] = ~frame[i];
		}
	} else if (caps->current_pixel_format != PIXEL_FORMAT_MONO01) {
		LOG_ERR("Display is not monochrome");
		return -ENOTSUP;
	}

	return display_write(display, 0, 0, &desc, frame);
}

/*
 * Colour picture: stored at 2 bits per pixel, written a row at a time in the
 * display's 4-bit indexed format. The stored values are the palette indices
 * of the display.
 */
static int write_indexed(const struct display_capabilities *caps)
{
	static uint8_t row[DIV_ROUND_UP(CONFIG_IMAGE_PACK_WIDTH, 2)];
	const struct display_buffer_descriptor desc = {
		.buf_size = sizeof(row),
		.width = CONFIG_IMAGE_PACK_WIDTH,
		.height = 1,
		.pitch = CONFIG_IMAGE_PACK_WIDTH,
	};

	if (caps->current_pixel_format != PIXEL_FORMAT_I_4) {
		LOG_ERR("Display does not take indexed colour");
		return -ENOTSUP;
	}

	for (uint16_t y = 0; y < CONFIG_IMAGE_PACK_HEIGHT; y++) {
		const uint8_t *src = &frame[y * FRAME_STRIDE];
		int err;

		memset(row, 0, sizeof(row));
		for (uint16_t x = 0; x < CONFIG_IMAGE_PACK_WIDTH; x++) {
			uint8_t index = (src[x / 4] >> (6 - 2 * (x % 4))) & 0x3;

			row[x / 2] |= index << ((x & 1) ? 0 : 4);
		}

		err = display_write(display, 0, y, &desc, row);
		if (err != 0) {
			return err;
		}
	}

	return 0;
}

static int show_picture(uint16_t index)
{
	struct display_capabilities caps;
	int err;

	err = image_pack_decode(index, frame, sizeof(frame));
	if (err != 0) {
		LOG_ERR("Could not decode picture %u (%d)", index, err);
		return err;
	}

	display_get_capabilities(display, &caps);

	/* Write with blanking on so that turning it off does one full refresh. */
	err = display_blanking_on(display);
	if (err != 0 && err != -ENOSYS) {
		return err;
	}

	err = (CONFIG_IMAGE_PACK_BPP == 1) ? write_mono(&caps) : write_indexed(&caps);
	if (err != 0) {
		LOG_ERR("Could not write to the display (%d)", err);
		return err;
	}

	err = display_blanking_off(display);
	if (err != 0 && err != -ENOSYS) {
		LOG_ERR("Display refresh failed (%d)", err);
		return err;
	}

	wait_refresh_done();

	return 0;
}

int main(void)
{
	struct image_pack_info info;
	int err;

	if (!device_is_ready(display)) {
		LOG_ERR("Display is not ready");
		return 0;
	}

	err = image_pack_get_info(&info);
	if (err != 0 || info.count == 0) {
		LOG_ERR("No usable pictures in the firmware (%d)", err);
		return 0;
	}

	if (info.width != CONFIG_IMAGE_PACK_WIDTH || info.height != CONFIG_IMAGE_PACK_HEIGHT ||
	    info.bpp != CONFIG_IMAGE_PACK_BPP) {
		LOG_ERR("Pictures are %ux%u at %u bpp, expected %dx%d at %d bpp", info.width,
			info.height, info.bpp, CONFIG_IMAGE_PACK_WIDTH, CONFIG_IMAGE_PACK_HEIGHT,
			CONFIG_IMAGE_PACK_BPP);
		return 0;
	}

	while (true) {
		uint32_t index = state_load() % info.count;

		LOG_INF("Showing picture %u of %u", index + 1, info.count);
		(void)show_picture(index);

		state_save((index + 1) % info.count);

		if (IS_ENABLED(CONFIG_APP_DEEP_SLEEP)) {
			LOG_INF("Powering off for %d s", CONFIG_APP_SLIDE_INTERVAL_SEC);
			err = timed_poweroff(CONFIG_APP_SLIDE_INTERVAL_SEC);
			LOG_ERR("Power-off failed (%d), staying awake", err);
		}

		k_sleep(K_SECONDS(CONFIG_APP_SLIDE_INTERVAL_SEC));
	}

	return 0;
}
