#include "wifi_board.h"
#include "display/lcd_display.h"
#include "pro_chat_display.h"
#include "esp_lcd_co5300.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <cstring>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

// 圆屏专业聊天界面（pro_chat_display.cc）+ CO5300 的 2 像素对齐要求
class CustomLcdDisplay : public ProChatDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : ProChatDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        ProChatDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch75 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* screen_timer_;    // screen off when idle, on any power source
    PowerSaveTimer* shutdown_timer_;  // power off when idle, only while discharging

    void InitializePowerSaveTimer() {
        // Only counts while the app is idle (no conversation, no audio playing). Wake word,
        // BOOT key and touch all wake it; wake word detection keeps running (cpu_max_freq = -1).
        screen_timer_ = new PowerSaveTimer(-1, SCREEN_OFF_SECONDS, -1);
        screen_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(0);  // AMOLED: brightness 0 = pixels off
        });
        screen_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        screen_timer_->SetEnabled(true);

        // Enabled/disabled by GetBatteryLevel() when the discharging state changes
        shutdown_timer_ = new PowerSaveTimer(-1, -1, BATTERY_SHUTDOWN_SECONDS);
        shutdown_timer_->OnShutdownRequest([this]() { pmic_->PowerOff(); });
    }

    void WakeUpPowerSave() {
        screen_timer_->WakeUp();
        shutdown_timer_->WakeUp();
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");
        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        ESP_ERROR_CHECK(ret);
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        // Unused octal lines default to 0 (= BOOT button GPIO0) with `{}`; mark them unused
        buscfg.data4_io_num = GPIO_NUM_NC;
        buscfg.data5_io_num = GPIO_NUM_NC;
        buscfg.data6_io_num = GPIO_NUM_NC;
        buscfg.data7_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            ReturnToDesktop();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lv_indev_t* touch_indev = lvgl_port_add_touch(&touch_cfg);
        // Touching the screen counts as activity: wakes the screen and resets the idle timers.
        // Runs in the LVGL task, so hand the wake-up to the main task.
        lv_indev_add_event_cb(touch_indev, [](lv_event_t* e) {
            auto board = static_cast<WaveshareEsp32s3TouchAMOLED1inch75*>(lv_event_get_user_data(e));
            Application::GetInstance().Schedule([board]() { board->WakeUpPowerSave(); });
        }, LV_EVENT_PRESSED, this);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

    // 初始化工具
    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });
        mcp_server.AddTool("self.system.return_to_desktop",
            "Close the AI assistant and return to the launcher desktop (the device reboots).\n"
            "Use this when the user says things like \"回到桌面\" or \"退出\".",
            PropertyList(), [this](const PropertyList& properties) {
                return ReturnToDesktop();
            });
    }

    // Dual-boot: a launcher (e.g. esp-brookesia) lives in the other OTA slot.
    // Switch the boot partition to it and reboot; returns false if no launcher is installed.
    bool ReturnToDesktop() {
        const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
        esp_app_desc_t desc = {};
        if (partition == nullptr || esp_ota_get_partition_description(partition, &desc) != ESP_OK ||
            strcmp(desc.project_name, esp_app_get_description()->project_name) == 0) {
            ESP_LOGW(TAG, "No launcher found in the other OTA slot");
            GetDisplay()->ShowNotification("未安装桌面");
            return false;
        }

        ESP_LOGI(TAG, "Return to desktop: %s in slot %s", desc.project_name, partition->label);
        GetDisplay()->ShowNotification("正在回到桌面...");

        // Switch in a separate task: writing otadata freezes the flash cache, so the caller's
        // stack must be in internal RAM (xTaskCreate guarantees that; callbacks may not).
        // The short delay also lets an MCP reply be sent before the reboot.
        xTaskCreate([](void* arg) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            auto partition = static_cast<const esp_partition_t*>(arg);
            esp_err_t err = esp_ota_set_boot_partition(partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Set boot partition %s failed: %s", partition->label, esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            Application::GetInstance().Reboot();
            vTaskDelete(NULL);
        }, "to_desktop", 4096, const_cast<esp_partition_t*>(partition), 5, NULL);
        return true;
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch75() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
#if CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_AMOLED_1_75
        InitializeTca9554();
#endif
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        // The shutdown timer starts disabled: enable it only while running on battery
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            shutdown_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            WakeUpPowerSave();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch75);
