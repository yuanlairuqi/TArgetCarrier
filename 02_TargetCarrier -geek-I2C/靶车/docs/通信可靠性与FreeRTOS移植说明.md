# 通信可靠性与 STM32 FreeRTOS 移植说明

> 版本：协议 v1.1（`PROTO_VERSION_MINOR = 1`） · 日期：2026-10-24
> 唯一设计依据：[`verification/freertos_port_20261024/DESIGN_CONTRACT.md`](../../../verification/freertos_port_20261024/DESIGN_CONTRACT.md)
> 本文所有时限、字段、任务表与代码常量必须一致；不一致时以契约为准并修文档/代码。
>
> **一句话结论**：本轮把"断联后自动恢复运动"这个根因从协议层堵死——新增运动授权
> ARM/令牌、桥接独立 FAILSAFE、STM32 用 FreeRTOS 把 1 kHz 控制与安全命令隔离，
> 并把"提交 / 回调 / 应用确认"三种状态拆开统计；网页只显示真实新鲜度，不再把
> 缓存当实时。

---

## 一、已确认问题及源码位置

> 下表行号来自**本轮修改前的基线快照**
> `verification/freertos_port_20261024/baseline_20261024_140557/`（下称"基线"），
> 路径均相对项目根 `靶车/靶车/`。修复后的代码不再保留这些写法，行号仅用于追溯问题。

| # | 已确认问题 | 基线源码位置 | 后果 |
|---|---|---|---|
| P1 | 把 ESP-NOW「发送提交」当成「送达」：`esp_now_send` 返回 OK 且**上一帧**回调成功就算本帧成功 | `firmware/esp32_master/src/main.cpp:101`（`g_last_send_ok`）、`:242`（回调写全局）、`:1090`（用它判当前帧） | 统计永久错位一帧，不同报文（控制/增益/参数/上行）互相串扰；界面/日志会显示"全成功"但对方没收到 |
| P2 | 桥接把**缓存**的 STM32 状态每 100 ms 无条件重发，主机收到就刷新"STM32 最后在线时间" | `firmware/esp32_bridge/src/main.cpp:365-390`（取 `g_last_status` 重编码）、`firmware/esp32_master/src/main.cpp:228`（收到即刷 `s_stm_last_ms`）、`firmware/esp32_master/web/index.html:575`（`a<2` 显示"实时"） | STM32 掉线后界面仍显示实时数据，操作者会按"能动的"云台做决策 |
| P3 | 桥接的 ESP-NOW 接收回调运行在 Wi-Fi 任务上下文，却使用 ISR 专用 API | `firmware/esp32_bridge/src/main.cpp:145/149、166/170、187/192`（`xQueueSendFromISR` + `portYIELD_FROM_ISR`） | 非 ISR 上下文误用 ISR API，队列满/调度时机不确定，属潜在崩溃点 |
| P4 | 安全命令（急停/失能/解除急停）走普通 WS 命令队列，队列满时静默丢弃 | `firmware/esp32_master/src/main.cpp:850`（`xQueueSend(s_ws_cmd_q, item, 0)`） | 最需要送达的命令反而可能丢，违反"停止/急停不得静默丢失" |
| P5 | 协议只有 ENABLE 持续电平，没有一次性授权/令牌：断联、急停、故障、复位后只要主机还在发 ENABLE 就会重新出力 | 基线 `firmware/lib/protocol/protocol.h` 无 `CTRL_FLAG_ARM`/令牌；`firmware/esp32_master/src/main.cpp:589-610`（`E` 只置 ENABLE）；STM32 无授权状态机 | **核心安全问题**：链路恢复 ≠ 用户重新授权，云台可能自己动起来 |
| P6 | STM32 失联后仍保持使能 1000 ms（宽限期），与文档"300 ms 断使能"矛盾 | `firmware/stm32_f103/include/config.h:38-39`（`CTRL_TIMEOUT_MS 300`、`CTRL_HOLD_MS 1000`）；`firmware/stm32_f103/src/app_main.c:119-124`（`link_hold`） | 现场断线后云台继续出力约 1 秒，且文档与代码不一致本身就是安全隐患 |
| P7 | 网页状态映射与 protocol 枚举不符：`0` 显示"失联"、`2` 显示"急停"、`3` 显示"故障"（4 无映射） | 基线 `firmware/esp32_master/web/index.html:567-570` | 界面语义与固件状态机错位，正好把 TIMEOUT/ESTOP/FAULT 读反 |
| P8 | 网页"丢包率"用未确认的提交计数（`ok/fail` 增量）计算 | 基线 `firmware/esp32_master/web/index.html:577-585`（`lossPct` 用 `txHist` 的 `ok/fail`） | 交付给 WiFi 层的帧被算成成功，丢包率偏乐观 |
| P9 | 主机 JSON 缺新鲜度/安全/授权/诊断字段；`st_age` 基于缓存时间，无 `st_fresh` | `firmware/esp32_master/src/main.cpp:789`（JSON 字段）、`:806`（`st_age` 计算） | 网页无法区分"新鲜数据 / 过期缓存"，也无法展示授权状态 |
| P10 | 桥接没有 STM32 反馈新鲜度跟踪，也没有 0x07 诊断帧 | 基线 `firmware/esp32_bridge/src/main.cpp`（只有 `g_have_status`/`g_last_status` 缓存重发，见 `:365-390`） | 主机/OLED 只能看到"最后一帧"，看不到"这帧多久了" |
| P11 | 单播/广播回退判据用魔法数（3000 ms / 40 帧） | `firmware/esp32_master/src/main.cpp:1102-1105` | 判据与真实链路状态无关，无法解释为什么"突然回退" |
| P12 | STM32 未使用 RTOS：单循环里同时做 USART/CAN/控制/显示，安全命令没有独立优先级 | 基线 `firmware/stm32_f103/src/**`（单循环 + 裸机看门狗） | 1 kHz 控制、20/50 Hz 发送、安全命令无法隔离保证；本轮移植 FreeRTOS 拆 5 任务 |

