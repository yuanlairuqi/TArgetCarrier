idf_component_register(SRCS "exfat_build_check.c" "capture_core/wave_store.c" "capture_core/sd_geek.c" "main.c" "master_guard.c" "usb_wire.c" "panel.c" "now_radio.c" "now_protocol.c" "robot_control.c"
    INCLUDE_DIRS "." REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_usb_serial_jtag esp_driver_spi esp_driver_gpio esp_driver_ledc esp_wifi esp_event esp_netif nvs_flash esp_timer mbedtls)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DUAL_NOW_MASTER=1)
