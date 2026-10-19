/**
 * @file    link_uart.cpp
 * @brief   STM32 <-> M5Stamp 桥接端的 USART1 链路
 *
 *  ★ 为什么这个文件是 .cpp:
 *    STM32duino 的 Serial1 是 C++ 类, 纯 C 文件包含 Arduino 头会直接报
 *    "unknown type name 'namespace'"。链路这一层用 C++, 对外仍只暴露
 *    C 接口 (link_uart.h), app_main.c 保持纯 C 可移植。
 *
 *  ★ 为什么用"轮询 Serial1.read()"而不是注册 RX 中断:
 *    两条路都试过, 这里说清楚取舍, 免得后人再踩:
 *      - 核心的 uart_attach_rx_callback() 是"独占"式接管: HAL 的
 *        HAL_UART_RxCpltCallback 只会调用这个用户回调, 不再往 Arduino 的
 *        _rx_buffer 里塞字节 (见核心 uart.c)。也就是说一旦注册回调,
 *        Serial1.read() 就再也读不到数据了。
 *      - 而注册回调需要拿到 serial_t*, 核心没有公开这个句柄
 *        (Uart::_serial 是 protected, HardwareSerial 也没提供访问器),
 *        只能去猜对象内存布局 —— 太脆, 不值得。
 *    本应用的量级: 主机 50Hz 发 17 字节 = 850 B/s, 环形缓冲 256 字节,
 *    主循环 1kHz 轮询一次, 余量极大。所以直接轮询最简单也最稳。
 *
 *  ★ 时间戳: 不引入 RTOS, 接收时间戳由主循环在 poll 时打, 1ms 分辨率,
 *    对 300ms 看门狗完全够用。
 */

#include <Arduino.h>
#include <Serial.h>

#include "link_uart.h"
#include "config.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 接收状态机 (主循环上下文)                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    RX_WAIT_HEAD = 0,
    RX_COLLECT
} rx_state_t;

static rx_state_t      s_state = RX_WAIT_HEAD;
static uint8_t         s_buf[CTRL_FRAME_SIZE];
static uint8_t         s_idx = 0;

/* 最新一帧有效命令 */
static bool            s_pending = false;
static ctrl_payload_t  s_pay;
static uint8_t         s_seq;
static uint32_t        s_rx_ms;

static uint32_t        s_now_ms = 0;
static uint32_t        s_rx_ok  = 0;
static uint32_t        s_rx_bad = 0;
static uint8_t         s_tx_seq = 0;

/* ------------------------------------------------------------------ */
/* 逐字节状态机                                                       */
/* ------------------------------------------------------------------ */

static void link_rx_byte(uint8_t b)
{
    switch (s_state) {
    case RX_WAIT_HEAD:
        if (b == PROTO_HEAD) {
            s_buf[0] = b;
            s_idx = 1;
            s_state = RX_COLLECT;
        }
        /* 非帧头字节直接丢, 天然重同步 */
        break;

    case RX_COLLECT:
        if (s_idx < CTRL_FRAME_SIZE) {
            s_buf[s_idx++] = b;
        }
        if (s_idx >= CTRL_FRAME_SIZE) {
            ctrl_payload_t pay;
            uint8_t seq;
            if (proto_decode_ctrl(s_buf, CTRL_FRAME_SIZE, &pay, &seq)) {
                s_pay     = pay;
                s_seq     = seq;
                s_rx_ms   = s_now_ms;
                s_pending = true;
                s_rx_ok++;
            } else {
                s_rx_bad++;
            }
            s_state = RX_WAIT_HEAD;
            s_idx   = 0;
        }
        break;

    default:
        s_state = RX_WAIT_HEAD;
        s_idx   = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

extern "C" void link_uart_init(void)
{
    /* Serial1 = USART1 (PA9=TX / PA10=RX), 波特率与桥接端一致 */
    Serial1.begin(LINK_BAUD, SERIAL_8N1);
    Serial1.setTimeout(0);
}

extern "C" void link_uart_tick_update(uint32_t now_ms)
{
    s_now_ms = now_ms;
}

/**
 * @brief 主循环每轮调一次, 把链路缓冲里的字节喂给状态机
 *        一次最多处理 LINK_POLL_MAX_BYTES 字节, 避免万一被灌爆时卡住控制环
 */
extern "C" void link_uart_poll(void)
{
    /* 最多处理 2 帧的量, 正常情况每轮只有 0~1 个字节 */
    for (uint16_t i = 0; i < 40u; i++) {
        int c = Serial1.read();
        if (c < 0) {
            break;
        }
        link_rx_byte((uint8_t)c);
    }
}

/* ------------------------------------------------------------------ */
/* 主循环侧                                                           */
/* ------------------------------------------------------------------ */

extern "C" bool link_uart_take_command(link_cmd_t *out)
{
    if (!s_pending) {
        return false;
    }

    out->payload = s_pay;
    out->seq     = s_seq;
    out->rx_ms   = s_rx_ms;
    s_pending    = false;
    return true;
}

extern "C" void link_uart_send_status(const status_frame_t *st)
{
    uint8_t buf[STATUS_FRAME_SIZE];
    proto_encode_status(buf, s_tx_seq++, st->rssi, st->state,
                        st->yaw_deg, st->pitch_deg, st->temp_mos, st->err_code);

    /* 17 字节 @921600 约 184us。write() 会拷进核心的发送缓冲并由 TX 中断
     * 慢慢发, 不会阻塞主循环, 50Hz 调用完全没压力。 */
    Serial1.write(buf, STATUS_FRAME_SIZE);
}

extern "C" uint32_t link_uart_rx_ok(void)  { return s_rx_ok; }
extern "C" uint32_t link_uart_rx_bad(void) { return s_rx_bad; }
