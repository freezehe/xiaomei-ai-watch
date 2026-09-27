/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Turn the AMOLED off after a period without touch input, and back on at the next touch.
 *
 *        Idle time comes from LVGL (`lv_display_get_inactive_time`), so any touch resets it.
 *        While the screen is off a transparent full-screen blocker sits on the system layer:
 *        the touch that wakes the screen is swallowed and never reaches the app underneath.
 *
 *        All functions must be called with the LVGL lock held (or from the LVGL task).
 *
 * @param timeout_s  Idle seconds before the screen turns off
 * @param brightness Brightness (0-100) restored on wake
 */
void screen_sleep_start(uint32_t timeout_s, int brightness);

/**
 * @brief Keep the screen on while held (reference counted), e.g. while a video is shown.
 *        Every `screen_sleep_inhibit(true)` must be paired with `screen_sleep_inhibit(false)`.
 */
void screen_sleep_inhibit(bool inhibit);

/** @brief Wake the screen now and restart the idle countdown (e.g. on a hardware button). */
void screen_sleep_wake(void);

/** @brief Whether the screen is currently off. */
bool screen_sleep_is_asleep(void);

#ifdef __cplusplus
}
#endif
