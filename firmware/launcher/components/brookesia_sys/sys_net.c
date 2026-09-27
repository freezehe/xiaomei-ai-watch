/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sys_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "SysNet"

#define XZ_NVS_NAMESPACE "wifi"     // Shared with XiaoZhi (esp-wifi-connect SsidManager, Ota)

#define TICK_MS          2000       // Worker wake-up period (RSSI refresh, timeouts)
#define SWITCH_WAIT_MS   3000       // Wait for our own disconnect before connecting elsewhere
#define JOIN_TIMEOUT_MS  25000
#define SCAN_HOLD_MS     8000       // Max time the reconnect loop is paused for a scan

/*
 * Design: one worker task owns all WiFi state. WiFi events and API requests (from the LVGL task)
 * are both posted to its queue, so there are no data races on the state, and every flash access
 * (NVS) happens on the worker's stack, which xTaskCreate() allocates from internal RAM. Flash
 * access with the cache disabled asserts when the calling task's stack is in PSRAM - the LVGL
 * task's stack is (main.cpp: stack_in_psram).
 */

typedef enum {
    MSG_STA_START,
    MSG_DISCONNECTED,
    MSG_GOT_IP,
    MSG_SCAN_DONE,
    MSG_CMD_SCAN,
    MSG_CMD_JOIN,
    MSG_CMD_FORGET,
} msg_type_t;

typedef struct {
    msg_type_t type;
    int reason;                 // MSG_DISCONNECTED
    bool use_saved_password;    // MSG_CMD_JOIN
    char ssid[33];
    char password[65];
} msg_t;

typedef struct {
    char ssid[33];
    char password[65];
    uint8_t channel;
} wifi_cred_t;

// ---- Worker-owned state (only touched by the worker task)
static wifi_cred_t s_creds[SYS_NET_MAX_SAVED];
static int s_cred_count;
static int s_cred_index;
static int s_attempts;          // Attempts on the current credential
static int s_full_cycles;
static bool s_connected;

static struct {
    bool active;
    bool switching;             // Waiting for the disconnect we caused
    TickType_t switch_tick;
    TickType_t start_tick;
    int attempts;
    wifi_cred_t cred;
} s_join;

static volatile bool s_shutting_down; // esp_restart() in progress: stop touching WiFi
static bool s_scan_hold;        // Reconnect loop paused so the scan can run
static TickType_t s_scan_hold_tick;

// ---- Shared with API readers
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;    // Guards the copies below
static volatile sys_net_state_t s_state = SYS_NET_IDLE;
static volatile sys_net_join_t s_join_state = SYS_NET_JOIN_IDLE;
static volatile bool s_scanning;
static volatile uint32_t s_scan_gen;
static volatile int s_rssi;
static char s_cur_ssid[33];
static char s_ip[16];
static char s_server_host[64];
static sys_net_ap_t s_scan[SYS_NET_MAX_SCAN];
static int s_scan_count;
static char s_saved[SYS_NET_MAX_SAVED][33];
static int s_saved_count;

static bool elapsed(TickType_t since, int ms)
{
    return (xTaskGetTickCount() - since) >= pdMS_TO_TICKS(ms);
}

