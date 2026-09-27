/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "music_font.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "bsp/esp-bsp.h"
#include "music_cbin_font.h"

#define TAG "MusicFont"

// XiaoZhi assets layout (main/assets.cc, scripts/build_default_assets.py):
//   u32 file_count, u32 checksum, u32 data_len,
//   file_count x { char name[32]; u32 size; u32 offset; u16 width; u16 height; },
//   data: each file is prefixed by 0x5A 0x5A; `offset` points at the prefix.
#define ASSET_NAME_LEN   32
#define ASSET_ENTRY_SIZE (ASSET_NAME_LEN + 12)
#define ASSET_MAX_FILES  256

typedef struct {
    char name[ASSET_NAME_LEN + 1];
    uint32_t size;
    uint32_t offset;    // Offset of the file data inside the partition (after the 0x5A5A prefix)
} asset_entry_t;

static const lv_font_t *s_font;

static bool find_asset(const esp_partition_t *part, uint32_t count, const char *name, asset_entry_t *out)
{
    const uint32_t data_start = 12 + count * ASSET_ENTRY_SIZE;
    uint8_t raw[ASSET_ENTRY_SIZE];
    for (uint32_t i = 0; i < count; i++) {
        if (esp_partition_read(part, 12 + i * ASSET_ENTRY_SIZE, raw, sizeof(raw)) != ESP_OK) {
            return false;
        }
        if (strncmp((const char *)raw, name, ASSET_NAME_LEN) == 0) {
            memcpy(out->name, raw, ASSET_NAME_LEN);
            out->name[ASSET_NAME_LEN] = '\0';
            memcpy(&out->size, raw + ASSET_NAME_LEN, 4);
            memcpy(&out->offset, raw + ASSET_NAME_LEN + 4, 4);
            out->offset += data_start + 2;
            return true;
        }
    }
    return false;
}

// index.json names the text font, e.g. {"text_font": "font_noto_sans_common_30_4.bin", ...}
static bool read_text_font_name(const esp_partition_t *part, uint32_t count, char *name, size_t len)
{
    asset_entry_t index;
    if (!find_asset(part, count, "index.json", &index) || index.size > 64 * 1024) {
        return false;
    }
    char *json = malloc(index.size + 1);
    if (json == NULL || esp_partition_read(part, index.offset, json, index.size) != ESP_OK) {
        free(json);
        return false;
    }
    json[index.size] = '\0';
    bool found = false;
    cJSON *root = cJSON_Parse(json);
    cJSON *font = cJSON_GetObjectItem(root, "text_font");
    if (cJSON_IsString(font)) {
        strlcpy(name, font->valuestring, len);
        found = true;
    }
    cJSON_Delete(root);
    free(json);
    return found;
}

static const lv_font_t *load(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (part == NULL) {
        ESP_LOGW(TAG, "No 'assets' partition (XiaoZhi not installed?)");
        return NULL;
    }
    uint32_t header[3];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK || header[0] == 0 || header[0] > ASSET_MAX_FILES) {
        ESP_LOGW(TAG, "Assets partition is empty or invalid");
        return NULL;
    }
    const uint32_t count = header[0];

    char font_name[ASSET_NAME_LEN + 1];
    asset_entry_t font;
    if (!read_text_font_name(part, count, font_name, sizeof(font_name)) || !find_asset(part, count, font_name, &font)) {
        ESP_LOGW(TAG, "No text font in XiaoZhi assets");
        return NULL;
    }

    // Keep the mapping for the lifetime of the program: glyphs are read from flash on demand
    const void *ptr = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, font.offset, font.size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmap %s (%lu bytes) failed: %s", font.name, (unsigned long)font.size, esp_err_to_name(err));
        return NULL;
    }
    // cbin_font_create() allocates with lv_malloc: hold the LVGL lock, we are not on the LVGL task
    lv_font_t *f = NULL;
    if (bsp_display_lock(5000) == ESP_OK) {
        f = cbin_font_create((uint8_t *)ptr);
        bsp_display_unlock();
    }
    if (f == NULL) {
        esp_partition_munmap(handle);
        return NULL;
    }
    ESP_LOGI(TAG, "Loaded %s from XiaoZhi assets (%lu KB, line height %d)", font.name,
             (unsigned long)(font.size / 1024), (int)f->line_height);
    return f;
}

void music_font_load(const lv_font_t *fallback)
{
    if (s_font != NULL) {
        return;
    }
    lv_font_t *f = (lv_font_t *)load();
    if (f != NULL) {
        f->fallback = fallback;
        s_font = f;
    }
}

const lv_font_t *music_font_get(const lv_font_t *fallback)
{
    return (s_font != NULL) ? s_font : fallback;
}
