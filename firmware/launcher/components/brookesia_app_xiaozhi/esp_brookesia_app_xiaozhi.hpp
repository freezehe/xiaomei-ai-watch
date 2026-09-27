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
 * @brief Launcher entry for the XiaoZhi AI voice assistant (dual-boot).
 *
 *        XiaoZhi firmware lives in the other OTA slot of the same flash (launcher = ota_0,
 *        XiaoZhi = ota_1, see partitions.csv). Opening this
 *        app switches the boot partition to that slot and restarts; XiaoZhi can later
 *        switch back to this launcher (long-press BOOT or say "回到桌面").
 */
class XiaoZhi: public systems::phone::App {
public:
    static XiaoZhi *requestInstance();

    ~XiaoZhi();

    /**
     * @brief Mark the running launcher image as valid.
     *
     *        XiaoZhi is built with app rollback enabled, so when it switches back to this slot
     *        the image may boot as "pending verify" (if a rollback-enabled bootloader is used).
     *        Without this call the next reset would roll back to XiaoZhi. Safe on every boot.
     */
    static void markRunningAppValid();

protected:
    XiaoZhi();

    // Called when the app is opened: show a hint, then reboot into XiaoZhi.
    bool run(void) override;

    // Called on the back gesture: notify the core to close the app.
    bool back(void) override;

private:
    static void onSwitchTimer(lv_timer_t *timer);

    lv_obj_t *_hint_label = nullptr;   // Status text ("Starting..." or an error)

    static XiaoZhi *_instance;         // Singleton instance
};

} // namespace esp_brookesia::apps