---

## 二、修改文件清单、修复逻辑与协议兼容性

### 2.1 三端修改文件清单

| 文件/目录 | 改动 | 归属 |
|---|---|---|
| `firmware/lib/protocol/protocol.h` | 协议 v1.1：控制帧 ARM 位/8 bit 令牌、`CTRL_FLAG_FAILSAFE`、`PROTO_MSG_BRIDGE_STATUS(0x07)` 编解码、电机帧 p[12..14] 安全位、`PROTO_VERSION_MINOR=1` | 三端共用 |
| `firmware/lib/linkcore/`（`include/linkcore.h` + `lc_*.c` + `test/`） | 纯 C 可靠性内核：8 bit 序号窗口、命令过期、ARM 授权状态机、反馈新鲜度、提交/回调/确认统计、CAN 邮箱协调、命令队列、字节流成帧 | 三端共用（已实现并有主机端测试） |
| `firmware/esp32_master/src/main.cpp` | 安全命令旁路（`s_safety_cmd`/`s_safety_seq`，`loop` 优先处理、清队列）；`g_safe`/`g_safe_reason`；`lc_arm_tx` 显式授权；`lc_txstats` 三组统计；`g_stm_fresh`/`g_bridge_fresh`；JSON 新增字段；`x`/`C`/`R` 语义修正；HTTP 拒绝 `J`/`D`/`b` | 主机 |
| `firmware/esp32_master/src/ws_server.cpp` | ws 任务栈 4096 → **5120**；`WS_TX_BUF` 512 → **1024**；`json[1024]`（容纳新增字段且 < 1000 B） | 主机 |
| `firmware/esp32_master/include/app_config.h` | 集中保护时限与队列长度常量（见第三节） | 主机 |
| `firmware/esp32_master/web/index.html` | 状态枚举修正、`st_fresh` 叠加、safe/arm 授权区、提交/回调/确认诊断、回调口径丢包率；保留轨迹/速度/巡航/手动/参数/预设/曲线/WS+HTTP 回退 | 主机（网页） |
| `firmware/esp32_bridge/src/main.cpp` | 接收回调只做长度/CRC 预校验 + `xQueueSend(...,0)`（删除 ISR API）；`safety_q`(4)/`cmd_q`(16) 双队列；`lc_seq_gate`/`lc_age_exceeds`；CTRL 帧原样转发；FAILSAFE 状态机（300 ms 进入、50 ms 注入）；STM32 新鲜度（`lc_fresh_t`）+ 只转发新鲜 STATUS + 100 ms 0x07 帧；`lc_txstats` 上行统计；初始化降级；删除死常量/失真注释 | 桥接 |
| `firmware/esp32_bridge/include/app_config.h`、`include/bridge_status.h`、`src/display_esp32.cpp` | 时限常量；`bridge_status_t` 增加 `stm32_age_ms/stm32_fresh/motion_blocked/overflows`；OLED 显示 FAILSAFE/STM32 年龄 | 桥接 |
| `firmware/stm32_f103/**` + `third_party/FreeRTOS-Kernel`（只读复制 V11.2.0） | FreeRTOS 移植：5 任务、TIM4 1 kHz 节拍、`lc_arm_t` 授权、`lc_can_tx_t` 安全槽、`lc_cmdq_t` 命令队列、DMA 成帧、栈溢出/内存失败钩子、CAN 恢复 | STM32（Lead） |
| `firmware/esp32_master/test/ws_proto_test.c`、`firmware/lib/protocol/test/test_protocol.c`、`firmware/lib/linkcore/test/test_linkcore.c` | 主机端自测覆盖新增帧/新 flag/新故障路径 | 主机端测试 |
| `tools/test_protocol_v11.py`（新增） | Python 标准库**独立实现**：CRC16、控制帧+ARM 令牌、0x07 帧、电机扩展帧 p12..14、序号窗口、命令过期模型；与 `protocol.h` 只读交叉核对 | 测试 |
| `tools/run_reliability_tests.ps1`（新增） | 一键跑 C 端 `test_protocol`/`test_ws_proto`/`test_linkcore`（cmake+ctest，若可用）+ Python 对拍；缺 cmake 时清晰提示并继续 Python | 测试 |
| `docs/通信可靠性与FreeRTOS移植说明.md`（本文，新增）、`docs/网页控制.md`、`README.md` | 行为/时限/授权语义文档同步 | 文档 |

