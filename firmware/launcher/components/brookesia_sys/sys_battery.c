/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sys_battery.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SysBattery"

// AXP2101 registers (same ones XiaoZhi's Axp2101 driver reads)
#define AXP2101_ADDR          0x34
#define AXP2101_REG_STATUS1   0x00    // bit3: battery present
#define AXP2101_REG_STATUS2   0x01    // bit6:5: 01 = charging, 10 = discharging
#define AXP2101_REG_PERCENT   0xA4    // Fuel gauge, 0..100
#define POLL_PERIOD_MS        30000
#define I2C_TIMEOUT_MS        50

static i2c_master_dev_handle_t s_dev;
static volatile int s_percent = -1;
static volatile bool s_charging;

static bool read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS) == ESP_OK;
}

static void poll_task(void *arg)
{
    while (true) {
        uint8_t st1 = 0, st2 = 0, pct = 0;
        if (read_reg(AXP2101_REG_STATUS1, &st1) && read_reg(AXP2101_REG_STATUS2, &st2) &&
                read_reg(AXP2101_REG_PERCENT, &pct)) {
            bool present = (st1 & 0x08) != 0;
            s_charging = ((st2 >> 5) & 0x03) == 1;
            s_percent = (present && pct <= 100) ? pct : -1;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void sys_battery_start(void)
{
    if (s_dev != NULL) {
        return;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGE(TAG, "BSP I2C bus not initialised");
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Add AXP2101 device failed");
        s_dev = NULL;
        return;
    }
    xTaskCreate(poll_task, "battery", 3072, NULL, 2, NULL);
}

bool sys_battery_get(int *percent, bool *charging)
{
    if (s_percent < 0) {
        return false;
    }
    *percent = s_percent;
    *charging = s_charging;
    return true;
}
