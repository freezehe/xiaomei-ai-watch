/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <cstring>
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "boost/thread.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"
#include "esp_brookesia_app_xiaozhi.hpp"
#include "screen_sleep.h"
#include "sys_net.h"
#include "sys_time.h"
#include "sys_battery.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lv_adapter_display.h"

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

constexpr bool EXAMPLE_SHOW_MEM_INFO = false;
constexpr uint32_t LVGL_TASK_STACK_SIZE = 40 * 1024;
constexpr uint32_t SCREEN_OFF_SECONDS = 120;   // Same as XiaoZhi (config.h SCREEN_OFF_SECONDS)
constexpr int SCREEN_BRIGHTNESS = 100;         // bsp_display_backlight_on() default
constexpr size_t DISPLAY_BOUNCE_BYTES = BSP_LCD_H_RES * 50 * 2;  // One LVGL band: BSP buffer_height 50, RGB565

/* LVGL draws into PSRAM buffers, and the SPI driver can only DMA from internal RAM, so for every
 * 466x50 band it used to malloc a ~46 KB internal DMA bounce buffer. With WiFi + music running that
 * allocation fails ("Failed to allocate priv TX buffer"), the band is skipped and the old screen
 * content stays there. Instead, reserve one bounce buffer at boot and copy each band into it.
 * Safe with a single buffer: LVGL waits for the previous flush to finish before the next one. */
static uint8_t *s_display_bounce;

static esp_err_t draw_bitmap_via_bounce(lv_display_t *disp, esp_lcd_panel_handle_t panel,
                                        int x_start, int y_start, int x_end, int y_end,
                                        const void *color_map, void *user_ctx)
{
    const size_t bytes = (size_t)(x_end - x_start) * (y_end - y_start) * 2;
    if (bytes <= DISPLAY_BOUNCE_BYTES) {
        memcpy(s_display_bounce, color_map, bytes);
        color_map = s_display_bounce;
    }
    return esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_map);
}

static void install_display_bounce_buffer(lv_display_t *disp)
{
    s_display_bounce = (uint8_t *)heap_caps_malloc(DISPLAY_BOUNCE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_display_bounce == nullptr) {
        ESP_UTILS_LOGE("No internal DMA memory for the display bounce buffer");
        return;
    }
    const esp_lv_adapter_draw_bitmap_callbacks_t cbs = {
        .custom_draw_bitmap = draw_bitmap_via_bounce,
    };
    ESP_UTILS_CHECK_ERROR_EXIT(esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, nullptr),
                               "Set draw bitmap callbacks failed");
}

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("Display ESP-Brookesia phone demo");

    /* Dual-boot with XiaoZhi: confirm this image so the bootloader does not roll back */
    esp_brookesia::apps::XiaoZhi::markRunningAppValid();

    /* Brookesia screen creation exceeds the adapter's 8 KB default on this board. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lv_adapter_config_t lv_adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
#pragma GCC diagnostic pop
    lv_adapter_config.task_stack_size = LVGL_TASK_STACK_SIZE;
    lv_adapter_config.stack_in_psram = true;

    bsp_display_cfg_t display_config = {};
    display_config.lv_adapter_cfg = lv_adapter_config;
    display_config.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.touch_flags.mirror_x = 1;
    display_config.touch_flags.mirror_y = 1;

    lv_display_t *disp = bsp_display_start_with_config(&display_config);
    ESP_UTILS_CHECK_NULL_EXIT(disp, "Start display failed");
    install_display_bounce_buffer(disp);
    ESP_UTILS_CHECK_ERROR_EXIT(bsp_display_backlight_on(), "Turn on display backlight failed");

    /* WiFi (networks saved by XiaoZhi), time (NVS restore + SNTP) and battery gauge. These read
     * flash, so they run here in the main task (internal-RAM stack), not in the LVGL task. */
    sys_net_start();
    sys_time_start();
    sys_battery_start();

    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        esp_err_t ret = bsp_display_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, false, "Lock failed (timeout_ms: %d)", timeout_ms);

        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    /* Create a phone object */
    Phone *phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");

    {
        // When operating on non-GUI tasks, should acquire a lock before operating on LVGL
        LvLockGuard gui_guard;

        /* Begin the phone */
        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");
        // assert(phone->getDisplay().showContainerBorder() && "Show container border failed");

        /* Init and install apps from registry */
        std::vector<systems::base::Manager::RegistryAppInfo> inited_apps;
        ESP_UTILS_CHECK_FALSE_EXIT(phone->initAppFromRegistry(inited_apps), "Init app registry failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installAppFromRegistry(inited_apps), "Install app registry failed");

        ESP_UTILS_LOGI("Internal RAM after start: free %u, largest block %u, DMA free %u",
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

        /* Turn the screen off after a while without touch; the next touch turns it back on */
        screen_sleep_start(SCREEN_OFF_SECONDS, SCREEN_BRIGHTNESS);

        /* Create a timer to update the clock */
        lv_timer_create([](lv_timer_t *t) {
            time_t now;
            struct tm timeinfo;
            Phone *phone = (Phone *)t->user_data;

            ESP_UTILS_CHECK_NULL_EXIT(phone, "Invalid phone");

            time(&now);
            localtime_r(&now, &timeinfo);

            auto status_bar = phone->getDisplay().getStatusBar();
            ESP_UTILS_CHECK_FALSE_EXIT(
                status_bar->setClock(timeinfo.tm_hour, timeinfo.tm_min), "Refresh status bar failed"
            );

            /* Only touch the icons when something changed, so the bar is not redrawn every second */
            static int last_wifi = -1;
            static int last_battery = -1;
            int wifi = (sys_net_state() == SYS_NET_CONNECTED) ? 3 : 0;
            if (wifi != last_wifi) {
                last_wifi = wifi;
                status_bar->setWifiIconState(wifi);
            }
            int percent = 0;
            bool charging = false;
            if (sys_battery_get(&percent, &charging) && (percent * 2 + charging) != last_battery) {
                last_battery = percent * 2 + charging;
                status_bar->setBatteryPercent(charging, percent);
            }
        }, 1000, phone);
    }

    if constexpr (EXAMPLE_SHOW_MEM_INFO) {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([ = ]() {
            char buffer[128];    /* Make sure buffer is enough for `sprintf` */
            size_t internal_free = 0;
            size_t internal_total = 0;
            size_t external_free = 0;
            size_t external_total = 0;

            while (1) {
                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                sprintf(buffer,
                        "\t           Biggest /     Free /    Total\n"
                        "\t  SRAM : [%8d / %8d / %8d]\n"
                        "\t PSRAM : [%8d / %8d / %8d]",
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), internal_free, internal_total,
                        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), external_free, external_total);
                ESP_UTILS_LOGI("\n%s", buffer);

                {
                    LvLockGuard gui_guard;
                    ESP_UTILS_CHECK_FALSE_EXIT(
                        phone->getDisplay().getRecentsScreen()->setMemoryLabel(
                            internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
                        ), "Set memory label failed"
                    );
                }

                boost::this_thread::sleep_for(boost::chrono::seconds(5));
            }
        }).detach();
    }
}