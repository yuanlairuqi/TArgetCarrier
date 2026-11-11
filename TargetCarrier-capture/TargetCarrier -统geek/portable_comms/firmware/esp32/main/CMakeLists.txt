set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
idf_component_register(SRCS "main.c" "${ROOT}/core/tc_protocol.c" "${ROOT}/core/tc_params.c" "${ROOT}/core/tc_wave.c" "${ROOT}/platform/esp_radio.c" "${ROOT}/platform/esp_bus.c"
 INCLUDE_DIRS "${ROOT}/core" "${ROOT}/platform" "${ROOT}/config"
 REQUIRES esp_driver_uart esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_wifi esp_event esp_netif nvs_flash esp_timer mbedtls)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TC_ROLE=${TC_ROLE} TC_BACKEND=${TC_BACKEND})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function -Wno-misleading-indentation)
