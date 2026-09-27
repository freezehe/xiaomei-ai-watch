/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_NET_IDLE,         // music_net_start() not called yet
    MUSIC_NET_NO_CONFIG,    // XiaoZhi has no saved WiFi (configure WiFi in XiaoZhi first)
    MUSIC_NET_CONNECTING,
    MUSIC_NET_CONNECTED,
    MUSIC_NET_FAILED,       // Tried every saved WiFi once; keeps retrying in the background
} music_net_state_t;

/**
 * @brief Connect to WiFi using the networks XiaoZhi saved in the shared NVS
 *        (namespace "wifi", keys ssid/password, ssid1/password1, ...). Non-blocking, idempotent.
 *        The WiFi driver config is kept in RAM only, so XiaoZhi's NVS data is never modified.
 *        Reads flash (NVS): call it from a task whose stack is in internal RAM, not the LVGL task.
 */
void music_net_start(void);

music_net_state_t music_net_state(void);

/** @brief SSID currently used (or being tried), "" if none. */
const char *music_net_ssid(void);

/**
 * @brief Music base URL derived from XiaoZhi's OTA URL (NVS "wifi"/"ota_url"):
 *        "http://host:port/<secret>/xiaozhi/ota/" -> "http://host:port/<secret>/music/"
 *        Cached by music_net_start(), so it is safe to call from any task.
 * @return false if XiaoZhi has no custom OTA URL (or music_net_start() has not run yet)
 */
bool music_net_base_url(char *out, size_t len);

/** @brief Percent-encode a relative path (UTF-8), keeping '/'. Caller frees the result. */
char *music_net_url_encode(const char *path);

typedef struct {
    char *name;     // UTF-8 file or directory name
    bool is_dir;
} music_entry_t;

/**
 * @brief Blocking: fetch the listing of `rel_dir` ("" = root, "sub/dir/" otherwise).
 *        Directories first, then files (mp3/wav only), each sorted by name.
 *        Free the result with music_net_free_list().
 */
esp_err_t music_net_fetch_list(const char *rel_dir, music_entry_t **entries, int *count);

void music_net_free_list(music_entry_t *entries, int count);

#ifdef __cplusplus
}
#endif