> 本轮**不修改**业务功能：串口控制台、网页控制、6 种轨迹、速度/巡航/手动、参数读写与
> NVS、状态回传、OLED/LCD、急停、清错、存零点全部保留。

### 2.2 修复逻辑（按端）

- **主机（ESP32）**
  1. 安全命令旁路：WS/HTTP/串口识别到 `x`/`C`/`R` 时**原子写** `s_safety_cmd`+`s_safety_seq`，`loop` 每轮最先处理并 `xQueueReset` 普通命令队列；即使队列满也不丢。
  2. 运动授权：只有用户明确点"使能"（`E`）才 `lc_arm_tx_next()` 生成新令牌，持续发 `ENABLE|ARM|token`；`x`/`C`/`safe` 时 `lc_arm_tx_clear()`，此后发 `ENABLE=0,ARM=0,token=0`。
  3. 安全状态机：桥接帧或 STM32 状态 >300 ms、桥接报 `FAILSAFE|MOTION_BLOCKED`、ESP-NOW 在途连续超时 → `g_safe=1` 并记录 `safe_reason`；动作是撤销 ENABLE/ARM、清命令队列、停巡航/速度模式；链路恢复只清链路原因，**仍需用户重新按 `E`**。
  4. 统计分离：`lc_txstats_t` 分别记录 submit / cb / confirm；`json` 输出 `tx_sub_ok/tx_sub_fail/tx_cb_ok/tx_cb_fail/tx_confirm/cb_timeout`；**禁止把提交当成功**。
  5. 新鲜度：`lc_fresh_t g_stm_fresh`/`g_bridge_fresh`；收到 `0x02`/`0x07` 才 kick；`st_age` 用 `lc_fresh_age()`，输出 `st_fresh`/`st_online`/`br_age`/`br_flags`；删除"仅凭缓存 STATUS 刷新 STM32 时间"。
  6. 重启类 `J`/`D`/`b` 只允许串口触发，HTTP `/api/cmd` 拒绝（避免网页把控制循环阻塞数秒）。

- **桥接（M5Stamp S3）**
  1. ESP-NOW 接收回调（Wi-Fi 任务上下文）只做长度/CRC 预校验、拷贝、`xQueueSend(...,0)`；**不再使用** `xQueueSendFromISR`/`portYIELD_FROM_ISR`。
  2. 按类型分 `safety_q`(4)/`s_cmd_q`(16) 两条队列，任务每轮先清 safety 再处理 cmd；安全帧满时丢最旧。
  3. CTRL 帧**原样转发 17 字节**（不重编码、不改 seq）；只对 CTRL 做序号过滤；出队按 100 ms 过期丢弃并计数。
  4. FAILSAFE：主机静默 >300 ms 进入并锁存、清两条队列，每 50 ms 注入 `flags=CTRL_FLAG_FAILSAFE`（无 ENABLE/ARM/ESTOP，seq 自增）的失能帧；FAILSAFE 期间不转发 ENABLE 并计 `arm_reject`；只有"链路不 stale 且收到过主机 `ENABLE=0`"才退出，退出后仍要主机新令牌。
  5. STM32 新鲜度：`lc_fresh_t g_stm_fb`；只转发 ≤300 ms 的 STATUS，**不重发缓存**；每 100 ms 发 0x07 帧（STM32 年龄/状态/计数）。
  6. 上行 `lc_txstats`：在途未回调前不提交下一帧，250 ms 超时放行并计数；`esp_now_add_peer` 失败限频重试，成功才置 `UPLINK_READY`。
  7. 队列/任务创建失败置 `s_bridge_degraded`，停发 ENABLE、只注入失能帧，OLED/日志明确显示。

