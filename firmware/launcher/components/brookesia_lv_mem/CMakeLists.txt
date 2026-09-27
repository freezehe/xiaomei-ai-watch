# LVGL allocator (CONFIG_LV_USE_CUSTOM_MALLOC). WHOLE_ARCHIVE: lvgl references these symbols,
# nothing else does, so the linker must not drop them.
idf_component_register(
    SRCS "lv_mem_psram.c"
    PRIV_REQUIRES lvgl__lvgl heap
    WHOLE_ARCHIVE
)
