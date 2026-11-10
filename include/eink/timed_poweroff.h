/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef EINK_TIMED_POWEROFF_H_
#define EINK_TIMED_POWEROFF_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Power the system off and restart it after a delay.
 *
 * The SoC enters its lowest power state and goes through a normal boot when
 * the delay expires. Only retained memory survives.
 *
 * @param seconds Time to stay powered off.
 *
 * @return Does not return on success, a negative errno value if the wake-up
 *         timer could not be armed.
 */
int timed_poweroff(uint32_t seconds);

#ifdef __cplusplus
}
#endif

#endif /* EINK_TIMED_POWEROFF_H_ */