- **STM32（F103）**
  1. FreeRTOS-Kernel V11.2.0，5 任务（见第四节），TIM2 中断只 `xTaskNotifyFromISR`+计数，不做控制计算。
  2. ARM 授权：`lc_arm_t` 是权威判定，任何急停/故障/断联/复位/CAN 恢复/反馈超时都 `lc_arm_force_safe()`；旧令牌永远不能重放。
  3. 控制/发送分离：`ctrl` 任务 1 kHz 做看门狗/ARM/CAN 协调/增益斜坡/MIT 打包；`status` 任务是唯一 UART 发送者（50 Hz 状态 / 20 Hz 电机 / 参数回包）。
  4. CAN：`lc_can_tx_t` 安全槽（失能 > 使能 > 清错 > 存零点），失能压过使能、停止清运动、恢复保留安全帧；连续失败/BOFF 达阈值请求恢复（64 次/2 s 最小间隔）。
  5. USART1 用 DMA 循环缓冲 512 B + `lc_rx_t` 成帧，溢出/滞后重同步计数；出队命令 100 ms 过期丢弃。
  6. 上电默认未授权、电机失能，不保存任何运动状态；栈溢出/内存失败钩子进安全态并打印。

### 2.3 协议兼容性（**三端必须同时升级**）

- **帧长全部保持 17 B**，旧解析器的长度/CRC 覆盖范围不变；只新增"未使用位/新类型/新载荷"。
- 新类型 `PROTO_MSG_BRIDGE_STATUS = 0x07`：旧主机 `proto_decode_*` 遇到未知类型返回失败并忽略，**不会误解成 0x02 状态帧**，因此可以共存（但旧主机看不到桥接新鲜度）。
- 新 flags 高位：`bit4 ARM`、`bit15:8 ARM_TOKEN`、`bit5 FAILSAFE`。旧固件"读到不认识的位"通常只按位掩码使用自己关心的位，不会崩溃；但——
  - **旧 STM32 不认识 ARM/令牌**：如果只升级主机和桥接、不升级 STM32，STM32 仍会把 ENABLE 当成持续使能，**断联恢复自动运动的漏洞依然存在**。因此**禁止只升级一端**。
  - `CTRL_FLAG_FAILSAFE = 0x0020` 是桥接本地注入的安全帧标志（主机不会发送）。桥接注入帧的 `flags` **只含该位**（无 ENABLE/ARM/ESTOP），`seq` 自增；STM32 见到该位必须立即撤销授权并失能。只升级桥接而不升级 STM32 时该位会被忽略。
- 电机帧 `p[12..14]` 是原先的 0 填充：旧解码器 `proto_decode_motor()` 不读这 3 字节，完全兼容；新解码器 `proto_decode_motor_ext2()` 读取安全位/丢弃计数/ARM 回显。
- 增益帧 `0x03`、参数帧 `0x04/0x05` 未改线格式，旧固件忽略未知类型/载荷的既有兼容性不变。
- 回退策略：三端**必须一起**回退到 v1.0；不允许只回退一端（否则 ARM/FAILSAFE 语义缺失）。

---

## 三、三端保护时限表（与契约 §2 完全一致）

| 名称 | 值 | 所在端 | 判定与动作 |
|---|---|---|---|
| `CTRL_CMD_MAX_AGE_MS` | 100 | 桥接入队、STM32 出队 | 超过即丢弃（`lc_age_exceeds`），计"过期包" |
| `BRIDGE_MASTER_TIMEOUT_MS` | 300 | 桥接 | 主机静默 >300 ms → 进入 FAILSAFE，清队列，每 50 ms 注入 `CTRL_FLAG_FAILSAFE` 失能帧 |
| `BRIDGE_FAILSAFE_PERIOD_MS` | 50 | 桥接 | FAILSAFE 注入间隔，20 Hz |
| `BRIDGE_STM_FRESH_MS` | 300 | 桥接 | STM32 反馈年龄 >300 ms → `STM_FRESH=0`；**不再转发缓存 STATUS** |
| `BRIDGE_UPLINK_PERIOD_MS` | 100 | 桥接 | 新鲜度帧发送周期（10 Hz） |
| `STM_CTRL_TIMEOUT_MS` | 300 | STM32 | 无有效控制帧 → 撤销 ARM、立即失能（**删除原 1000 ms 宽限**） |
| `STM_CMD_MAX_AGE_MS` | 100 | STM32 | 出队命令过期丢弃 |
| `STM_FB_TIMEOUT_MS` | 200 | STM32 | 已使能时电机反馈 >200 ms 未更新 → 撤销授权、失能、置 FAULT |
| `STM_FB_GRACE_MS` | 1000 | STM32 | 上电/清错后允许首次反馈的宽限 |
| `MASTER_STM_STALE_MS` | 300 | 主机 | STM32 反馈或桥接新鲜度帧 >300 ms 未更新 → 进入 SAFE（撤销 ENABLE/ARM、清命令队列） |
| `MASTER_BRIDGE_STALE_MS` | 300 | 主机 | 收不到桥接帧 → 同上 |
| `ESPNOW_CB_TIMEOUT_MS` | 250 | 主机/桥接 | ESP-NOW 发送在途超时，计入超时，允许后续发送（不无限等待） |
| `WS_CMD_Q_LEN` | 8 | 主机 | 普通命令队列；**安全命令不走该队列**，见第二节 |
| `CAN_RECOVER_THRESHOLD` | 64 | STM32 | 连续发送失败/BOFF → 请求恢复 |
| `CAN_RECOVER_PERIOD_MS` | 2000 | STM32 | 恢复最小间隔 |

