/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sys_time.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sys_net.h"

#define TAG "SysTime"

#define TIME_ZONE          "CST-8"          // Beijing time (POSIX sign is inverted: UTC+8)
#define NVS_NAMESPACE      "bs_time"        // Own namespace: XiaoZhi's "wifi" data is never touched
#define NVS_KEY_EPOCH      "epoch"
#define VALID_EPOCH        1735689600LL     // 2025-01-01: anything earlier means "never set"
#define SAVE_PERIOD_MS     (10 * 60 * 1000) // Few writes: NVS wear is negligible

static volatile sys_time_state_t s_state = SYS_TIME_UNSET;
static TaskHandle_t s_saver;

static bool load_saved_time(void)
{
    nvs_handle_t h;
    int64_t epoch = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_i64(h, NVS_KEY_EPOCH, &epoch);
    nvs_close(h);
    if (err != ESP_OK || epoch < VALID_EPOCH) {
        return false;
    }
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return true;
}

static void save_time(void)
{
    time_t now = time(NULL);
    if (now < VALID_EPOCH) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(h, NVS_KEY_EPOCH, (int64_t)now) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

// Writes NVS from a task with an internal-RAM stack (flash writes disable the cache)
static void saver_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_PERIOD_MS));
        if (s_state != SYS_TIME_UNSET) {
            save_time();
        }
    }
}

static void on_sntp_sync(struct timeval *tv)
{
    s_state = SYS_TIME_SYNCED;
    ESP_LOGI(TAG, "Time synced by SNTP");
    if (s_saver != NULL) {
        xTaskNotifyGive(s_saver);
    }
}

void sys_time_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;

    setenv("TZ", TIME_ZONE, 1);
    tzset();

    if (time(NULL) >= VALID_EPOCH) {
        // System time survives a soft reset (e.g. switching back from XiaoZhi)
        s_state = SYS_TIME_RESTORED;
        ESP_LOGI(TAG, "Kept system time across reset");
    } else if (load_saved_time()) {
        s_state = SYS_TIME_RESTORED;
        ESP_LOGI(TAG, "Restored last saved time from NVS (waiting for SNTP)");
    } else {
        ESP_LOGW(TAG, "No known time yet (waiting for SNTP)");
    }

    xTaskCreate(saver_task, "time_save", 3072, NULL, 2, &s_saver);

    if (sys_net_state() == SYS_NET_IDLE || sys_net_state() == SYS_NET_NO_CONFIG) {
        ESP_LOGW(TAG, "No WiFi: SNTP not started");
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
                                2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org"));
    cfg.sync_cb = on_sntp_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

sys_time_state_t sys_time_state(void)
{
    return s_state;
}
