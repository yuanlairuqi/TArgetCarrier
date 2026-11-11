/**
 * @file    config.h
 * @brief   靶车 —— STM32F103C8T6 控制器 编译期配置
 *
 *  职责:
 *    USART1 收桥接端转发的控制帧 -> 校验 -> 1kHz 闭环 -> CAN 1Mbps -> DM-G6220
 *    并把电机反馈打包成反馈帧从 USART1 发回桥接
 *
 *  接线 (野火 STM32F103C8T6 核心板):
 *    PA9  (USART1_TX) -> M5Stamp G2 (RX)
 *    PA10 (USART1_RX) <- M5Stamp G1 (TX)
 *    GND              <-> M5Stamp GND        ★ 必须共地
 *    PB9  (CAN1_TX, 重映射) -> SN65HVD230 CTX
 *    PB8  (CAN1_RX, 重映射) <- SN65HVD230 CRX
 *    SN65HVD230 VCC   -> 3V3 (模块是 3.3V 供电)
 *    SN65HVD230 CANH/CANL -> 电机 CANH/CANL  (总线两端各 120Ω 终端电阻)
 *
 *  ★ 为什么 CAN 用 PB8/PB9 而不是默认的 PA11/PA12:
 *    野火这块「双 USB 款」核心板把 PA11/PA12 直接焊到了 USB TYPE-C 插座上
 *    (USB_DM = PA11, USB_DP = PA12, 见规格书 2.4.7 节)。而 PA11/PA12 正好是
 *    CAN1 的默认引脚 —— 板上这两个脚已经被 USB 占死, 接 CAN 收发器驱动不了。
 *    所以本工程在 can_drv.c 里把 AFIO_MAPR 的 CAN_REMAP 设成 "10",
 *    把 CAN1 重映射到 PB8(CRX) / PB9(CTX)。这两根脚在板上排针是引出的。
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ================================================================== */
/* USART1 与桥接端通信                                                 */
/* ================================================================== */
/* 必须与 firmware/esp32_bridge/include/app_config.h 的 LINK_BAUD 一致。
 * 921600 已按 72MHz PCLK2 核算过误差: USARTDIV=4.8828 -> 实际 921600, 误差 0%。
 * 如果现场线比较长偶尔出错, 两端一起改成 460800 (USARTDIV=9.7656, 误差 0%) 更稳。 */
#define LINK_BAUD                 921600u

/* 主机失联判定: 超过该毫秒数没收到有效控制帧 -> 撤销 ARM 授权、立即失能。
 * ★ 统一口径: 主机 SAFE / 桥接 FAILSAFE / STM32 自身超时 / 文档都必须是 300ms。
 *   旧版这里还有一个 CTRL_HOLD_MS=1000 的"保持使能宽限期", 实际行为与文档写的
 *   300ms 不一致; 本轮已删除宽限, 失联即失能 (运动授权是独立的 ARM 令牌, 重新
 *   运动必须由用户重新按使能)。 */
#define CTRL_TIMEOUT_MS           300u

/* 反馈帧发送周期 */
#define STATUS_PERIOD_MS          20u     /* 50Hz */

/* ================================================================== */
/* CAN (bxCAN)                                                        */
/* ================================================================== */
/* BTR 寄存器按 RM0008 的位定义拆开写 (不用 CAN_BS1_15TQ 之类的 HAL 宏,
 * 那些宏在 F1 的 CMSIS 头文件里并不存在):
 *
 *   BRP[9:0]   = 预分频 - 1
 *   TS1[19:16] = 时间段1 - 1
 *   TS2[22:20] = 时间段2 - 1
 *   SJW[25:24] = 重同步跳宽 - 1
 *
 *   ★ 波特率推导 (APB1 = 36MHz):
 *     预分频 = 2  ->  tq = 1 / (36MHz / 2) = 55.556ns
 *     位时间 = 1(SYNC) + (TS1+1) + (TS2+1) tq
 *     要 1.000Mbps -> 位时间必须正好 1.000us -> 1000ns / 55.556ns = 18tq
 *     1 + (TS1+1) + (TS2+1) = 18  ->  TS1 + TS2 = 15  ->  TS1 = 13, TS2 = 2
 *     (采样点 = (1 + 14) / 18 = 83.3%, 在 CiA 推荐的 75~87.5% 区间内)
 *
 *   ★★ 这里踩过两次坑, 记下来:
 *      1) 达妙官方 F4 例程用 BS1=15TQ / BS2=5TQ, 那是 APB1=42MHz 配的 1Mbps。
 *         照搬到 F103 (36MHz) 会算出 21tq = 857kbps, 总线完全不通。
 *      2) 后来改成 TS1=15 / TS2=2, 算成 1+16+3 = 20tq = 900kbps, 看着像 1M
 *         其实不是 —— 电机听不出来就不应答, 现象是 s_can_fail 一直涨、
 *         但万用表量 CAN_H/CAN_L 完全正常(60Ω), 极容易误判成接线问题。
 *         正确值 TS1=13 / TS2=2, 总 18tq, 正好 1.000Mbps。
 */