> 说明：`STM_FB_GRACE_MS=1000 ms` 只用于"上电/清错后允许首次反馈"的启动宽限，
> **不是**失联后的控制宽限；失联保护是 300 ms 且立即生效。文档/日志中出现
> "失联 1000 ms"都属于旧口径，已全部删除。

---

## 四、STM32 任务划分与 Flash/RAM 预算

### 4.1 任务表（最终实现口径）

| 任务 | 优先级 | 周期/触发 | 栈(words) | 职责 |
|---|---|---|---|---|
| `ctrl` | 5（最高） | TIM2 1 kHz 通知（中断只发通知，不做控制计算） | 384 | 看门狗 / ARM 授权 / CAN 发送协调 / 增益斜坡 / MIT 打包 / 邮箱完成统计 |
| `link` | 4 | 1 ms 轮询 | 320 | USART1 DMA 消费、成帧、序号/100 ms 过期、命令入队 |
| `can` | 4 | 1 ms 轮询 | 256 | CAN 接收、电机反馈、参数回包、总线恢复（持锁） |
| `status` | 3 | 20 ms | 320 | 50 Hz 状态帧 / 20 Hz 电机帧 / 参数回包，唯一 UART 发送者 |
| `ui` | 1（最低） | 100 ms + 500 ms 日志 + 5 s 栈诊断 | 384 | OLED / 调试日志 / 栈余量诊断 |

- **调度**：抢占式、静态分配（无 heap）；FreeRTOS-Kernel V11.2.0（从
  `x轴/野火stm/Middlewares/Third_Party/FreeRTOS-Kernel` 只读复制到
  `firmware/stm32_f103/third_party/FreeRTOS-Kernel`）。
- **节拍**：FreeRTOS 节拍源 = **TIM4 1 kHz**（`HardwareTimer`，中断向量由 STM32duino 核心持有）；
  **SysTick 仍只给 STM32duino `HAL_IncTick()` / `millis()`**，不与 RTOS 节拍抢向量。
- **中断优先级**：TIM2 = TIM4 = 5（允许 `FromISR`），SysTick/PendSV = 15，SVC = 0；
  4 位优先级分组；`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5`。
- **安全兜底**：`configCHECK_FOR_STACK_OVERFLOW=2`；`vApplicationStackOverflowHook` 与
  `app_rtos_assert_failed` 会先尽力发失能帧再 `NVIC_SystemReset()`；`uxTaskGetStackHighWaterMark`
  每 5 s 打印各任务栈余量。**没有动态分配**（`configSUPPORT_DYNAMIC_ALLOCATION=0`），
  因此不需要也不启用 malloc 失败钩子；所有任务栈/队列/互斥量静态分配，创建失败在启动期
  直接进入"失能 + 停机"而不是带病运行。
  
  另外 `can` 任务每 1 ms 检查一次 `g_ctrl_ticks`：连续 50 ms 没有 1 kHz 控制节拍
  （TIM2 停摆/被长时间屏蔽）→ 发失能帧 + 复位。这是独立于 ctrl 任务的最后一道兜底。
- **掉电/复位**：启动默认未授权、电机失能，不保存任何运动状态。

> ⚠ 上述"节拍源=TIM4 / SysTick 归 HAL"依赖具体 STM32duino core 版本，**必须上板确认**
> `SysTick_Handler` 是否仍调用 `HAL_IncTick()`、TIM4 中断是否与核心的 `HardwareTimer`
> 注册冲突（见第七节未完成验证清单）。

### 4.2 Flash / RAM 预算

基线实测（本轮修改前，主机端产物，**不是** FreeRTOS 移植后）：

| 项目 | 基线实测值 |
|---|---|
| Flash | **25,264 B** |
| RAM `.data + .bss` | **2,964 B** |
| 堆栈保留 | **1,540 B** |

移植后估算（**未编译、未上板实测；只用于判断是否可能装得下，最终以 Lead 的 map/`size` 输出为准**）：

| 项目 | 估算值 | 说明 |
|---|---|---|
| FreeRTOS-Kernel V11.2.0（tasks/list/queue + ARM_CM3 port） | Flash ≈ **5–7 KB** | 不编译 timers/event_groups/stream_buffer（未启用）；**无 heap 文件**（静态分配） |
| 任务栈合计 | RAM ≈ **6,656 B** | (384+320+256+320+384) words × 4 B = 6,656 B |
| Idle 任务栈 | RAM ≈ **512 B** | 128 words |
| 控制块/队列/信号量/静态存储 | RAM ≈ **1.0–1.5 KB** | 5 任务 TCB + 4 队列 + 2 mutex + 静态栈结构 |
| `linkcore` + `protocol`（新增/复用） | Flash ≈ **1–2 KB** | 纯 C、无 printf |
| 应用代码增量（任务拆分/安全槽/诊断） | Flash ≈ **2–4 KB** | 删除旧单循环后增量有限 |
| **移植后预估合计** | **Flash ≈ 35–40 KB / 64 KB**；**RAM ≈ 12–14 KB / 20 KB**（含基线 2,964 B 与 1,540 B 保留） | **估算，未编译未实测**；若 RAM 超限，优先压缩 `ui`/`status` 栈 |

