#include "limit_inputs.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"

static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static bool latched, ready;
static bool high_tracking;
static uint32_t high_since;
static TaskHandle_t stop_task;
static void IRAM_ATTR on_limit(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&guard);
    latched = true;
    high_tracking = false;
    portEXIT_CRITICAL_ISR(&guard);
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(stop_task, &wake);
    if (wake) portYIELD_FROM_ISR();
}
static bool occupied(int pin) {
    (void)pin;
#ifdef CONFIG_ROBOT_UART_ENABLED
    if (pin == CONFIG_ROBOT_UART_TX_GPIO || pin == CONFIG_ROBOT_UART_RX_GPIO) return true;
#endif
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
    if (pin == CONFIG_ROBOT_PULSE_PUL_GPIO || pin == CONFIG_ROBOT_PULSE_DIR_GPIO) return true;
#endif
#ifdef CONFIG_ROBOT_OLED_ENABLED
    if (pin == CONFIG_ROBOT_OLED_SCL_GPIO || pin == CONFIG_ROBOT_OLED_SDA_GPIO) return true;
#endif
#ifdef CONFIG_ROBOT_BATTERY_ENABLED
    if (pin == CONFIG_ROBOT_BATTERY_CHANNEL + 1) return true;
#endif
    return false;
}
bool limit_inputs_init(TaskHandle_t waiter) {
    if (!waiter || occupied(1) || occupied(2)) {
        ESP_LOGE("limit", "GPIO1/2 conflict or missing task: movement inhibited");
        return false;
    }
    stop_task = waiter;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << 1) | (1ULL << 2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    /* Do not reuse an unknown ISR service: its IRAM settings may differ. */
    if (err == ESP_OK) err = gpio_isr_handler_add(GPIO_NUM_1, on_limit, NULL);
    if (err == ESP_OK) err = gpio_isr_handler_add(GPIO_NUM_2, on_limit, NULL);
    if (err != ESP_OK) {
        ESP_LOGE("limit", "Input setup failed (%s): movement inhibited", esp_err_to_name(err));
        return false;
    }
    portENTER_CRITICAL(&guard);
    ready = true;
    if (!gpio_get_level(GPIO_NUM_1) || !gpio_get_level(GPIO_NUM_2)) latched = true;
    portEXIT_CRITICAL(&guard);
    ESP_LOGI("limit", "GPIO1 OR GPIO2 LOW: latched UART emergency STOP; external 10k pull-ups to 3.3V required");
    xTaskNotifyGive(stop_task);
    return true;
}
bool limit_inputs_blocked(void) {
    portENTER_CRITICAL(&guard);
    if (ready && (!gpio_get_level(GPIO_NUM_1) || !gpio_get_level(GPIO_NUM_2))) {
        latched = true;
        high_tracking = false;
    }
    bool blocked = !ready || latched;
    portEXIT_CRITICAL(&guard);
    return blocked;
}
void limit_inputs_sample(uint32_t now) {
    portENTER_CRITICAL(&guard);
    if (!ready || !gpio_get_level(GPIO_NUM_1) || !gpio_get_level(GPIO_NUM_2)) {
        high_tracking = false;
        if (ready) latched = true;
    } else if (!high_tracking) {
        high_tracking = true;
        high_since = now;
    }
    portEXIT_CRITICAL(&guard);
}
bool limit_inputs_try_reset(uint32_t now) {
    portENTER_CRITICAL(&guard);
    bool allowed = ready && high_tracking && (uint32_t)(now-high_since) >= 100U &&
        gpio_get_level(GPIO_NUM_1) && gpio_get_level(GPIO_NUM_2);
    if (allowed) latched = false;
    portEXIT_CRITICAL(&guard);
    return allowed;
}
