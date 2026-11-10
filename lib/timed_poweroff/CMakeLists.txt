# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_SOC_FAMILY_ESPRESSIF_ESP32 timed_poweroff_esp32.c)
