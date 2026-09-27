/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start polling the AXP2101 fuel gauge (read-only, every 30 s) on the shared BSP I2C bus.
 *        Call after the display (and so the BSP I2C bus) has been started.
 */
void sys_battery_start(void);

/**
 * @brief Latest battery reading.
 * @param[out] percent  0..100
 * @param[out] charging true while the charger is charging the battery
 * @return false if no battery is connected or the gauge has not been read yet
 */
bool sys_battery_get(int *percent, bool *charging);

#ifdef __cplusplus
}
#endif
