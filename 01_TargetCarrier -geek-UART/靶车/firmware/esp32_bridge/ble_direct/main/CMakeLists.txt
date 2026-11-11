set(TARGET_LIB "${CMAKE_CURRENT_LIST_DIR}/../../../lib")
idf_component_register(SRCS "capture_core/capture.c" "capture_core/fragment.c" "capture_core/source.c" "main.c" "ble_guard.c" "now_radio.c" "now_protocol.c" "now_session.c"
    "${TARGET_LIB}/linkcore/lc_guard.c" "${TARGET_LIB}/linkcore/lc_frame_rx.c"
    "${TARGET_LIB}/oled/oled.c"
    INCLUDE_DIRS "." "${TARGET_LIB}/protocol" "${TARGET_LIB}/linkcore/include" "${TARGET_LIB}/oled"
    REQUIRES bt nvs_flash esp_timer esp_driver_uart esp_driver_gpio esp_hw_support esp_wifi esp_event esp_netif mbedtls)
target_compile_definitions(${COMPONENT_LIB} PRIVATE OLED_SEG_REMAP=0xA1 OLED_COM_SCAN=0xC0)
