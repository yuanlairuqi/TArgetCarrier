/**
 * @file    can_drv.c
 * @brief   STM32F103 bxCAN 裸寄存器驱动实现 (多任务安全版)
 *
 *  与旧版的关键差别:
 *    1. can_drv_send() 内部用 taskENTER_CRITICAL()/taskEXIT_CRITICAL() 做一次
 *       "读 TSR 选空闲邮箱 -> 写 TIR/TDTR/TDLR/TDHR -> 置 TXRQ" 的短临界区,
 *       防止任务切换刚好落在"选好邮箱、还没写完"的窗口里。
 *    2. 记录每个邮箱的在途状态, 用 TSR 的 TXOK/ALST/TERR 区分"总线成功/失败",
 *       让上层能区分"已写入邮箱"和"总线上确实发出去了"。
 *    3. 增加 FreeRTOS 互斥量: 控制任务用 timeout=0 尝试, 拿不到就下一拍再发,
 *       绝不阻塞 1kHz 控制任务; CAN 恢复任务持锁做重新初始化。
 *    4. 所有中断都不调用发送函数。
 *
 *  ★ 引脚用 CAN1 重映射到 PB8(CRX) / PB9(CTX), 不用默认的 PA11/PA12
 *    (野火双 USB 板把 PA11/PA12 焊到 USB 座上了), 详见 config.h 顶部说明。
 */

#include "can_drv.h"
#include "config.h"

#include "stm32f1xx.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                           */
/* ------------------------------------------------------------------ */

static volatile uint32_t s_tx_submit_fail = 0;   /* 三邮箱全满导致的提交失败 */
static volatile uint32_t s_bus_ok = 0;
static volatile uint32_t s_bus_fail = 0;
static volatile bool     s_recovering = false;

static bool     s_mailbox_pending[3] = { false, false, false };

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock = NULL;

/* TSR 位定义 (RM0008 24.9.3)。前缀 CDRV_ 避免与 CMSIS 已有的
 * CAN_TSR_TME / CAN_TSR_TXOK 宏重名 (重名会产生 redefined 告警)。 */
#define CDRV_TSR_RQCP(mb)  (CAN_TSR_RQCP0 << (8u * (mb)))
#define CDRV_TSR_TXOK(mb)  (CAN_TSR_TXOK0 << (8u * (mb)))
#define CDRV_TSR_ALST(mb)  (CAN_TSR_ALST0 << (8u * (mb)))
#define CDRV_TSR_TERR(mb)  (CAN_TSR_TERR0 << (8u * (mb)))
#define CDRV_TSR_ABRQ(mb)  (1u << (7u + 8u * (mb)))
#define CDRV_TSR_TME(mb)   (1u << (26u + (mb)))

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

static void can_gpio_remap_init(void)
{
    /* 1. 开 GPIOB 和 AFIO 时钟 (改重映射必须有 AFIO 时钟) */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    /* 2. ★ CAN1 重映射到 PB8/PB9 (CAN_REMAP 编码 00/10/11, 没有 01) */
    uint32_t mapr = AFIO->MAPR;
    mapr &= ~(AFIO_MAPR_CAN_REMAP);
#if   CAN_REMAP == 2
    mapr |=  AFIO_MAPR_CAN_REMAP_REMAP3;       /* "11" -> PD0/PD1 */
#elif CAN_REMAP == 0
    /* "00" -> PA11/PA12, 不用置位 */
#else
    mapr |=  AFIO_MAPR_CAN_REMAP_REMAP2;       /* "10" -> PB8/PB9 (默认) */
#endif
    AFIO->MAPR = mapr;

#if CAN_REMAP == 2
    RCC->APB2ENR |= RCC_APB2ENR_IOPDEN;
    uint32_t crl_d = GPIOD->CRL;
    crl_d &= ~(0xFu << 0);
    crl_d |=  (0x8u << 0);          /* PD0 输入 */
    crl_d &= ~(0xFu << 4);
    crl_d |=  (0xBu << 4);          /* PD1 复用推挽 */
    GPIOD->CRL = crl_d;
    GPIOD->ODR |= (1u << 0);
#elif CAN_REMAP == 0
    uint32_t crh_a = GPIOA->CRH;
    crh_a &= ~(0xFu << 12);         /* PA11 */
    crh_a |=  (0x8u << 12);
    crh_a &= ~(0xFu << 16);         /* PA12 */
    crh_a |=  (0xBu << 16);
    GPIOA->CRH = crh_a;
    GPIOA->ODR |= (1u << 11);
#else
    uint32_t crh = GPIOB->CRH;
    crh &= ~(0xFu << 0);           /* PB8 = CAN_RX 上拉输入 */
    crh |=  (0x8u << 0);
    crh &= ~(0xFu << 4);           /* PB9 = CAN_TX 复用推挽 50MHz */
    crh |=  (0xBu << 4);
    GPIOB->CRH = crh;
    GPIOB->ODR |= (1u << 8);
#endif
}

