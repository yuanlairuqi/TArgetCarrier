/* C++ is limited to STM32duino time/EXTI registration. Public C API preserved. */
#include <Arduino.h>
#include "link_uart.h"
#include "wl_stm.h"
#include "tc_board.h"
#if TC_BACKEND != TC_I2C
#error "Target I2C firmware requires TC_I2C on both receivers"
#endif
static uint32_t rx_bytes,overflows,tx_fail,seen_errors;static bool overflowed;
extern "C" void link_uart_init(void){wl_stm_init();
#ifdef WL_ARDUINO_SPI_CALLBACK
 attachInterrupt(digitalPinToInterrupt(PA4),wl_spi_cs_rising,RISING);
#endif
}
extern "C" uint16_t link_uart_read(uint8_t*out,uint16_t cap){uint32_t e=wl_stm_errors();if(e!=seen_errors){seen_errors=e;overflowed=true;++overflows;}unsigned n=wl_stm_read(out,cap);rx_bytes+=n;return (uint16_t)n;}
extern "C" bool link_uart_take_overflow(void){bool b=overflowed;overflowed=false;return b;}
extern "C" uint32_t link_uart_rx_bytes(void){return rx_bytes;}
extern "C" uint32_t link_uart_overflow_count(void){return overflows;}
extern "C" uint32_t link_uart_tx_fail(void){return tx_fail;}
static void link_uart_write_frame(const uint8_t*d,uint16_t n){if(wl_stm_bound()&&!wl_stm_send(d,n))++tx_fail;}
extern "C" void link_uart_send_status(const status_frame_t *st, uint8_t seq)
{
    if (st == NULL) {
        return;
    }
    uint8_t buf[STATUS_FRAME_SIZE];
    proto_encode_status(buf, seq, st->rssi, st->state,
                        st->yaw_deg, st->pitch_deg, st->temp_mos, st->err_code);
    link_uart_write_frame(buf, STATUS_FRAME_SIZE);
}

extern "C" void link_uart_send_motor(float pos_rad, float vel_rad_s, float tau,
                                     uint16_t can_fail, uint8_t safety, uint8_t cmd_drops,
                                     uint8_t arm_echo, uint8_t seq)
{
    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_motor_ext2(buf, pos_rad, vel_rad_s, tau, can_fail,
                            safety, cmd_drops, arm_echo, seq);
    link_uart_write_frame(buf, CTRL_FRAME_SIZE);
}

extern "C" void link_uart_send_param_reply(uint8_t rid, bool ok, const uint8_t value[4], uint8_t seq)
{
    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_param_reply(buf, rid, ok ? 1u : 0u, value, seq);
    link_uart_write_frame(buf, CTRL_FRAME_SIZE);
}
