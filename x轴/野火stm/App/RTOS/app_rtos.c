#include "app_rtos.h"
#include "app_rtos_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include "robot_uart.h"
#include "motor_motion.h"

static StaticTask_t control_tcb, idle_tcb;
static StackType_t control_stack[APP_CONTROL_STACK_WORDS];
static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
static TIM_HandleTypeDef *motion_timer;
static UART_HandleTypeDef *control_uart;
const char * volatile g_rtos_fault_file;
volatile int g_rtos_fault_line;
#if APP_LED_BLINK_ENABLED
static StaticTask_t led_tcb;
static StackType_t led_stack[APP_LED_STACK_WORDS];
#endif

static void ControlTask(void *argument)
{
    (void)argument;
    RobotUART_Init(control_uart);
    Motor_StartOnce(motion_timer); /* Preserve the original one-shot startup. */
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        RobotUART_Poll(); /* STOP must be processed before any new batch. */
        Motor_Process();
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_CONTROL_PERIOD_MS));
    }
}

#if APP_LED_BLINK_ENABLED
static void LedTask(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* LED1/2/3 are PA1/PA2/PA3, active low, all initially off. */
        HAL_GPIO_TogglePin(GPIOA, BOARD_LED_R_Pin | BOARD_LED_G_Pin | BOARD_LED_B_Pin);
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_LED_TOGGLE_MS));
    }
}
#endif

void AppRTOS_Start(TIM_HandleTypeDef *timer, UART_HandleTypeDef *uart)
{
    configASSERT(timer != NULL && uart != NULL);
    motion_timer = timer;
    control_uart = uart;
    configASSERT(HAL_NVIC_GetPriorityGrouping() == NVIC_PRIORITYGROUP_4);
    configASSERT(xTaskCreateStatic(ControlTask, "control", APP_CONTROL_STACK_WORDS,
                                  NULL, 2U, control_stack, &control_tcb) != NULL);
#if APP_LED_BLINK_ENABLED
    configASSERT(xTaskCreateStatic(LedTask, "rgb", APP_LED_STACK_WORDS,
                                  NULL, 1U, led_stack, &led_tcb) != NULL);
#endif
    vTaskStartScheduler();
    AppRTOS_Fatal(__FILE__, __LINE__); /* Scheduling must never return. */
}

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *depth)
{
    *tcb = &idle_tcb;
    *stack = idle_stack;
    *depth = configMINIMAL_STACK_SIZE;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    AppRTOS_Fatal(__FILE__, __LINE__);
}

void AppRTOS_Fatal(const char *file, int line)
{
    __disable_irq();
    g_rtos_fault_file = file;
    g_rtos_fault_line = line;
    Motor_Stop(); /* Also forces an already-high PUL output inactive. */
    for (;;) { __NOP(); }
}