static void post(const msg_t *m)
{
    if (s_queue == NULL || xQueueSend(s_queue, m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Request %d dropped", m->type);
    }
}

static void set_cur_ssid(const char *ssid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- NVS (worker task only)
static void make_key(char *out, size_t len, const char *prefix, int index)
{
    // SsidManager::MakeWifiKey: index 0 -> "ssid", i -> "ssid<i>"
    if (index == 0) {
        strlcpy(out, prefix, len);
    } else {
        snprintf(out, len, "%s%d", prefix, index);
    }
}

static void publish_saved(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_saved_count = s_cred_count;
    for (int i = 0; i < s_cred_count; i++) {
        strlcpy(s_saved[i], s_creds[i].ssid, sizeof(s_saved[i]));
    }
    // Refresh the "saved" marks of the last scan
    for (int i = 0; i < s_scan_count; i++) {
        s_scan[i].saved = false;
        for (int j = 0; j < s_cred_count; j++) {
            if (strcmp(s_scan[i].ssid, s_creds[j].ssid) == 0) {
                s_scan[i].saved = true;
            }
        }
    }
    xSemaphoreGive(s_lock);
}

static void log_saved(const char *when)
{
    char line[SYS_NET_MAX_SAVED * 36] = "";
    for (int i = 0; i < s_cred_count; i++) {
        strlcat(line, i ? ", " : "", sizeof(line));
        strlcat(line, s_creds[i].ssid, sizeof(line));
    }
    ESP_LOGI(TAG, "Saved WiFi %s: [%s]", when, line);
}

// Same rules as SsidManager::LoadFromNvs(): an entry needs both ssid and password (may be "")
static void load_credentials(void)
{
    nvs_handle_t h;
    s_cred_count = 0;
    if (nvs_open(XZ_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < SYS_NET_MAX_SAVED; i++) {
            char key[16];
            wifi_cred_t *c = &s_creds[s_cred_count];
            size_t len = sizeof(c->ssid);
            make_key(key, sizeof(key), "ssid", i);
            if (nvs_get_str(h, key, c->ssid, &len) != ESP_OK || c->ssid[0] == '\0') {
                continue;
            }
            len = sizeof(c->password);
            make_key(key, sizeof(key), "password", i);
            if (nvs_get_str(h, key, c->password, &len) != ESP_OK) {
                continue;
            }
            c->channel = 0;
            make_key(key, sizeof(key), "channel", i);
            nvs_get_u8(h, key, &c->channel);
            s_cred_count++;
        }
        nvs_close(h);
    }
    publish_saved();
}

// Same layout as SsidManager::SaveToNvs(). Only ssid*/password*/channel* keys are touched:
// the namespace also holds XiaoZhi's "ota_url", which must survive.
static bool save_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(XZ_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    for (int i = 0; i < SYS_NET_MAX_SAVED; i++) {
        char ssid_key[16], pass_key[16], chan_key[16];
        make_key(ssid_key, sizeof(ssid_key), "ssid", i);
        make_key(pass_key, sizeof(pass_key), "password", i);
        make_key(chan_key, sizeof(chan_key), "channel", i);
        if (i < s_cred_count) {
            nvs_set_str(h, ssid_key, s_creds[i].ssid);
            nvs_set_str(h, pass_key, s_creds[i].password);
            nvs_set_u8(h, chan_key, s_creds[i].channel);
        } else {
            nvs_erase_key(h, ssid_key);
            nvs_erase_key(h, pass_key);
            nvs_erase_key(h, chan_key);
        }
    }
    err = nvs_commit(h);
    nvs_close(h);
    publish_saved();
    return err == ESP_OK;
}

static int find_cred(const char *ssid)
{
    for (int i = 0; i < s_cred_count; i++) {
        if (strcmp(s_creds[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

// The user picked this network: store it first (preferred), dropping the oldest if full
static void save_as_preferred(const wifi_cred_t *cred)
{
    log_saved("before");
    int i = find_cred(cred->ssid);
    if (i < 0) {
        i = (s_cred_count < SYS_NET_MAX_SAVED) ? s_cred_count++ : SYS_NET_MAX_SAVED - 1;
    }
    memmove(&s_creds[1], &s_creds[0], i * sizeof(wifi_cred_t));
    s_creds[0] = *cred;
    save_credentials();
    log_saved("after");
}

static void forget(const char *ssid)
{
    int i = find_cred(ssid);
    if (i < 0) {
        return;
    }
    log_saved("before");
    memmove(&s_creds[i], &s_creds[i + 1], (s_cred_count - i - 1) * sizeof(wifi_cred_t));
    s_cred_count--;
    save_credentials();
    log_saved("after");
}

// "http://host:port/secret/xiaozhi/ota/" -> "host:port". The path is a secret: never shown.
static void load_server_host(void)
{
    nvs_handle_t h;
    if (nvs_open(XZ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char url[256];
    size_t n = sizeof(url);
    if (nvs_get_str(h, "ota_url", url, &n) == ESP_OK) {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        size_t len = strcspn(p, "/");
        if (len >= sizeof(s_server_host)) {
            len = sizeof(s_server_host) - 1;
        }
        memcpy(s_server_host, p, len);
        s_server_host[len] = '\0';
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- connecting (worker task)
static void connect_to(const wifi_cred_t *c)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, c->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, c->password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = c->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    set_cur_ssid(c->ssid);
    ESP_LOGI(TAG, "Connecting to '%s'", c->ssid);
    s_state = SYS_NET_CONNECTING;
    esp_wifi_connect();
}

// Connect to whatever should be tried now: the user's pick first, else the saved list
static void connect_next(void)
{
    if (s_join.active) {
        connect_to(&s_join.cred);
    } else if (s_cred_count > 0) {
        if (s_cred_index >= s_cred_count) {
            s_cred_index = 0;
        }
        connect_to(&s_creds[s_cred_index]);
    } else {
        set_cur_ssid("");
        s_state = SYS_NET_NO_CONFIG;
    }
}

static void join_finish(sys_net_join_t result)
{
    s_join.active = false;
    s_join.switching = false;
    s_join_state = result;
    ESP_LOGI(TAG, "Join '%s' -> %d", s_join.cred.ssid, result);
}

static void start_scan(void)
{
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_OK) {
        return;
    }
    if (!s_connected && !s_scan_hold) {
        // A connection attempt is in progress (the driver refuses to scan): pause the reconnect
        // loop, abort the attempt and scan once the disconnect event arrives
        s_scan_hold = true;
        s_scan_hold_tick = xTaskGetTickCount();
        esp_wifi_disconnect();
        return;
    }
    ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
    s_scanning = false;
    if (s_scan_hold) {
        s_scan_hold = false;
        connect_next();
    }
}

static void on_scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = n ? calloc(n, sizeof(*recs)) : NULL;
    if (recs == NULL) {
        n = 0;
        esp_wifi_clear_ap_list();
    } else {
        esp_wifi_scan_get_ap_records(&n, recs);
    }

    sys_net_ap_t list[SYS_NET_MAX_SCAN];
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;               // Hidden network
        }
        int k = 0;
        while (k < count && strcmp(list[k].ssid, ssid) != 0) {
            k++;
        }
        if (k < count) {            // Same SSID on several APs: keep the strongest
            if (recs[i].rssi > list[k].rssi) {
                list[k].rssi = recs[i].rssi;
                list[k].channel = recs[i].primary;
            }
            continue;
        }
        if (count == SYS_NET_MAX_SCAN) {
            continue;
        }
        sys_net_ap_t *ap = &list[count++];
        strlcpy(ap->ssid, ssid, sizeof(ap->ssid));
        ap->rssi = recs[i].rssi;
        ap->channel = recs[i].primary;
        ap->secure = recs[i].authmode != WIFI_AUTH_OPEN && recs[i].authmode != WIFI_AUTH_OWE;
        ap->saved = find_cred(ssid) >= 0;
    }
    free(recs);

    // Strongest first (insertion sort, at most 20 entries)
    for (int i = 1; i < count; i++) {
        sys_net_ap_t t = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < t.rssi) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = t;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_scan, list, count * sizeof(list[0]));
    s_scan_count = count;
    xSemaphoreGive(s_lock);
    s_scan_gen++;
    s_scanning = false;
    ESP_LOGI(TAG, "Scan done: %d network(s)", count);

    if (s_scan_hold) {
        s_scan_hold = false;
        if (!s_connected) {
            connect_next();
        }
    }
}

static bool is_auth_failure(int reason)
{
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_MIC_FAILURE;
}

static bool is_not_found(int reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void on_disconnected(int reason)
{
    bool was_connected = s_connected;
    s_connected = false;
    s_rssi = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ip[0] = '\0';
    xSemaphoreGive(s_lock);

    if (s_scan_hold) {
        start_scan();               // Reconnect resumes in on_scan_done()
        return;
    }
    if (s_join.active) {
        if (s_join.switching) {     // Our own disconnect: now try the user's pick
            s_join.switching = false;
            connect_next();
            return;
        }
        ESP_LOGW(TAG, "Join '%s' attempt failed, reason %d", s_join.cred.ssid, reason);
        if (++s_join.attempts < 2) {
            connect_next();
            return;
        }
        join_finish(is_auth_failure(reason) ? SYS_NET_JOIN_FAIL_AUTH :
                    is_not_found(reason) ? SYS_NET_JOIN_FAIL_NOT_FOUND : SYS_NET_JOIN_FAIL);
        s_attempts = 0;
        connect_next();             // Back to the saved networks
        return;
    }

    if (was_connected) {
        ESP_LOGW(TAG, "Disconnected from '%s', reason %d", s_cur_ssid, reason);
    }
    // Two attempts per network, then the next one; loop forever (phone hotspot may appear later)
    if (++s_attempts >= 2) {
        s_attempts = 0;
        if (++s_cred_index >= s_cred_count) {
            s_cred_index = 0;
            s_full_cycles++;
        }
    }
    connect_next();
    if (s_full_cycles > 0 && s_state == SYS_NET_CONNECTING) {
        s_state = SYS_NET_FAILED;
    }
}

static void on_got_ip(void)
{
    s_connected = true;
    s_attempts = 0;
    s_full_cycles = 0;
    wifi_ap_record_t ap;
    uint8_t channel = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        channel = ap.primary;
        s_rssi = ap.rssi;
    }
    if (s_join.active) {
        s_join.cred.channel = channel;
        save_as_preferred(&s_join.cred);
        s_cred_index = 0;
        join_finish(SYS_NET_JOIN_OK);
    }
    s_state = SYS_NET_CONNECTED;
}

static void on_join_request(const msg_t *m)
{
    wifi_cred_t cred = {0};
    strlcpy(cred.ssid, m->ssid, sizeof(cred.ssid));
    if (m->use_saved_password) {
        int i = find_cred(m->ssid);
        if (i >= 0) {
            strlcpy(cred.password, s_creds[i].password, sizeof(cred.password));
            cred.channel = s_creds[i].channel;
        }
    } else {
        strlcpy(cred.password, m->password, sizeof(cred.password));
    }

    s_join.active = true;
    s_join.cred = cred;
    s_join.attempts = 0;
    s_join.start_tick = xTaskGetTickCount();
    s_join_state = SYS_NET_JOIN_BUSY;
    if (s_scan_hold) {              // The pending scan is dropped: the user picked a network
        s_scan_hold = false;
        s_scanning = false;
    }
    ESP_LOGI(TAG, "Join '%s' requested", cred.ssid);

    wifi_ap_record_t ap;
    bool busy = s_connected || s_state == SYS_NET_CONNECTING || s_state == SYS_NET_FAILED ||
                esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (busy) {
        // Leave the current AP / abort the current attempt; connect in on_disconnected()
        s_join.switching = true;
        s_join.switch_tick = xTaskGetTickCount();
        esp_wifi_disconnect();
    } else {
        connect_next();
    }
}

static void on_tick(void)
{
    // Diagnostics: internal RAM is what task stacks and flash-safe buffers need
    static TickType_t s_heap_log_tick;
    if (elapsed(s_heap_log_tick, 10000)) {
        s_heap_log_tick = xTaskGetTickCount();
        ESP_LOGI(TAG, "Internal RAM free %u, largest %u, min ever %u | PSRAM free %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    // Safety nets for events that never arrive
    if (s_join.active && s_join.switching && elapsed(s_join.switch_tick, SWITCH_WAIT_MS)) {
        s_join.switching = false;
        connect_next();
    }
    if (s_join.active && elapsed(s_join.start_tick, JOIN_TIMEOUT_MS)) {
        join_finish(SYS_NET_JOIN_FAIL);
        esp_wifi_disconnect();      // on_disconnected() resumes the saved networks
    }
    if (s_scan_hold && elapsed(s_scan_hold_tick, SCAN_HOLD_MS)) {
        s_scan_hold = false;
        s_scanning = false;
        if (!s_connected) {
            connect_next();
        }
    }
}

static void worker_task(void *arg)
{
    msg_t m;
    while (true) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) {
            if (s_shutting_down) {
                continue;
            }
            on_tick();
            continue;
        }
        if (s_shutting_down) {
            continue;               // Racing esp_wifi_stop() during restart can hang the reboot
        }
        switch (m.type) {
        case MSG_STA_START:
            connect_next();
            break;
        case MSG_DISCONNECTED:
            on_disconnected(m.reason);
            break;
        case MSG_GOT_IP:
            on_got_ip();
            break;
        case MSG_SCAN_DONE:
            on_scan_done();
            break;
        case MSG_CMD_SCAN:
            if (!s_scan_hold) {
                start_scan();
            }
            break;
        case MSG_CMD_JOIN:
            on_join_request(&m);
            break;
        case MSG_CMD_FORGET:
            forget(m.ssid);
            if (strcmp(m.ssid, s_cur_ssid) == 0 && !s_join.active) {
                s_cred_index = 0;
                s_attempts = 0;
                esp_wifi_disconnect();
                if (s_cred_count == 0) {
                    set_cur_ssid("");
                    s_state = SYS_NET_NO_CONFIG;
                }
            }
            break;
        }
        memset(m.password, 0, sizeof(m.password));
        on_tick();
    }
}

// esp_restart() runs shutdown handlers in reverse registration order, so this one (registered
// after esp_wifi_init) runs before WiFi's own esp_wifi_stop(): from now on the worker ignores the
// resulting disconnect event instead of reconnecting while the driver is being stopped.
static void on_shutdown(void)
{
    s_shutting_down = true;
}

// Runs in the default event loop task: only forward to the worker
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    msg_t m = {0};
    if (s_shutting_down) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        m.type = MSG_STA_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        m.type = MSG_DISCONNECTED;
        m.reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        m.type = MSG_SCAN_DONE;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Connected to '%s', IP %s", s_cur_ssid, s_ip);
        m.type = MSG_GOT_IP;
    } else {
        return;
    }
    post(&m);
}

// ---------------------------------------------------------------- public API
void sys_net_start(void)
{
    if (s_state != SYS_NET_IDLE) {
        return;
    }
    // Only init, never erase: the NVS partition holds XiaoZhi's WiFi and activation data
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(8, sizeof(msg_t));
    load_credentials();
    load_server_host();
    ESP_LOGI(TAG, "Found %d WiFi network(s) saved by XiaoZhi", s_cred_count);
    s_state = s_cred_count ? SYS_NET_CONNECTING : SYS_NET_NO_CONFIG;

    // WiFi is started even with no saved network, so the Settings app can scan
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;  // Keep the WiFi driver config in RAM: credentials are saved by us only
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_register_shutdown_handler(on_shutdown);

    // xTaskCreate() puts the stack in internal RAM: NVS writes are safe there
    xTaskCreate(worker_task, "sys_net", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(esp_wifi_start());
}

sys_net_state_t sys_net_state(void)
{
    return s_state;
}

const char *sys_net_ssid(void)
{
    return s_cur_ssid;
}

bool sys_net_ip(char *out, size_t len)
{
    if (s_lock == NULL || len == 0) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_ip[0] != '\0';
    strlcpy(out, s_ip, len);
    xSemaphoreGive(s_lock);
    return ok;
}

int sys_net_rssi(void)
{
    return s_rssi;
}

const char *sys_net_server_host(void)
{
    return s_server_host;
}

void sys_net_scan(void)
{
    if (s_scanning) {
        return;
    }
    s_scanning = true;
    msg_t m = {.type = MSG_CMD_SCAN};
    post(&m);
}

bool sys_net_scanning(void)
{
    return s_scanning;
}

uint32_t sys_net_scan_gen(void)
{
    return s_scan_gen;
}

int sys_net_scan_results(sys_net_ap_t *out, int max)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

int sys_net_saved(char (*ssids)[33], int max)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = (s_saved_count < max) ? s_saved_count : max;
    memcpy(ssids, s_saved, n * sizeof(s_saved[0]));
    xSemaphoreGive(s_lock);
    return n;
}

void sys_net_join(const char *ssid, const char *password)
{
    msg_t m = {.type = MSG_CMD_JOIN, .use_saved_password = (password == NULL)};
    strlcpy(m.ssid, ssid, sizeof(m.ssid));
    if (password != NULL) {
        strlcpy(m.password, password, sizeof(m.password));
    }
    s_join_state = SYS_NET_JOIN_BUSY;
    post(&m);
    memset(m.password, 0, sizeof(m.password));
}

sys_net_join_t sys_net_join_state(void)
{
    return s_join_state;
}

void sys_net_forget(const char *ssid)
{
    msg_t m = {.type = MSG_CMD_FORGET};
    strlcpy(m.ssid, ssid, sizeof(m.ssid));
    post(&m);
}
