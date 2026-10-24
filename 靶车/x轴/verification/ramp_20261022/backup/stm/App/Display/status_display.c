#include "status_display.h"
#include "oled_config.h"
#include "oled_text.h"
#include "FreeRTOS.h"
#include "task.h"
#include "motor_motion.h"
#include "motor_config.h"
#include "robot_uart.h"
#include <stdio.h>

typedef struct {
    uint32_t uptime, rx, rx_errors, tx_errors, hz;
    MotorState state;
    uint8_t linked, direction, output, fault;
} display_state_t;
static display_state_t latest;
static StaticTask_t display_tcb;
static StackType_t display_stack[384];
volatile uint32_t g_oled_errors;
volatile uint8_t g_oled_address, g_oled_online;

void StatusDisplay_Publish(void)
{
    display_state_t s={0};
    s.uptime=HAL_GetTick()/1000U;s.state=g_motor_status.state;
    s.linked=g_robot_uart_status.linked;s.rx=g_robot_uart_status.frames_received;
    s.rx_errors=g_robot_uart_status.rx_faults;s.tx_errors=g_robot_uart_status.tx_faults;
    s.fault=Motor_HasFault();
    s.direction=(MOTOR_DIR_GPIO_Port->ODR&MOTOR_DIR_Pin)?1U:0U;
    s.output=(s.state==MOTOR_RUNNING || Motor_OutputActive()) && (TIM1->CR1&TIM_CR1_CEN);
    if (s.output) s.hz=MOTOR_TIMER_INPUT_HZ/(TIM1->PSC+1U)/(TIM1->ARR+1U);
    taskENTER_CRITICAL();latest=s;taskEXIT_CRITICAL();
}
/* Software I2C: only these two open-drain GPIOs, no IRQ masking and no HAL I2C
 * dependency. ControlTask and UART interrupts may preempt every bit. */
static void delay_us(uint32_t us)
{
    uint32_t cycles=(SystemCoreClock/1000000U)*us, start=DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT-start)<cycles) { __NOP(); }
}
static void sda(bool high) { HAL_GPIO_WritePin(GPIOB,GPIO_PIN_7,high?GPIO_PIN_SET:GPIO_PIN_RESET); }
static void scl(bool high) { HAL_GPIO_WritePin(GPIOB,GPIO_PIN_6,high?GPIO_PIN_SET:GPIO_PIN_RESET); }
static bool clock_high(void)
{
    scl(true);uint32_t start=HAL_GetTick();
    while (HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_6)==GPIO_PIN_RESET) {
        if ((uint32_t)(HAL_GetTick()-start)>=2U) return false;
    }
    delay_us(5);return true;
}
static bool byte_out(uint8_t value)
{
    for (unsigned bit=0;bit<8;bit++) {
        scl(false);sda((value&0x80U)!=0U);delay_us(5);
        if (!clock_high()) return false;
        scl(false);value<<=1;
    }
    sda(true);delay_us(5);
    if (!clock_high()) return false;
    bool ack=HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_7)==GPIO_PIN_RESET;
    scl(false);return ack;
}
static bool write_bus(void *ctx,uint8_t address,const uint8_t *bytes,size_t size)
{
    (void)ctx;sda(true);
    bool ok=clock_high() && HAL_GPIO_ReadPin(GPIOB,GPIO_PIN_7)==GPIO_PIN_SET;
    if (ok) {
        sda(false);delay_us(5);scl(false);
        ok=byte_out((uint8_t)(address<<1));
        for (size_t i=0;ok && i<size;i++) ok=byte_out(bytes[i]);
    }
    scl(false);sda(false);delay_us(5);
    bool stopped=clock_high();sda(true);delay_us(5);
    return ok && stopped;
}
static void bus_init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_6|GPIO_PIN_7,GPIO_PIN_SET);
    GPIO_InitTypeDef pins={0};pins.Pin=GPIO_PIN_6|GPIO_PIN_7;
    pins.Mode=GPIO_MODE_OUTPUT_OD;pins.Pull=GPIO_NOPULL;pins.Speed=GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB,&pins);
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
}
static const char *state_name(MotorState state)
{
    switch (state) {
    case MOTOR_NOT_STARTED:return "INIT";
    case MOTOR_WAITING:return "WAIT";
    case MOTOR_RUNNING:return "ONESHOT";
    case MOTOR_DONE:return "DONE";
    case MOTOR_FAULT:return "FAULT";
    case MOTOR_ABORTED:return "STOP";
    case MOTOR_CONTINUOUS_HOLD:return "DIR HOLD";
    case MOTOR_CONTINUOUS_WAITING:return "DIR WAIT";
    case MOTOR_CONTINUOUS_RUNNING:return "CONTINUOUS";
    default:return "UNKNOWN";
    }
}
static void DisplayTask(void *argument)
{
    (void)argument;bus_init();vTaskDelay(pdMS_TO_TICKS(100));
    oled_text_t d={.write=write_bus,.height=APP_OLED_HEIGHT,
        .sh1106=APP_OLED_SH1106,.column_offset=APP_OLED_SH1106?2:0};
    for (;;) {
        if (!g_oled_online) {
            const uint8_t candidates[]={APP_OLED_ADDRESS?APP_OLED_ADDRESS:0x3c,0x3d};
            for (unsigned i=0;i<(APP_OLED_ADDRESS?1U:2U);i++) {
                d.address=candidates[i];
                if (oled_text_init(&d)) {g_oled_online=1;g_oled_address=d.address;break;}
            }
        }
        if (g_oled_online) {
            display_state_t s;taskENTER_CRITICAL();s=latest;taskEXIT_CRITICAL();
            char lines[8][22]={{0}};
            snprintf(lines[0],22,"STM32 X AXIS");
            snprintf(lines[1],22,"UART %s RX %lu",s.linked?"ON":"OFF",(unsigned long)s.rx);
            snprintf(lines[2],22,"STATE %s",state_name(s.state));
            snprintf(lines[3],22,"SET HZ %lu",(unsigned long)s.hz);
            snprintf(lines[4],22,"DIR %u OUT %s",s.direction,s.output?"ON":"OFF");
            snprintf(lines[5],22,"ERR M%u R%lu T%lu",s.fault,(unsigned long)s.rx_errors,(unsigned long)s.tx_errors);
            snprintf(lines[6],22,"UP %lu S",(unsigned long)s.uptime);
            snprintf(lines[7],22,"LOGIC STATUS ONLY");
            if (!oled_text_frame(&d,(const char (*)[22])lines,s.uptime/3U)) g_oled_online=0;
        }
        if (!g_oled_online) {++g_oled_errors;g_oled_address=0;}
        vTaskDelay(pdMS_TO_TICKS(g_oled_online?APP_OLED_PERIOD_MS:3000U));
    }
}
void StatusDisplay_Start(void)
{
    /* Failure is diagnostic only: the motion control task already exists. */
    if (xTaskCreateStatic(DisplayTask,"oled",384,NULL,1U,display_stack,&display_tcb)==NULL)
        ++g_oled_errors;
}