> 动态堆：**0 B**（FreeRTOS 关闭动态分配；`configTOTAL_HEAP_SIZE=0`）。
> 上表中的"任务栈"和"控制块"都是 `.bss` 里的静态占用，不含在基线 2,964 B 内；
> 移植后真正的 `.data+.bss` 需要等有 SDK 的环境编译出 `.map` 才能确认。

> 结论：F103C8T6（64 KB Flash / 20 KB RAM）**大概率装得下**，但没有实测前不得当作结论；
> 若编译后 RAM 富裕不足 500 B，应重新评估 `ui` 任务是否降为事件驱动。
> 实际数字与警告见 `verification/freertos_port_20261024/`（由 Lead 填写）。

---

## 五、测试与编译结果

> 结论先行：**主机端 C 测试、Python 测试、三端源码的独立语法检查全部通过；
> 三端固件在本机无法完成 CMake 编译/链接**，原因是本机没有 arduino-esp32、
> STM32duino、PlatformIO 交叉工具链，且无外网可下载。**没有任何固件产物被生成**，
> 因此不存在"已编译通过"的固件；上板前必须在有 SDK 的环境重新构建。
> 原始日志与命令见 `verification/freertos_port_20261024/`。

### 5.1 主机端 C 测试（真实编译 + 真实运行）

环境：Windows 10/11 x64；CMake 4.0.3（`C:\Espressif\tools\cmake\4.0.3`）；
Ninja 1.12.1（`C:\Espressif\tools\ninja\1.12.1`）；MinGW-w64 GCC/G++ 15.2.0（`D:\mingw\mingw64`）。
因中文源码路径会让 GCC 打不开文件，本轮按"ASCII 隔离构建目录"要求，把
`靶车/靶车` 镜像到 `C:\tc-build-reliability\src` 后构建（源码与工作区逐字节一致）。

```powershell
cmake -S . -B build-host -G Ninja -DCMAKE_C_COMPILER=D:/mingw/mingw64/bin/gcc.exe
cmake --build build-host --target test_protocol test_ws_proto test_linkcore
ctest --test-dir build-host --output-on-failure
```

结果（2026-10-24）：**3/3 通过，0 失败**。

| 测试 | 内容 | 结果 |
|---|---|---|
| `test_protocol` | 协议 v1.1 线格式/CRC/往返/限幅/新 flags 位 | Passed |
| `test_ws_proto` | WebSocket SHA-1/Base64/握手/帧（既有测试） | Passed |
| `test_linkcore` | 序号窗口（重复/乱序/迟到/回绕）、队列满急停、停止屏障、过期命令、ARM 防重放、复位启动门、断联恢复、反馈新鲜度、ESP-NOW 提交/回调/超时分离、CAN 优先级/停止/恢复、坏帧重同步 | Passed |

### 5.2 Python 协议与行为测试（真实运行）

```powershell
C:\Espressif\tools\python\python.exe tools\test_protocol_v11.py
```

结果：**276/276 断言通过，退出码 0**。覆盖 CRC16-Modbus 标准向量、控制帧逐字节布局/
坏帧拒绝/限幅、ARM 位与 8 bit 令牌、防重放、`CTRL_FLAG_FAILSAFE`、0x07 新鲜度帧逐字段、
电机扩展帧 p[12..14]、8 bit 序号窗口（重复/乱序/回绕/128 边界）、命令过期模型
（100 ms/毫秒回绕/安全优先/停止屏障）、契约 §2 的 15 个时限常量，以及与 `protocol.h`
的只读交叉核对。一键脚本：`pwsh -File tools\run_reliability_tests.ps1`
（有 cmake 时连同三个 C 测试一起跑；缺 cmake 时明确提示并继续 Python）。

### 5.3 三端源码的独立语法检查（不是固件编译，不产生 .bin）

