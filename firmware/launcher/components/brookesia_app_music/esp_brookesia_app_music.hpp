/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>
#include <vector>
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief Network music player: streams MP3s from the music directory on the XiaoZhi server
 *        (nginx `/<secret>/music/`, see aliyun-deploy/4_music_http.sh).
 *
 *        Zero configuration: WiFi networks and the server URL are read from what XiaoZhi saved
 *        in the shared NVS. Playback keeps running after the app is closed; the UI only sends
 *        commands to music_player and polls its status.
 */
class Music: public systems::phone::App {
public:
    static Music *requestInstance();

    ~Music();

protected:
    Music();

    bool run(void) override;
    bool back(void) override;

private:
    struct Entry {
        std::string name;
        bool is_dir;
    };

    static void onTimer(lv_timer_t *timer);
    static void onRowClicked(lv_event_t *e);
    static void onPrev(lv_event_t *e);
    static void onPlay(lv_event_t *e);
    static void onNext(lv_event_t *e);
    static void onVolume(lv_event_t *e);
    static void fetchTask(void *arg);

    lv_obj_t *createControlButton(lv_obj_t *parent, const char *symbol, int size, lv_event_cb_t cb);
    void refresh();
    void startFetch(const std::string &dir);
    void rebuildList();
    void openEntry(int index);

    // UI (recreated on every run())
    const lv_font_t *_font = nullptr;
    lv_obj_t *_title = nullptr;
    lv_obj_t *_status = nullptr;
    lv_obj_t *_list = nullptr;
    lv_obj_t *_play_label = nullptr;
    lv_obj_t *_volume = nullptr;
    bool _list_dirty = false;       // Entries changed and the list widget must be rebuilt

    // State kept across open/close
    std::string _dir;               // Current relative directory, "" or "a/b/"
    std::vector<Entry> _entries;
    bool _loaded = false;
    bool _loading = false;
    std::string _error;
    uint32_t _error_tick = 0;       // lv_tick of the last failed fetch (for retry)
    SemaphoreHandle_t _lock = nullptr;

    // Filled by fetchTask, applied by refresh() on the LVGL task
    std::string _fetch_dir;
    std::vector<Entry> _fetched;
    bool _fetch_done = false;
    std::string _fetch_error;

    static Music *_instance;
};

} // namespace esp_brookesia::apps
