/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYS_TIME_UNSET,       // Nothing known: the clock still counts from 1970
    SYS_TIME_RESTORED,    // Approximate: kept across a soft reset, or last value saved in NVS
    SYS_TIME_SYNCED,      // Synced by SNTP since this boot
} sys_time_state_t;

/**
 * @brief Set the time zone (Beijing, CST-8), restore the last known time and start SNTP.
 *
 *        The board has no RTC chip, so the time is lost on power-off. Recovery order:
 *        1. After a soft reset (e.g. coming back from XiaoZhi) the ESP32 system time survives -> keep it.
 *        2. Otherwise restore the last time saved in NVS (saved every 10 min and on each sync).
 *           This is behind by however long the board was off, so it is marked RESTORED, not SYNCED.
 *        3. SNTP (ntp.aliyun.com, cn.pool.ntp.org) corrects it once WiFi is up, then every hour.
 *
 *        Call after sys_net_start(), from a task whose stack is in internal RAM (reads NVS).
 */
void sys_time_start(void);

sys_time_state_t sys_time_state(void);

#ifdef __cplusplus
}
#endif