/* ★ CAN 引脚选择 (必须在 can_drv.c 的 #if 之前定义, 否则会被当成 0!) */
#define CAN_REMAP                 1

/* 诊断开关: 1 = CAN 内部回环自测 (不经过收发器, 用来区分 MCU 问题和外部接线问题) */
#define CAN_SELFTEST_LOOPBACK     0     /* 1 = PB8(RX)/PB9(TX), 2 = PD0/PD1, 0 = PA11/PA12 */

#define CAN_BTR_BRP_VAL           1u    /* 预分频 2  -> tq = 55.556ns */
#define CAN_BTR_TS1_VAL           13u   /* 时间段1 = 14tq */
#define CAN_BTR_TS2_VAL           2u    /* 时间段2 = 3tq  */
#define CAN_BTR_SJW_VAL           0u    /* SJW = 1tq     */

/* CAN 发送失败多少次后判定总线故障 (掉线/没接终端电阻) */
/* 连续失败达到这个数就重新初始化 CAN (自愈); 间隔限制避免抖动 */
#define CAN_RECOVER_THRESHOLD     64u
#define CAN_RECOVER_PERIOD_MS     2000u

/* ================================================================== */
/* 电机参数 —— 达妙 DM-G6220 中空云台电机                              */
/* ================================================================== */
/* 出厂默认 CAN ID = 1。用达妙上位机 DMTool 改过 ID 的话改这里。 */
#define MOTOR_CAN_ID              1u
#define MOTOR_MASTER_ID           0x00u   /* 主控 ID, 用于读寄存器命令 */

/* MIT 协议量程: 必须与电机固件里的配置一致, 否则位置会整体偏移/放缩错。
 * DM-G6220 说明书给出的典型值: 位置 ±12.5rad, 速度 ±45rad/s, 力矩 ±10Nm。
 * ★ 如果你的电机用 DMTool 改过量程, 这里必须同步改, 否则会飞车。 */
#define MOTOR_PMAX                12.5f
/* VMAX 必须和电机内部参数一致 (达妙工具"速度 MAX"= 30 rad/s), 否则 MIT 速度字段解码比例错 1.5 倍 */
#define MOTOR_VMAX                30.0f
#define MOTOR_TMAX                10.0f

/* MIT 增益量程 (达妙固件固定值, 不要改) */
#define MOTOR_KP_MIN              0.0f
#define MOTOR_KP_MAX              500.0f
#define MOTOR_KD_MIN              0.0f
#define MOTOR_KD_MAX              5.0f

/* 默认闭环增益: 云台大惯量负载, kp 给到 40~80 比较跟手, kd 1~3 抑制过冲 */
#define MOTOR_KP_DEFAULT          60.0f
#define MOTOR_KD_DEFAULT          1.5f
/* 力矩前馈默认 0 (纯位置阻抗控制) */
#define MOTOR_TORQUE_FF           0.0f

/* ================================================================== */
/* 机械限位 (度) —— 防止上位机给了离谱角度把云台撞到机械限位           */
/* ================================================================== */
#define GIMBAL_YAW_MIN_DEG        (-180.0f)
#define GIMBAL_YAW_MAX_DEG        ( 180.0f)
#define GIMBAL_PITCH_MIN_DEG      ( -30.0f)
#define GIMBAL_PITCH_MAX_DEG      (  60.0f)

/* ================================================================== */
/* 调试                                                               */
/* ================================================================== */
/* ★ PA9/PA10 被 USART1 (接桥接端) 占用, 本芯片没有第二路可用串口,
 *   所以 STM32 端不提供串口打印。要看运行状态有两个办法:
 *     1. 看桥接端 M5Stamp 的串口日志 (已经带 STM32 回传的角度/温度/错误码)
 *     2. VSCode + cortex-debug + DAPLink 在线调试 (SWD: DIO->PA13, CLK->PA14), 直接看变量
 *   (曾经这里有个 STM32_ENABLE_USB_DEBUG 宏, 但 Serial 在 F103 上就是
 *    USART1, 与桥接链路冲突, 留着只会误导, 已删除。) */

#endif /* CONFIG_H */
