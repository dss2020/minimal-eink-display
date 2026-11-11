/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <hal/gpio_hal.h>
#include <soc/soc_caps.h>

#include <eink/timed_poweroff.h>

#define HOLD_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(timed_poweroff_hold)

#if DT_NODE_EXISTS(HOLD_NODE)

static const struct gpio_dt_spec hold_gpios[] = {
	DT_FOREACH_PROP_ELEM_SEP(HOLD_NODE, gpios, GPIO_DT_SPEC_GET_BY_IDX, (,))
};

static gpio_hal_context_t gpio_hal = {.dev = GPIO_HAL_GET_HW(GPIO_PORT_0)};

/* SoC pin number of a pin given by its GPIO port device and pin. */
static int soc_pin(const struct gpio_dt_spec *gpio)
{
#if DT_NODE_EXISTS(DT_NODELABEL(gpio1))
	if (gpio->port == DEVICE_DT_GET(DT_NODELABEL(gpio1))) {
		return gpio->pin + 32;
	}
#endif
	return gpio->pin;
}

/* Deep sleep releases the pads unless their state is latched. */
static void hold_pins(void)
{
	ARRAY_FOR_EACH_PTR(hold_gpios, gpio) {
		int pin = soc_pin(gpio);

		if (gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE) != 0) {
			continue;
		}

		if (rtc_gpio_is_valid_gpio(pin)) {
			rtc_gpio_hold_en(pin);
		} else {
			gpio_hal_hold_en(&gpio_hal, pin);
		}
	}

#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
	gpio_deep_sleep_hold_en();
#endif
}

/* The latches survive the wake-up reset and would keep the pins frozen. */
static int release_pins(void)
{
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
	gpio_deep_sleep_hold_dis();
#endif

	ARRAY_FOR_EACH_PTR(hold_gpios, gpio) {
		int pin = soc_pin(gpio);

		if (rtc_gpio_is_valid_gpio(pin)) {
			rtc_gpio_hold_dis(pin);
		} else {
			gpio_hal_hold_dis(&gpio_hal, pin);
		}
	}

	return 0;
}

SYS_INIT(release_pins, PRE_KERNEL_1, 0);

#else

static void hold_pins(void)
{
}

#endif /* DT_NODE_EXISTS(HOLD_NODE) */

int timed_poweroff(uint32_t seconds)
{
	if (esp_sleep_enable_timer_wakeup((uint64_t)seconds * USEC_PER_SEC) != ESP_OK) {
		return -EINVAL;
	}

	hold_pins();

	/* Enters deep sleep; the SoC restarts from reset on wake-up. */
	sys_poweroff();
}
