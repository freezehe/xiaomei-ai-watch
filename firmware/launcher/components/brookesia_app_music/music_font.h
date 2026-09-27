/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the Chinese font for song titles from XiaoZhi's "assets" partition.
 *
 *        Dual-boot: XiaoZhi's assets partition (0x800000) holds a Noto Sans CJK font in
 *        "cbin" format (~7000 common characters, 30px). It is memory-mapped, so it costs no
 *        app flash and almost no RAM. Glyphs it lacks (e.g. LV_SYMBOL_* icons) fall back to
 *        `fallback`.
 *
 *        Reads flash: call from a task whose stack is in internal RAM (NOT the LVGL task,
 *        its stack is in PSRAM). Takes the LVGL lock internally. Loads only once.
 */
void music_font_load(const lv_font_t *fallback);

/** @brief The CJK font if music_font_load() succeeded, otherwise `fallback`. Any task. */
const lv_font_t *music_font_get(const lv_font_t *fallback);

#ifdef __cplusplus
}
#endif
