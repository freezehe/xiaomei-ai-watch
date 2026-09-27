/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "screen_sleep.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

#define TAG "ScreenSleep"
#define CHECK_PERIOD_MS 500

static uint32_t s_timeout_ms;
static int s_brightness = 100;
static int s_inhibit_count;
static bool s_asleep;
static lv_obj_t *s_blocker;     // Swallows the wake-up touch while the screen is off
static lv_timer_t *s_timer;

static void on_blocker_pressed(lv_event_t *e)
{
    (void)e;
    screen_sleep_wake();
}

static void enter_sleep(void)
{
    ESP_LOGI(TAG, "Screen off after %lu s idle", (unsigned long)(s_timeout_ms / 1000));
    // Blocker first, so a touch arriving right now is already swallowed
    s_blocker = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_blocker);
    lv_obj_set_size(s_blocker, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_blocker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_blocker, on_blocker_pressed, LV_EVENT_PRESSED, NULL);
    bsp_display_brightness_set(0);  // AMOLED: brightness 0 = pixels off
    s_asleep = true;
}

static void check_idle(lv_timer_t *timer)
{
    (void)timer;
    if (s_asleep || s_inhibit_count > 0) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= s_timeout_ms) {
        enter_sleep();
    }
}

void screen_sleep_start(uint32_t timeout_s, int brightness)
{
    s_timeout_ms = timeout_s * 1000;
    s_brightness = brightness;
    if (s_timer == NULL) {
        s_timer = lv_timer_create(check_idle, CHECK_PERIOD_MS, NULL);
    }
    ESP_LOGI(TAG, "Screen off after %lu s without touch", (unsigned long)timeout_s);
}

void screen_sleep_wake(void)
{
    lv_display_trigger_activity(NULL);  // Restart the idle countdown
    if (!s_asleep) {
        return;
    }
    bsp_display_brightness_set(s_brightness);
    if (s_blocker != NULL) {
        // May be called from the blocker's own event: delete it after the event finishes
        lv_obj_delete_async(s_blocker);
        s_blocker = NULL;
    }
    s_asleep = false;
    ESP_LOGI(TAG, "Screen on");
}

void screen_sleep_inhibit(bool inhibit)
{
    if (inhibit) {
        s_inhibit_count++;
        screen_sleep_wake();
    } else if (s_inhibit_count > 0) {
        s_inhibit_count--;
        lv_display_trigger_activity(NULL);  // Full timeout again after the inhibit ends
    }
}

bool screen_sleep_is_asleep(void)
{
    return s_asleep;
}
