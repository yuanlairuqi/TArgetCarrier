#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "app_rtos.h"
#include "app_rtos_config.h"
#include "task.h"
static jmp_buf scheduler_jump, task_jump;
static void (*functions[2])(void *);
static unsigned count, step, loops, ticks, led_toggles, led_bits;
static int running_led;
static unsigned boot_stop;
static TIM_HandleTypeDef timer;
static UART_HandleTypeDef uart;
unsigned HAL_NVIC_GetPriorityGrouping(void) { return NVIC_PRIORITYGROUP_4; }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned words,
                             void *arg, unsigned priority, StackType_t *stack, StaticTask_t *tcb)
{
    assert(arg == NULL && stack && tcb && words >= 128U);
    assert(count < 2U && priority == (count == 0U ? 2U : 1U));
    assert(strcmp(name, count == 0U ? "control" : "rgb") == 0);
    functions[count++] = fn;
    return tcb;
}
void vTaskStartScheduler(void) { longjmp(scheduler_jump, 1); }
TickType_t xTaskGetTickCount(void) { return ticks; }
int xTaskDelayUntil(TickType_t *last, TickType_t delay)
{
    assert(delay == (running_led ? APP_LED_TOGGLE_MS : 1U));
    assert(*last == ticks);
    if (!running_led) { assert(step == 4U); step = 2U; }
    else { assert(led_toggles == loops + 1U); assert(led_bits == ((led_toggles % 2U) ? 14U : 0U)); }
    *last += delay;
    ticks += delay;
    if (++loops == 4U) longjmp(task_jump, 1);
    return 1;
}
void RobotUART_Init(UART_HandleTypeDef *p) { assert(p == &uart && step++ == 0U); }
void Motor_StartOnce(TIM_HandleTypeDef *p) { assert(p == &timer && step++ == 1U); }
void RobotUART_Poll(void) { assert(step++ == 2U); }
void Motor_Process(void) { assert(step++ == 3U); }
void Motor_Stop(void) { assert(step == 2U); ++boot_stop; }
void HAL_GPIO_WritePin(void *p,unsigned m,unsigned l) {(void)p;(void)m;(void)l;}
void HAL_GPIO_TogglePin(void *port, unsigned mask)
{
    assert(running_led && port == GPIOA && mask == 14U);
    ++led_toggles; led_bits ^= mask;
}
int main(void)
{
    if (setjmp(scheduler_jump) == 0) AppRTOS_Start(&timer, &uart);
    assert(step == 0U); /* No motion request before scheduling. */
    assert(count == (APP_LED_BLINK_ENABLED ? 2U : 1U));
    if (setjmp(task_jump) == 0) functions[0](NULL);
    assert(loops == 4U && ticks == 4U); assert(boot_stop == (MOTOR_STARTUP_ONESHOT_ENABLED ? 0U : 1U));
#if APP_LED_BLINK_ENABLED
    running_led = 1; loops = 0; ticks = 0;
    if (setjmp(task_jump) == 0) functions[1](NULL);
    assert(led_toggles == 4U && ticks == APP_LED_TOGGLE_MS * 4U);
#endif
    puts("PASS: static tasks, UART-before-motion, 1ms control, RGB timing/config");
    return 0;
}