static void can_filter_init(void)
{
    /* 只配银行 0, 32 位掩码模式, 掩码全 0 = 全通过, 挂 FIFO0。
     * (F1 的 CMSIS 头没有导出 F0R1/FiRx 成员, 按 RM0008 偏移换算) */
#define CAN_FILTER_BANK          0u
#define CAN_FILTER_BASE_OFFSET   0x240u
#define CAN_FILTER_STRIDE        8u

    volatile uint32_t *can_fir1 =
        (volatile uint32_t *)(((volatile uint8_t *)CAN1) + CAN_FILTER_BASE_OFFSET
                              + CAN_FILTER_STRIDE * CAN_FILTER_BANK);
    volatile uint32_t *can_fir2 = can_fir1 + 1;

    CAN1->FMR  |= CAN_FMR_FINIT;
    CAN1->FA1R &= ~CAN_FA1R_FACT0;

    *can_fir1 = 0x00000000u;
    *can_fir2 = 0x00000000u;
    CAN1->FS1R |= CAN_FS1R_FSC0;
    CAN1->FM1R &= ~CAN_FM1R_FBM0;
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0;

    CAN1->FA1R |= CAN_FA1R_FACT0;
    CAN1->FMR  &= ~CAN_FMR_FINIT;
}

static bool can_hw_init(void)
{
    /* 1. 进入初始化模式 (ABOM: bus-off 后允许硬件自动恢复) */
    CAN1->MCR |= CAN_MCR_INRQ | CAN_MCR_ABOM;
    uint32_t guard = 0xFFFFFu;
    while ((CAN1->MSR & CAN_MSR_INAK) == 0u) {
        if (--guard == 0u) {
            return false;
        }
    }

    /* 2. 波特率: 18tq @ 55.556ns = 1.000Mbps, 采样点 83.3% */
    CAN1->BTR = ((uint32_t)CAN_BTR_SJW_VAL << 24)
              | ((uint32_t)CAN_BTR_TS2_VAL << 20)
              | ((uint32_t)CAN_BTR_TS1_VAL << 16)
              | ((uint32_t)CAN_BTR_BRP_VAL);
#if CAN_SELFTEST_LOOPBACK
    CAN1->BTR |= (1u << 30);   /* LBKM=1 内部回环自测 */
#endif

    /* 3. 过滤器 */
    can_filter_init();

    /* 4. 退出初始化 -> 正常模式 */
    CAN1->MCR &= ~CAN_MCR_INRQ;
    guard = 0xFFFFFu;
    while (CAN1->MSR & CAN_MSR_INAK) {
        if (--guard == 0u) {
            return false;
        }
    }

    CAN1->MCR &= ~CAN_MCR_SLEEP;
    return true;
}

bool can_drv_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            return false;
        }
    }
    for (uint8_t i = 0; i < 3u; i++) {
        s_mailbox_pending[i] = false;
    }

    can_gpio_remap_init();

    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
    (void)RCC->APB1ENR;

    return can_hw_init();
}

/* ------------------------------------------------------------------ */
/* 互斥量                                                             */
/* ------------------------------------------------------------------ */

bool can_drv_lock(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0u) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lock, ticks) == pdTRUE;
}

void can_drv_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

void can_drv_set_recovering(bool on)
{
    s_recovering = on;
}

bool can_drv_is_recovering(void)
{
    return s_recovering;
}

/* ------------------------------------------------------------------ */
/* 发送                                                               */
/* ------------------------------------------------------------------ */

bool can_drv_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (data == NULL) {
        return false;
    }
    if (len > 8u) {
        len = 8u;
    }

    /* 短临界区: 选邮箱 + 写寄存器 + 置 TXRQ 必须原子, 否则两个任务
     * 可能选中同一个邮箱 (TME 还没反应) 并互相覆盖数据。 */
    taskENTER_CRITICAL();

    uint8_t mailbox = 0xFFu;
    for (uint8_t i = 0; i < 3u; i++) {
        if (!s_mailbox_pending[i] && (CAN1->TSR & CDRV_TSR_TME(i))) {
            mailbox = i;
            break;
        }
    }
    if (mailbox == 0xFFu) {
        taskEXIT_CRITICAL();
        s_tx_submit_fail++;
        return false;
    }

    CAN1->TSR = CDRV_TSR_RQCP(mailbox); /* Clear any completion from an explicitly aborted request. */
    CAN1->sTxMailBox[mailbox].TIR = ((id & 0x7FFu) << 21);

    uint32_t d0 = 0, d1 = 0;
    for (uint8_t i = 0; i < 4u; i++) {
        if (i < len) d0 |= ((uint32_t)data[i]) << (8u * i);
    }
    for (uint8_t i = 4u; i < 8u; i++) {
        if (i < len) d1 |= ((uint32_t)data[i]) << (8u * (i - 4u));
    }
    CAN1->sTxMailBox[mailbox].TDTR = (uint32_t)len;
    CAN1->sTxMailBox[mailbox].TDLR = d0;
    CAN1->sTxMailBox[mailbox].TDHR = d1;

    CAN1->sTxMailBox[mailbox].TIR |= CAN_TI0R_TXRQ;
    s_mailbox_pending[mailbox] = true;

    taskEXIT_CRITICAL();
    return true;
}

