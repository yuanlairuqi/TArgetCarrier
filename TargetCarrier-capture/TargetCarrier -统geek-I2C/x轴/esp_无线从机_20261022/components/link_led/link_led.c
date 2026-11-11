#include "sdkconfig.h"
#include "link_led.h"
#ifdef CONFIG_LINK_LED_ENABLED
#if defined(CONFIG_ROBOT_UART_ENABLED) && (CONFIG_ROBOT_UART_TX_GPIO == 10 || CONFIG_ROBOT_UART_RX_GPIO == 10)
#error GPIO10 WS2812 conflicts with UART configuration
#endif
#if defined(CONFIG_ROBOT_OLED_ENABLED) && (CONFIG_ROBOT_OLED_SCL_GPIO == 10 || CONFIG_ROBOT_OLED_SDA_GPIO == 10)
#error GPIO10 WS2812 conflicts with OLED configuration
#endif
#if defined(CONFIG_ROBOT_PULSE_CAPTURE_ENABLED) && (CONFIG_ROBOT_PULSE_PUL_GPIO == 10 || CONFIG_ROBOT_PULSE_DIR_GPIO == 10)
#error GPIO10 WS2812 conflicts with pulse capture configuration
#endif
#if defined(CONFIG_ROBOT_BATTERY_ENABLED) && CONFIG_ROBOT_BATTERY_CHANNEL == 9
#error GPIO10 WS2812 conflicts with battery ADC configuration
#endif
#include "link_led_state.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* GPIO10 is free on these two slave projects. Do not add this component to
 * GEEK: GPIO10 there is the built-in LCD chip-select. */
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static link_led_snapshot_t latest;
static uint32_t updated_ms;
static bool started;

static uint32_t milliseconds(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void link_led_publish(const link_led_snapshot_t *snapshot)
{
    uint32_t now = milliseconds();
    portENTER_CRITICAL(&state_lock);
    latest = *snapshot;
    updated_ms = now;
    portEXIT_CRITICAL(&state_lock);
}

static void led_task(void *arg)
{
    (void)arg;
    led_strip_config_t config = {
        .strip_gpio_num = 10,
        .max_leds = CONFIG_LINK_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .flags.with_dma = true,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t error = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (error != ESP_OK) {
        ESP_LOGW("link_led", "GPIO10 indicator unavailable: %s; control continues", esp_err_to_name(error));
        vTaskDelete(NULL);
        return;
    }
    uint32_t previous = ~0U;
    uint32_t window_at = milliseconds(), rate = 0, ok = 0, fail = 0, errors = 0;
    link_led_snapshot_t baseline = {0};
    bool was_geek = false, was_linked = false;
    for (;;) {
        link_led_snapshot_t snapshot;
        uint32_t at;
        portENTER_CRITICAL(&state_lock);
        snapshot = latest;
        at = updated_ms;
        portEXIT_CRITICAL(&state_lock);
        uint32_t now = milliseconds();
        bool linked = (uint32_t)(now - at) <= 1000U && (snapshot.ble || snapshot.geek);
        if (was_geek != snapshot.geek || was_linked != linked) {
            baseline = snapshot; window_at = now; rate = ok = fail = errors = 0;
        } else if ((uint32_t)(now - window_at) >= 1000U) {
            uint32_t elapsed = (uint32_t)(now - window_at);
            uint32_t received = snapshot.rx_packets >= baseline.rx_packets ? snapshot.rx_packets - baseline.rx_packets : snapshot.rx_packets;
            rate = (uint32_t)((uint64_t)received * 1000U / elapsed);
            ok = snapshot.tx_ok - baseline.tx_ok;
            fail = snapshot.tx_fail - baseline.tx_fail;
            errors = snapshot.errors - baseline.errors;
            baseline = snapshot; window_at = now;
        }
        was_geek = snapshot.geek; was_linked = linked;
        unsigned connection = link_led_connection(&snapshot, (uint32_t)(now - at), now);
        bool activity = linked && link_led_activity(rate, now);
        unsigned quality = linked ? link_led_quality(&snapshot, now, ok, fail, errors) : 0;
        bool quality_on = linked && link_led_quality_on(quality, now);
        uint32_t pattern = connection | ((uint32_t)activity << 8) | ((uint32_t)quality_on << 9);
        if (pattern != previous) {
            uint32_t level = CONFIG_LINK_LED_BRIGHTNESS;
            for (unsigned i = 0; i < CONFIG_LINK_LED_COUNT; ++i) {
                unsigned color = i == 0 ? connection : i == 1 ? (activity ? LINK_LED_BLUE : LINK_LED_DARK) :
                                 quality_on ? LINK_LED_BLUE : LINK_LED_DARK;
                error = led_strip_set_pixel(strip, i,
                    color == LINK_LED_RED ? level : 0,
                    color == LINK_LED_GREEN ? level : 0,
                    color == LINK_LED_BLUE ? level : 0);
                if (error != ESP_OK) break;
            }
            if (error == ESP_OK) error = led_strip_refresh(strip);
            if (error != ESP_OK) {
                ESP_LOGW("link_led", "LED update failed: %s; indicator disabled", esp_err_to_name(error));
                (void)led_strip_clear(strip);
                (void)led_strip_del(strip);
                vTaskDelete(NULL);
                return;
            }
            previous = pattern;
        }
        vTaskDelay(pdMS_TO_TICKS(50) ? pdMS_TO_TICKS(50) : 1);
    }
}

void link_led_init(void)
{
    if (started) return;
    started = true;
    if (xTaskCreate(led_task, "link_led", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGW("link_led", "Cannot create indicator task; control continues");
}
#else
void link_led_publish(const link_led_snapshot_t *snapshot) { (void)snapshot; }
void link_led_init(void) {}
#endif