| 源码 | 工具/方式 | 结果 |
|---|---|---|
| STM32：`app_main.c`、`dm_motor.c`、`can_drv.c`、`rtos_start.c`、`link_uart.cpp`、`timer_drv.cpp`、`main.cpp`、`lib/linkcore/*.c`、`protocol.c` | arm-none-eabi-gcc/g++ 14.3.1（STM32CubeIDE 自带），`-mcpu=cortex-m3 -mthumb -Wall -Wextra -fsyntax-only`，CMSIS 头来自 `x轴/野火stm/Drivers/CMSIS`（只读引用） | **0 错误 0 警告** |
| STM32：FreeRTOS-Kernel `tasks.c/list.c/queue.c/port.c` | 同上，带本工程 `FreeRTOSConfig.h` | **0 错误**（证明配置宏合法） |
| ESP32 主机：`esp32_master/src/main.cpp` | MinGW G++ 15.2 `-std=gnu++17 -Wall -Wextra -fsyntax-only` + 临时 Arduino/WiFi/ESP-NOW 桩头（未入库），`protocol.h`/`linkcore.h` 用真实头 | **0 错误 0 警告** |
| ESP32 桥接：`esp32_bridge/src/main.cpp`、`display_esp32.cpp` | 同上 | **0 错误 0 警告** |

> 语法检查能发现类型/签名/拼写/格式串问题，但**不能**发现链接问题、SDK 版本差异、
> 真实调度/时序/无线/总线问题。

### 5.4 三端固件 CMake 编译（本机阻塞，未生成固件）

尝试命令（各自工程目录）：

```powershell
cd firmware/esp32_master  && cmake --preset esp32-master
cd firmware/esp32_bridge  && cmake --preset esp32-bridge
cd firmware/stm32_f103    && cmake --preset stm32-f103
```

三个配置都在 `project()` 前的 `pio_require_package()` 失败退出（rc=1）：

| 工程 | 错误（原文摘要） | 期望路径 |
|---|---|---|
| esp32_master | `缺少 PlatformIO 包 'toolchain-xtensa-esp32s3'` | `C:/Users/y/.platformio/packages/toolchain-xtensa-esp32s3` |
| esp32_bridge | 同上 | 同上 |
| stm32_f103 | `缺少 PlatformIO 包 'toolchain-gccarmnoneeabi'` | `C:/Users/y/.platformio/packages/toolchain-gccarmnoneeabi` |

本机实际情况：`~/.platformio/packages` 下只有 `contrib-piohome`；缺少
`framework-arduinoespressif32`、`tool-esptoolpy`、`framework-arduinoststm32`、
`framework-cmsis` 等全部框架包。联网检查 `curl https://github.com`、
`https://api.registry.platformio.org` 均返回连接失败（000），**无法在线补齐**。
因此按"不擅自升级框架/不改系统配置"的约束，本机未安装任何 SDK，也未生成
`.bin/.elf/.map`；Flash/RAM 的移植后数字仍是估算（见 4.2）。
在有 SDK 的机器上补齐上述包后，用同样三条 preset 命令即可构建；
STM32 构建目录必须保持 ASCII（预设已指向 `C:/cmake_build/target-vehicle/stm32`）。

### 5.5 网页静态检查

- `web/index.html`：由前端工作流做了 JS 语法/标签平衡检查，并用临时 Node 桩对
  `stStateText`（0..4）、`st_fresh` 过期、`safe_reason` 1..5、回调口径丢包率、
  `br_flags/br_age` 等 26 项 computed 逻辑做运行时断言，全部通过（临时脚本未入库，
  页面本身不引入 Node 依赖）。
- **未在真实浏览器 + 板子上联调**：`st_fresh`/`safe`/诊断字段的实际显示与
  JSON 长度（估算最坏 871 B < 1000 B）需上板实测。

> 以上测试只能证明"协议/纯逻辑/源码语法"层面正确，**不能**证明真实 ESP-NOW、
> UART、CAN、电机、供电干扰下的行为，也不等于固件可编译。

---

## 六、上板分阶段验收步骤

> 前提：**三端同时烧 v1.1 固件**；云台先**断开负载或空载**；24 V 电源可随时物理断开；
> 手边有串口工具（主机 460800、桥接 115200）。每一步把串口日志/截图存到
> `verification/freertos_port_20261024/`（由 Lead 归档）。

