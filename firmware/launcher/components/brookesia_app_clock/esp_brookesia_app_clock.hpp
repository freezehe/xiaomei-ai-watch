/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief Full-screen watch face for the 466x466 round AMOLED, two faces (tap to switch):
 *        - Digital: big HH:MM, seconds ring, date + weekday, lunar date, WiFi / battery / sync state
 *        - Analog : 60-tick dial with hour / minute / second hands, weekday + day, lunar date
 *        Time comes from brookesia_sys (SNTP + NVS restore); until it is known the face says so
 *        instead of showing 1970.
 */
class Clock: public systems::phone::App {
public:
    static Clock *requestInstance();

    ~Clock();

protected:
    Clock();

    // Called when the app is opened: create all UI here (on the default screen).
    bool run(void) override;

    // Called on the back gesture: notify the core to close the app.
    bool back(void) override;

private:
    enum Face {
        FACE_DIGITAL = 0,
        FACE_ANALOG,
    };

    static void onTimer(lv_timer_t *timer);
    static void onClicked(lv_event_t *e);

    void createDigitalFace(lv_obj_t *parent);
    void createAnalogFace(lv_obj_t *parent);
    void showFace(Face face);
    void refresh(bool force);
    void refreshStatus(bool force);

    // Digital face
    lv_obj_t *_digital = nullptr;
    lv_obj_t *_sec_arc = nullptr;
    lv_obj_t *_wifi_label = nullptr;
    lv_obj_t *_battery_label = nullptr;
    lv_obj_t *_date_label = nullptr;
    lv_obj_t *_time_label = nullptr;
    lv_obj_t *_lunar_label = nullptr;
    lv_obj_t *_sync_dot = nullptr;
    lv_obj_t *_sync_label = nullptr;

    // Analog face
    lv_obj_t *_analog = nullptr;
    lv_obj_t *_dial = nullptr;
    lv_obj_t *_hour_hand = nullptr;
    lv_obj_t *_min_hand = nullptr;
    lv_obj_t *_sec_hand = nullptr;
    lv_obj_t *_a_date_label = nullptr;
    lv_obj_t *_a_lunar_label = nullptr;
    lv_obj_t *_a_sync_label = nullptr;

    // Last drawn values: only changed widgets are updated, so only their area is redrawn
    int _last_sec = -1;
    int _last_min = -1;
    int _last_yday = -1;
    int _last_time_state = -1;
    int _last_net_state = -1;
    int _last_battery = -2;

    static Face _face;                 // Kept while the launcher runs
    static Clock *_instance;           // Singleton instance
};

} // namespace esp_brookesia::apps