can_tx_done_t can_drv_poll_tx(void)
{
    can_tx_done_t done = { 0u, 0u, 0u };

    taskENTER_CRITICAL();
    for (uint8_t mb = 0; mb < 3u; mb++) {
        if (!s_mailbox_pending[mb]) {
            continue;
        }
        if ((CAN1->TSR & (CDRV_TSR_TME(mb) | CDRV_TSR_RQCP(mb))) !=
            (CDRV_TSR_TME(mb) | CDRV_TSR_RQCP(mb))) {
            continue;   /* 还在发 (或等待 ACK) */
        }

        bool ok = (CAN1->TSR & CDRV_TSR_TXOK(mb)) != 0u;
        /* RQCP is the write-one-to-clear completion bit; TXOK/ALST/TERR are status. */
        CAN1->TSR = CDRV_TSR_RQCP(mb);
        s_mailbox_pending[mb] = false;

        done.completed++;
        if (ok) {
            done.ok++;
        } else {
            done.failed++;
        }
    }
    taskEXIT_CRITICAL();

    s_bus_ok += done.ok;
    s_bus_fail += done.failed;
    return done;
}

void can_drv_abort_all(void)
{
    taskENTER_CRITICAL();
    CAN1->TSR = CDRV_TSR_ABRQ(0) | CDRV_TSR_ABRQ(1) | CDRV_TSR_ABRQ(2);
    for (uint8_t i = 0; i < 3u; i++) {
        s_mailbox_pending[i] = false;
    }
    taskEXIT_CRITICAL();
}

uint32_t can_drv_tx_fail_count(void)
{
    return s_tx_submit_fail;
}

uint32_t can_drv_bus_ok_count(void)
{
    return s_bus_ok;
}

uint32_t can_drv_bus_fail_count(void)
{
    return s_bus_fail;
}

/* ------------------------------------------------------------------ */
/* 接收                                                               */
/* ------------------------------------------------------------------ */

bool can_drv_receive(can_frame_t *out)
{
    if (out == NULL) {
        return false;
    }
    if ((CAN1->RF0R & CAN_RF0R_FMP0) == 0u) {
        return false;
    }

    uint32_t rir = CAN1->sFIFOMailBox[0].RIR;
    uint32_t rdtr = CAN1->sFIFOMailBox[0].RDTR;
    uint32_t rdlr = CAN1->sFIFOMailBox[0].RDLR;
    uint32_t rdhr = CAN1->sFIFOMailBox[0].RDHR;
    out->id  = (rir >> 21) & 0x7FFu;
    out->len = (uint8_t)(rdtr & 0xFu);
    if (out->len > 8u) {
        out->len = 8u;
    }
    for (uint8_t i = 0; i < 4u; i++) {
        out->data[i] = (uint8_t)(rdlr >> (8u * i));
    }
    for (uint8_t i = 4u; i < 8u; i++) {
        out->data[i] = (uint8_t)(rdhr >> (8u * (i - 4u)));
    }

    CAN1->RF0R |= CAN_RF0R_RFOM0;
    return true;
}

/* ------------------------------------------------------------------ */
/* 健康检查 / 恢复                                                    */
/* ------------------------------------------------------------------ */

bool can_drv_is_healthy(void)
{
    if (CAN1->MSR & CAN_MSR_INAK) {
        return false;
    }
    if ((CAN1->ESR & CAN_ESR_BOFF) != 0u) {
        return false;
    }
    return true;
}

bool can_drv_bus_off(void)
{
    return (CAN1->ESR & CAN_ESR_BOFF) != 0u;
}

bool can_drv_recover(void)
{
    /* 进入初始化模式 */
    CAN1->MCR |= CAN_MCR_INRQ;
    uint32_t guard = 0xFFFFFu;
    while ((CAN1->MSR & CAN_MSR_INAK) == 0u) {
        if (--guard == 0u) {
            return false;
        }
    }

    /* 中止所有挂起的发送请求 (否则旧的错误帧会一直被硬件重发) */
    can_drv_abort_all();

    /* 时序保持原样 */
    CAN1->BTR = ((uint32_t)CAN_BTR_SJW_VAL << 24)
              | ((uint32_t)CAN_BTR_TS2_VAL << 20)
              | ((uint32_t)CAN_BTR_TS1_VAL << 16)
              | ((uint32_t)CAN_BTR_BRP_VAL);

    CAN1->MCR &= ~CAN_MCR_INRQ;
    guard = 0xFFFFFu;
    while (CAN1->MSR & CAN_MSR_INAK) {
        if (--guard == 0u) {
            return false;
        }
    }
    return true;
}
