idf_component_register(SRCS "main.c" "usb_wire.c" "panel.c" "now_radio.c" "now_protocol.c" "robot_control.c"
    INCLUDE_DIRS "." REQUIRES esp_driver_usb_serial_jtag esp_driver_spi esp_driver_gpio esp_driver_ledc esp_wifi esp_event esp_netif nvs_flash esp_timer mbedtls)
