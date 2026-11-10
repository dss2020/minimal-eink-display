/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/clock.h>

#include <esp_sleep.h>

#include <eink/timed_poweroff.h>

int timed_poweroff(uint32_t seconds)
{
	if (esp_sleep_enable_timer_wakeup((uint64_t)seconds * USEC_PER_SEC) != ESP_OK) {
		return -EINVAL;
	}

	/* Enters deep sleep; the SoC restarts from reset on wake-up. */
	sys_poweroff();
}
