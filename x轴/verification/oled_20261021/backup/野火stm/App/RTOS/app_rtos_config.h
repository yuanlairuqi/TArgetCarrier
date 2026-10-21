#ifndef APP_RTOS_CONFIG_H
#define APP_RTOS_CONFIG_H
#ifndef APP_LED_BLINK_ENABLED
#define APP_LED_BLINK_ENABLED 0
#endif
#ifndef APP_LED_TOGGLE_MS
#define APP_LED_TOGGLE_MS 100U
#endif
#ifndef APP_CONTROL_PERIOD_MS
#define APP_CONTROL_PERIOD_MS 1U
#endif
#define APP_CONTROL_STACK_WORDS 512U
#define APP_LED_STACK_WORDS 128U
#if APP_LED_BLINK_ENABLED != 0 && APP_LED_BLINK_ENABLED != 1
#error APP_LED_BLINK_ENABLED must be 0 or 1
#endif
#if APP_LED_TOGGLE_MS < 1 || APP_LED_TOGGLE_MS > 60000
#error APP_LED_TOGGLE_MS must be between 1 and 60000
#endif
#if APP_CONTROL_PERIOD_MS != 1
#error Keep the control service interval at 1 ms
#endif
#endif
