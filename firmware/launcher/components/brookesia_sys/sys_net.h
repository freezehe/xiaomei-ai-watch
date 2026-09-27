/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYS_NET_IDLE,         // sys_net_start() not called yet
    SYS_NET_NO_CONFIG,    // No saved WiFi (configure it in the Settings app or in XiaoZhi)
    SYS_NET_CONNECTING,
    SYS_NET_CONNECTED,
    SYS_NET_FAILED,       // Tried every saved WiFi once; keeps retrying in the background
} sys_net_state_t;

/** Result of the last sys_net_join() */
typedef enum {
    SYS_NET_JOIN_IDLE,
    SYS_NET_JOIN_BUSY,
    SYS_NET_JOIN_OK,              // Connected and saved
    SYS_NET_JOIN_FAIL_AUTH,       // Wrong password
    SYS_NET_JOIN_FAIL_NOT_FOUND,  // Network not in range
    SYS_NET_JOIN_FAIL,            // Other error / timeout
} sys_net_join_t;

#define SYS_NET_MAX_SAVED  10     // Same limit as XiaoZhi's SsidManager
#define SYS_NET_MAX_SCAN   20

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;                  // Needs a password
    bool saved;                   // In the saved list
} sys_net_ap_t;

/**
 * @brief Start WiFi and connect to the networks saved in the NVS shared with XiaoZhi
 *        (namespace "wifi", keys ssid/password/channel, ssid1/password1/channel1, ...).
 *        Non-blocking, idempotent. Reads flash (NVS, PHY calibration): call it from a task whose
 *        stack is in internal RAM (app_main is fine), not the LVGL task.
 *
 *        All other functions below are safe from any task, including the LVGL task: requests are
 *        queued to an internal worker task (internal-RAM stack), which does every flash access.
 */
void sys_net_start(void);

sys_net_state_t sys_net_state(void);

/** @brief SSID currently used (or being tried), "" if none. */
const char *sys_net_ssid(void);

/** @brief Current IPv4 address as text; false when not connected. */
bool sys_net_ip(char *out, size_t len);

/** @brief RSSI of the current AP (dBm), 0 when not connected. Updated every ~2 s. */
int sys_net_rssi(void);

/** @brief Host[:port] of XiaoZhi's OTA server (no path: it contains a secret), "" if not set. */
const char *sys_net_server_host(void);

/** @brief Start a scan. Results appear in sys_net_scan_results() when sys_net_scan_gen() changes. */
void sys_net_scan(void);
bool sys_net_scanning(void);
uint32_t sys_net_scan_gen(void);
/** @brief Copy the last scan (strongest first, one entry per SSID). Returns the count. */
int sys_net_scan_results(sys_net_ap_t *out, int max);

/** @brief Copy the saved SSIDs (most preferred first). Returns the count. */
int sys_net_saved(char (*ssids)[33], int max);

/**
 * @brief Connect to `ssid`. `password` NULL = use the saved password. On success the network is
 *        saved in XiaoZhi's format as the preferred one (XiaoZhi uses it after the switch too);
 *        on failure nothing is saved and the saved networks are tried again.
 */
void sys_net_join(const char *ssid, const char *password);
sys_net_join_t sys_net_join_state(void);

/** @brief Remove `ssid` from the saved list (disconnects if it is the current network). */
void sys_net_forget(const char *ssid);

#ifdef __cplusplus
}
#endif
