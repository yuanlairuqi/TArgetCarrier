idf_component_register(SRCS "main.c" "now_protocol.c" "now_radio.c" "robot_control.c" "battery.c" "robot_bridge.c" "uart_link.c" "pulse_capture.c" "pulse_wire.c" "../shared/robot_wire.c"
    INCLUDE_DIRS "." "../shared" REQUIRES mbedtls esp_wifi esp_event esp_netif bt nvs_flash esp_timer esp_adc esp_driver_uart esp_driver_gpio esp_driver_mcpwm esp_hw_support)