| 阶段 | 操作 | 通过判据 | 失败时先查 |
|---|---|---|---|
| 0 静态确认 | 不接电机，只上电三端 | 三端启动日志都打印协议版本/任务启动；无栈溢出/内存失败；OLED 正常 | 供电、烧录校验、分区/链接脚本 |
| 1 无线空载 | 主机+桥接上电，不发运动命令 | 桥接日志 `UPLINK_READY`；主机 `br_age` < 300 ms、`br_flags` 含 `STM_FRESH`（若 STM32 已接）；0x07 帧计数稳定 10 Hz | 信道是否一致、`esp_now_add_peer` 返回值、天线 |
| 2 串口/STM32 | 接桥接↔STM32 UART，主机只观察 | 主机 `st_fresh=1`、`st_age` 稳定；`st_state` 与 STM32 实际状态一致；拔掉 STM32 电源后 300 ms 内 `st_fresh=0`/`st_online=0`，**页面显示"过期/离线"，不再是实时** | UART 接线/波特率、DMA 环形缓冲、`lc_rx` 统计 |
| 3 CAN 反馈 | 接 CAN 收发器与电机（**不使能**） | 电机温度/错误码/位置以 20 Hz 回传；`err=0/1` 不被误判故障；拔 CAN 后 STM32 置 FAULT 而不是继续"正常" | CAN 终端电阻、1 Mbps 时序、PB8/PB9 |
| 4 首次使能 | 空载、小 kp（如 40）、用户明确点"使能" | `arm=1`、`arm_token` 变为非 0；电机使能；页面显示"已授权"；松开网页不自动掉使能（业务语义见第八节） | `E` 前置检查（急停/kp/kd/新鲜度）、桥接是否转发 ENABLE、STM32 `lc_arm` 日志 |
| 5 断联/恢复 | 直接给主机断电（或拔桥接天线） | 桥接 300 ms 内打印 FAILSAFE、每 50 ms 注入失能帧；STM32 300 ms 内撤销 ARM、失能；**主机恢复后不会自动运动**，页面 `safe=1` 且"使能"禁用，必须重新点使能 | `safe_reason`（1/2/3/4）、桥接 FAILSAFE 锁存退出条件 |
| 6 急停/清错/存零点 | 运动中按急停 `x`；再 `R`；`e`；`z` | 急停立即失能并锁存；`R` 只解除急停**不恢复使能**；清错/存零点有明确回执；安全命令不因队列满丢失 | `s_safety_cmd` 旁路、`WS_CMD_Q_LEN`、STM32 `lc_can_tx` 安全槽 |
| 7 长跑/劣化 | 连续运行 ≥30 min，人为遮挡/拉远/丢包 | 无看门狗复位；`cb_timeout`/`q_drop`/`cmd_age_drop` 可解释地增长；栈水位 > 20%；README/日志无"1000 ms 宽限"旧口径 | 内存/栈、WiFi 信道干扰、日志级别 |

> **软件测试/编译不能替代真实无线/UART/CAN/电机/供电测试。** 阶段 5 的"断联后不自动
> 恢复运动"是本轮最关键的验收项，必须实机复现，不能只看代码。

---

## 七、未完成验证清单

- [ ] **三端固件编译**：本机无 arduino-esp32 / STM32duino / PlatformIO 交叉工具链且无外网，
      ESP32/STM32 固件无法在本机编译——**阻塞项**（缺失包、命令、错误日志见
      `verification/freertos_port_20261024/`，不得伪造通过）。
- [ ] STM32 FreeRTOS 移植后 **Flash/RAM 实测**（当前为估算，见 4.2）。
- [ ] **TIM4 节拍源 / SysTick 归 HAL** 是否与所用 STM32duino core 版本冲突（上板确认）。
- [ ] 中断优先级分组、`configMAX_SYSCALL_INTERRUPT_PRIORITY`、栈溢出钩子在实机上生效。
- [ ] ESP-NOW 真实链路质量/距离/信道、单播↔广播回退判据。
- [ ] USART1 DMA 环形缓冲在 921600 下的溢出/滞后表现。
- [ ] CAN 1 Mbps 实际时序、电机反馈超时/BOFF 恢复路径、`lc_can_tx_t` 安全槽在最坏情况下的延迟。
- [ ] 网页在真实手机/浏览器 + 板子上的显示（`st_fresh`、`safe_reason`、`arm`、诊断计数、曲线）。
- [ ] 主机 JSON 总长度 < 1000 B 的实测（新增字段后）。
- [ ] 0x07 帧与"旧主机/旧固件"共存场景（按设计旧端忽略，但需实测确认不会误动作）。
- [ ] 三端同时升级/同时回退的操作流程演练。
- [ ] 长时间运行（≥30 min）与看门狗/复位原因、内存泄漏观察。
- [ ] `J`/`D`/`b` 串口重启类命令在网页端被拒绝的实测（HTTP 400/提示）。

---

## 八、业务语义说明（网页关闭后自主轨迹继续，未改变）

- 主机（ESP32）是轨迹发生器：**网页/浏览器关闭、手机 WiFi 断开、WebSocket 断开都不会停止
  自主轨迹**；网页只是"遥控器"，状态推送断开只影响显示（退回 HTTP 轮询或显示离线）。
- 主机进入 SAFE 的触发条件只有**链路失效类**：桥接新鲜度帧/STM32 状态 >300 ms、桥接报
  `FAILSAFE|MOTION_BLOCKED`、ESP-NOW 在途连续超时。**没有**新增"网页断开就停车"的语义。
- 但安全授权约定不变：**没有用户新的、明确的使能命令，断联恢复、解除急停、CAN 恢复、
  设备复位都不得恢复运动**；急停/停止/失能命令不得因队列满而静默丢失。
- 因此推荐的操作习惯：网页关闭不影响已授权的自主轨迹；需要停车时用**急停/停止按钮或物理
  断电**，不要依赖"关网页"。
