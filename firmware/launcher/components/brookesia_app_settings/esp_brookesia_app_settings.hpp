/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>
#include <vector>
#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * @brief Settings: pick a WiFi network by hand.
 *
 *        Networks are saved in the NVS shared with XiaoZhi (same format as its SsidManager), so
 *        after switching to AI Chat XiaoZhi connects to the same network and keeps using the
 *        OTA server (Aliyun) it already knows. All WiFi work is done by brookesia_sys (sys_net).
 */
class Settings: public systems::phone::App {
public:
    static Settings *requestInstance();

    ~Settings();

protected:
    Settings();

    bool run(void) override;
    bool back(void) override;

private:
    enum class Page {
        MAIN,           // Status + network list
        PASSWORD,       // Password entry
        JOINING,        // Connecting / result
    };

    struct Row {
        std::string ssid;
        int rssi;
        bool secure;
        bool saved;
        bool in_range;
    };

    static void onTimer(lv_timer_t *timer);
    static void onRowClicked(lv_event_t *e);
    static void onRescan(lv_event_t *e);
    static void onSheetButton(lv_event_t *e);
    static void onKeyboard(lv_event_t *e);
    static void onEyeToggle(lv_event_t *e);
    static void onPasswordCancel(lv_event_t *e);
    static void onPasswordConnect(lv_event_t *e);
    static void onResultButton(lv_event_t *e);

    void buildMain(lv_obj_t *scr);
    void buildPassword(lv_obj_t *scr);
    void buildJoining(lv_obj_t *scr);
    lv_obj_t *createButton(lv_obj_t *parent, const char *text, uint32_t color, int w, lv_event_cb_t cb);
    void showPage(Page page);
    void refresh();
    void refreshStatus();
    void refreshJoin();
    void rebuildList();
    void openRow(int index);
    void showSheet(const Row &row, bool connected);
    void startJoin(const std::string &ssid, const char *password);

    // UI (recreated on every run())
    const lv_font_t *_font = nullptr;
    lv_obj_t *_screen = nullptr;
    lv_obj_t *_page_main = nullptr;
    lv_obj_t *_page_password = nullptr;
    lv_obj_t *_page_joining = nullptr;
    lv_obj_t *_status_dot = nullptr;
    lv_obj_t *_status_title = nullptr;
    lv_obj_t *_status_detail = nullptr;
    lv_obj_t *_rescan_btn = nullptr;
    lv_obj_t *_rescan_spinner = nullptr;
    lv_obj_t *_list = nullptr;
    lv_obj_t *_server = nullptr;
    lv_obj_t *_pw_title = nullptr;
    lv_obj_t *_pw_text = nullptr;
    lv_obj_t *_join_spinner = nullptr;
    lv_obj_t *_join_icon = nullptr;
    lv_obj_t *_join_text = nullptr;
    lv_obj_t *_join_button = nullptr;
    lv_obj_t *_sheet = nullptr;

    Page _page = Page::MAIN;
    std::vector<Row> _rows;
    uint32_t _scan_gen = 0;
    std::string _saved_sig;         // Saved SSIDs joined: detects changes of the saved list
    std::string _connected_ssid;    // Connected SSID the list was built with
    std::string _target;            // SSID of the password page / current join
    std::string _sheet_ssid;
    int _last_join = -1;
    uint32_t _join_ok_tick = 0;

    static Settings *_instance;
};

} // namespace esp_brookesia::apps
