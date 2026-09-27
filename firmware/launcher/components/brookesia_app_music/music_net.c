/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "music_net.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "sys_net.h"

#define TAG "MusicNet"

#define XZ_NVS_NAMESPACE "wifi"     // Shared with XiaoZhi (Ota)
#define XZ_OTA_SUFFIX    "xiaozhi/ota/"
#define MAX_LIST_BYTES   (256 * 1024)
#define HTTP_TIMEOUT_MS  8000

static char s_base_url[256];           // Cached at start: NVS must not be read from PSRAM-stack tasks

// Read once in music_net_start(): reading NVS touches flash, which asserts when called from a
// task whose stack is in PSRAM (the LVGL task here)
static void load_base_url(void)
{
    nvs_handle_t h;
    s_base_url[0] = '\0';
    if (nvs_open(XZ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char url[256];
    size_t n = sizeof(url);
    if (nvs_get_str(h, "ota_url", url, &n) == ESP_OK) {
        size_t ul = strlen(url), sl = strlen(XZ_OTA_SUFFIX);
        if (ul > sl && strcmp(url + ul - sl, XZ_OTA_SUFFIX) == 0 && ul - sl + strlen("music/") < sizeof(s_base_url)) {
            memcpy(s_base_url, url, ul - sl);
            strcpy(s_base_url + ul - sl, "music/");
        }
    }
    nvs_close(h);
}

void music_net_start(void)
{
    // WiFi itself is shared with the rest of the launcher (brookesia_sys, started in app_main)
    sys_net_start();
    load_base_url();
}

music_net_state_t music_net_state(void)
{
    switch (sys_net_state()) {
    case SYS_NET_NO_CONFIG:
        return MUSIC_NET_NO_CONFIG;
    case SYS_NET_CONNECTING:
        return MUSIC_NET_CONNECTING;
    case SYS_NET_CONNECTED:
        return MUSIC_NET_CONNECTED;
    case SYS_NET_FAILED:
        return MUSIC_NET_FAILED;
    default:
        return MUSIC_NET_IDLE;
    }
}

const char *music_net_ssid(void)
{
    return sys_net_ssid();
}

bool music_net_base_url(char *out, size_t len)
{
    if (s_base_url[0] == '\0' || strlen(s_base_url) >= len) {
        return false;
    }
    strcpy(out, s_base_url);
    return true;
}

char *music_net_url_encode(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(path);
    char *out = malloc(n * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    char *p = out;
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
                *s == '-' || *s == '_' || *s == '.' || *s == '~' || *s == '/') {
            *p++ = (char)*s;
        } else {
            *p++ = '%';
            *p++ = hex[*s >> 4];
            *p++ = hex[*s & 0x0F];
        }
    }
    *p = '\0';
    return out;
}

static bool is_audio_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0);
}

static int cmp_entry(const void *a, const void *b)
{
    const music_entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

esp_err_t music_net_fetch_list(const char *rel_dir, music_entry_t **entries, int *count)
{
    *entries = NULL;
    *count = 0;
    if (music_net_state() != MUSIC_NET_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    char base[256];
    if (!music_net_base_url(base, sizeof(base))) {
        return ESP_ERR_NOT_FOUND;
    }
    char *enc = music_net_url_encode(rel_dir);
    size_t url_len = strlen(base) + (enc ? strlen(enc) : 0) + 1;
    char *url = malloc(url_len);
    if (enc == NULL || url == NULL) {
        free(enc);
        free(url);
        return ESP_ERR_NO_MEM;
    }
    snprintf(url, url_len, "%s%s", base, enc);
    free(enc);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_open(client, 0);
    char *body = NULL;
    int total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "List %s -> HTTP %d", url, status);
            err = ESP_FAIL;
        } else {
            int cap = 8192;
            body = malloc(cap);
            while (body != NULL) {
                if (total + 2048 + 1 > cap) {
                    if (cap >= MAX_LIST_BYTES) {
                        break;
                    }
                    char *nb = realloc(body, cap * 2);
                    if (nb == NULL) {
                        break;
                    }
                    body = nb;
                    cap *= 2;
                }
                int r = esp_http_client_read(client, body + total, 2048);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            if (body == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                body[total] = '\0';
            }
        }
    } else {
        ESP_LOGE(TAG, "Open %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    free(url);
    if (err != ESP_OK) {
        free(body);
        return err;
    }

    // nginx autoindex_format json: [{"name":"a.mp3","type":"file",...},{"name":"sub","type":"directory",...}]
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int n = cJSON_GetArraySize(root);
    music_entry_t *list = calloc(n > 0 ? n : 1, sizeof(music_entry_t));
    int k = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (list == NULL || !cJSON_IsString(name) || !cJSON_IsString(type)) {
            continue;
        }
        bool is_dir = strcmp(type->valuestring, "directory") == 0;
        if (!is_dir && !is_audio_file(name->valuestring)) {
            continue;
        }
        list[k].name = strdup(name->valuestring);
        list[k].is_dir = is_dir;
        if (list[k].name != NULL) {
            k++;
        }
    }
    cJSON_Delete(root);
    if (list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    qsort(list, k, sizeof(music_entry_t), cmp_entry);
    *entries = list;
    *count = k;
    ESP_LOGI(TAG, "Listed '%s': %d entries", rel_dir, k);
    return ESP_OK;
}

void music_net_free_list(music_entry_t *entries, int count)
{
    for (int i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}
