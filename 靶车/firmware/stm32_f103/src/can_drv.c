/**
 * @file    can_drv.c
 * @brief   STM32F103 bxCAN 裸寄存器驱动实现
 *
 *  ★ 引脚用 CAN1 重映射到 PB8(CRX) / PB9(CTX), 不用默认的 PA11/PA12。
 *
 *  为什么必须重映射:
 *    野火 STM32F103C8T6「双 USB 款」核心板把 PA11/PA12 直接接到了 USB
 *    TYPE-C 插座上 (USB_DM = PA11, USB_DP = PA12, 见规格书 2.4.7 节
 *    "USB Device 口...连接了主芯片的 USB 外设脚")。而 PA11/PA12 恰好
 *    是 CAN1 的默认引脚, 于是板上这两个脚已经被 USB 占用, 接 CAN 收发器
 *    根本驱动不了。
 *    STM32F103 支持把 CAN1 重映射到 PB8/PB9, 这两根脚在板上排针是引出的,
 *    所以改用重映射方案。
 *
 *  寄存器路径 (ST 参考手册 RM0008):
 *    1. 开时钟: RCC_APB1ENR.CAN1EN + RCC_APB2ENR.IOPBEN + AFIOEN
 *    2. ★ AFIO_MAPR 置 CAN_REMAP = "10" (即 bit14=0, bit13=1) -> PB8/PB9
 *    3. PB9 = 复用推挽输出 (CAN_TX), PB8 = 上拉输入 (CAN_RX)
 *    4. CAN_MCR: 置 INRQ 请求进入初始化模式, 等 INAK 置位
 *    5. CAN_BTR: 写波特率分频 (只有初始化模式下能写)
 *    6. CAN_MCR: 清 INRQ 退出初始化, 等 INAK 清零 -> 进入正常模式
 *    7. CAN_FMR/CAN_FiR1/CAN_FiR2/CAN_FA1R: 配一个全通过的过滤器
 *
 *  ★ 顺序很重要: 必须先写 AFIO 重映射, 再配 GPIO 模式, 最后开 CAN 时钟。
 *    AFIO_MAPR 只能在 CAN1 处于复位状态时改 (本函数在初始化最开始做, 满足)。
 */

#include "can_drv.h"
#include "config.h"

#include "stm32f1xx.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                           */
/* ------------------------------------------------------------------ */

static volatile uint32_t s_tx_fail = 0;

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

static void can_gpio_remap_init(void)
{
    /* 1. 开 GPIOB 和 AFIO 时钟 (改重映射必须有 AFIO 时钟) */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;

    /* 2. ★ CAN1 重映射到 PB8/PB9
     *    AFIO_MAPR 的 CAN_REMAP[1:0] 在 bit14:13:
     *      00 = 无重映射    -> PA11 = CAN_RX, PA12 = CAN_TX (本板被 USB 占用)
     *      10 = 重映射      -> PB8  = CAN_RX, PB9  = CAN_TX  ← 用这个
     *      11 = 重映射      -> PD0  = CAN_RX, PD1  = CAN_TX
     *    注意值是 "10" 而不是 "01", 这是 ST 手册里的一个坑:
     *      CAN_REMAP 的编码顺序是 00/10/11, 没有 01 这个组合。
     *    所以 bit14 清 0, bit13 置 1。 */
    uint32_t mapr = AFIO->MAPR;
    mapr &= ~(AFIO_MAPR_CAN_REMAP);            /* 先清掉两位 */
    mapr |=  AFIO_MAPR_CAN_REMAP_REMAP2;       /* "10" -> PB8/PB9 */
    AFIO->MAPR = mapr;

    /* 3. 配置 PB8 / PB9
     *    CRL 管 PB0..PB7, CRH 管 PB8..PB15。
     *    PB8 (CAN_RX): CRH 的 bit3:0  -> CNF=10 上拉/下拉输入, MODE=00 -> 0x8
     *    PB9 (CAN_TX): CRH 的 bit7:4  -> CNF=10 复用推挽,     MODE=11 -> 0xB */
    uint32_t crh = GPIOB->CRH;
    crh &= ~(0xFu << 0);           /* 清 PB8  */
    crh |=  (0x8u << 0);           /* PB8  输入, 带上下拉 */
    crh &= ~(0xFu << 4);           /* 清 PB9  */
    crh |=  (0xBu << 4);           /* PB9  复用推挽 50MHz */
    GPIOB->CRH = crh;

    GPIOB->ODR |= (1u << 8);       /* PB8 上拉 (配合 CNF=10 + ODR=1) */
}

