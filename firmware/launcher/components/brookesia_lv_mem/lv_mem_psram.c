/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * LVGL heap in PSRAM.
 *
 * With CONFIG_LV_USE_CLIB_MALLOC every LVGL object, style and label text went through malloc(),
 * and CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 sends every allocation below 4 KB to internal RAM.
 * Widgets are a few hundred bytes each, so the UI (network list, song list, launcher) slowly ate
 * the internal RAM that task stacks, WiFi and DMA need: Music could not start its player tasks and
 * AI Chat could not start the switch task. XiaoZhi keeps its LVGL heap in PSRAM too.
 *
 * LVGL only touches this memory from the LVGL task, which is suspended while flash operations
 * disable the cache, so PSRAM is safe here. Internal RAM is only a fallback.
 */
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define LV_CAPS_PSRAM    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_CAPS_FALLBACK (MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS_PSRAM);
    return p ? p : heap_caps_malloc(size, LV_CAPS_FALLBACK);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_CAPS_PSRAM);
    return n ? n : heap_caps_realloc(p, new_size, LV_CAPS_FALLBACK);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(LV_CAPS_PSRAM);
    mon_p->free_size = heap_caps_get_free_size(LV_CAPS_PSRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(LV_CAPS_PSRAM);
    mon_p->used_pct = mon_p->total_size ? 100 - (100 * mon_p->free_size / mon_p->total_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
