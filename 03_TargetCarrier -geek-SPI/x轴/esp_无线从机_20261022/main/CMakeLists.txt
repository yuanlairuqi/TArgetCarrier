idf_component_register(SRCS "capture_core/capture.c" "capture_core/fragment.c" "capture_core/source.c" "main.c" "limit_inputs.c" "oled_status.c" "oled_text.c" "now_protocol.c" "now_radio.c" "robot_control.c" "battery.c" "robot_bridge.c" "uart_link.c" "pulse_capture.c" "pulse_wire.c" "../shared/robot_wire.c"
    INCLUDE_DIRS "." "../shared" REQUIRES esp_driver_spi esp_driver_i2c esp_driver_i2c mbedtls esp_wifi esp_event esp_netif bt nvs_flash esp_timer esp_adc esp_driver_uart esp_driver_gpio esp_driver_mcpwm esp_hw_support)

set(WIRED_TRANSPORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../wired_transport" CACHE PATH "Shared SPI/I2C adapter")
set(WIRED_NODE 1)
include(${WIRED_TRANSPORT_ROOT}/wired_esp.cmake)