bool can_drv_init(void)
{
    /* 先把引脚重映射到 PB8/PB9 —— 必须在开 CAN 时钟之前做 */
    can_gpio_remap_init();

    /* ---- 1. 进入初始化模式 ---- */
    RCC->APB1ENR |= RCC_APB1ENR_CAN1EN;
    (void)RCC->APB1ENR;            /* 读一次保证时钟寄存器写入生效 */

    CAN1->MCR |= CAN_MCR_INRQ;
    /* 等硬件确认, 加超时防止时钟没开死等 */
    uint32_t guard = 0xFFFFFu;
    while ((CAN1->MSR & CAN_MSR_INAK) == 0u) {
        if (--guard == 0u) {
            return false;
        }
    }

    /* ---- 2. 波特率: 见 config.h 的推导, 18tq @ 55.556ns = 1.000Mbps ---- */
    CAN1->BTR = ((uint32_t)CAN_BTR_SJW_VAL << 24)
              | ((uint32_t)CAN_BTR_TS2_VAL << 20)
              | ((uint32_t)CAN_BTR_TS1_VAL << 16)
              | ((uint32_t)CAN_BTR_BRP_VAL);
    /* BTR 的 bit31 SILM / bit30 LBKM 保持 0 => 正常模式, 不环回 */

    /* ---- 3. 过滤器: 只配银行 0, 32 位掩码模式, 掩码全 0 = 全通过, 挂 FIFO0 ---- */
    /* ★ 坑: STM32F1 的 CMSIS 头文件里 CAN_TypeDef 没有导出 F0R1/F0R2/FiRx 这些
     *   过滤器寄存器成员 (H7 的 FDCAN 才有类似命名), 直接写 CAN1->F0R1 编不过。
     *   这里按 RM0008 的寄存器偏移换算, 与 ST HAL 内部的做法一致:
     *     CAN_FiR1(银行 i) = CAN_BASE + 0x240 + 8*i
     *     CAN_FiR2(银行 i) = CAN_FiR1 + 4 */
#define CAN_FILTER_BANK   0u
#define CAN_FILTER_BASE_OFFSET   0x240u
#define CAN_FILTER_STRIDE        8u

    volatile uint32_t *can_fir1 =
        (volatile uint32_t *)(((volatile uint8_t *)CAN1) + CAN_FILTER_BASE_OFFSET
                              + CAN_FILTER_STRIDE * CAN_FILTER_BANK);
    volatile uint32_t *can_fir2 = can_fir1 + 1;

    CAN1->FMR  |= CAN_FMR_FINIT;         /* 进入过滤器初始化模式 */
    CAN1->FA1R &= ~CAN_FA1R_FACT0;       /* 先停用银行 0 */

    *can_fir1 = 0x00000000u;             /* 期望 ID (全 0 = 不关心) */
    *can_fir2 = 0x00000000u;             /* 掩码    (全 0 = 全部通过) */
    CAN1->FS1R |= CAN_FS1R_FSC0;         /* 32 位尺度 */
    CAN1->FM1R &= ~CAN_FM1R_FBM0;        /* 掩码模式 (非列表模式) */
    CAN1->FFA1R &= ~CAN_FFA1R_FFA0;      /* 关联 FIFO0 */

    CAN1->FA1R |= CAN_FA1R_FACT0;        /* 启用银行 0 */
    CAN1->FMR  &= ~CAN_FMR_FINIT;        /* 退出过滤器初始化 */

    /* ---- 4. 退出初始化模式 -> 正常模式 ---- */
    CAN1->MCR &= ~CAN_MCR_INRQ;
    guard = 0xFFFFFu;
    while (CAN1->MSR & CAN_MSR_INAK) {
        if (--guard == 0u) {
            return false;
        }
    }

    /* 退出睡眠 */
    CAN1->MCR &= ~CAN_MCR_SLEEP;

    return true;
}

/* ------------------------------------------------------------------ */
/* 发送                                                               */
/* ------------------------------------------------------------------ */

bool can_drv_send(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (len > 8u) {
        len = 8u;
    }

    /* 找空闲邮箱: TSR 的 TME0/1/2 对应邮箱 0/1/2 */
    uint8_t mailbox = 0xFFu;
    if (CAN1->TSR & CAN_TSR_TME0) {
        mailbox = 0u;
    } else if (CAN1->TSR & CAN_TSR_TME1) {
        mailbox = 1u;
    } else if (CAN1->TSR & CAN_TSR_TME2) {
        mailbox = 2u;
    }

    if (mailbox == 0xFFu) {
        /* 三个邮箱都占着 = 总线卡住或对端没 ACK */
        s_tx_fail++;
        return false;
    }

    /* 标准帧: ID 放 TIR 的 [31:21], 数据长度放 [3:0] */
    CAN1->sTxMailBox[mailbox].TIR = ((id & 0x7FFu) << 21);

    /* 数据按小端塞进两个 32 位寄存器 */
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

    /* 请求发送 */
    CAN1->sTxMailBox[mailbox].TIR |= CAN_TI0R_TXRQ;

    return true;
}

uint32_t can_drv_tx_fail_count(void)
{
    return s_tx_fail;
}

/* ------------------------------------------------------------------ */
/* 接收                                                               */
/* ------------------------------------------------------------------ */

bool can_drv_receive(can_frame_t *out)
{
    /* RF0R 的 FMP0[1:0] 是 FIFO0 里的报文数 */
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

    /* 释放邮箱: 写 RFOM0 = 1 */
    CAN1->RF0R |= CAN_RF0R_RFOM0;

    return true;
}

bool can_drv_is_healthy(void)
{
    /* INAK 不该在正常模式下置位; 置位说明总线进了异常状态 */
    if (CAN1->MSR & CAN_MSR_INAK) {
        return false;
    }
    return true;
}
