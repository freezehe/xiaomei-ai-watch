idf_component_register(
    SRCS "sys_net.c" "sys_time.c" "sys_battery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_i2c
)
