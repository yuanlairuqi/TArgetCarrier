idf_component_register(SRCS "link_led.c" INCLUDE_DIRS "."
    REQUIRES led_strip esp_timer freertos)
