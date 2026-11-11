# 模块、上下文和最小接入

核心为标准 C 接口；本次 ESP-IDF 与 F103 CMSIS 适配也全部是 `.c/.h`，没有新增 C++ 层。核心不依赖 GUI、引脚、电机、ESP-IDF、HAL 或 FreeRTOS。Python 上位机协议与窗口分离。

| 模块 | 职责 / 公共 API | 依赖与所有者 |
|---|---|---|
| tc_protocol.c | `tc_encode/decode/feed/join/send`、`tc_push/pop` | tc.h、C库；一个协议任务所有 |
| tc_params.c | `tc_service_init/receive/tick`、`tc_service.control` | 协议API与调用方参数表；同一服务任务 |
| tc_wave.c | `tc_wave`、`tc_wave_buffer_emit`、`tc_wave_flush` | 协议API、有界快照队列 |
| esp_radio.c | `tc_radio_init/send/poll/receive/get_stats` | ESP-NOW、Wi-Fi、PSA、FreeRTOS队列；任务处理、Wi-Fi回调搬运 |
| esp_bus.c | `tc_bus_init/send/poll/receive/get_stats` | ESP-IDF驱动；协议任务唯一调用者 |
| stm32_bus.c | 同一 `tc_bus_*` API | F103 CMSIS、DMA、SPI NSS EXTI或I2C EV/ER IRQ |
| tc_protocol.py | pack/unpack/Parser/Join/Client | Python标准库；串口仅在连接时导入pyserial |
| portable_console.py | 参数输入、波形绘图和分层状态 | Tkinter+Client，无固件反向依赖 |

## 内存与所有权

- 全部核心对象由调用方分配，无malloc。`tc_service`约1.1 KiB，`tc_queue`8×132 B加管理项；实际目标尺寸以ELF/size为准。
- `tc_send`使用栈上槽并同步调用输出回调。回调返回前必须拷贝，不能保留传入指针。平台队列和无线回调均拷贝128/144 B固定槽。
- 服务参数表在服务整个生命周期内有效；协议所有者任务写入`bits`。其他任务用消息或短临界区复制一致快照，不能无锁逐字段读取多项参数。只有单项32位自然对齐值也不能代替应用层参数组事务。
- `tc_wave`在返回前复制值和通道表，将32位位模式逐项序列化为小端；可传原生U32/I32/IEEE754 F32数组。`now_ms`用于队列时效，`first_us`用于采样时间，不能从已回绕的微秒时间推导毫秒队列时效。
- 波形采集缓冲独立于发送；main演示在同一任务采集/flush。真正ADC DMA ISR只通知或移交双缓冲，采集任务读取完成快照后调用`tc_wave`，不要在ISR中分包/发送或运行GUI相关逻辑。
- SPI DMA缓冲在NSS事务期间由硬件占有，主循环不得改写。I2C发布邮箱在显式AK/2匹配前保留，TX中断从只读快照搬运字节。UART TX在DMA结束前保留。
- STM32适配在任务与IRQ之间使用保存/恢复PRIMASK的短临界区；RX事务结束的ISR有固定上限的128 B CRC与拷贝，不调用FreeRTOS API。它并不是已测得最坏执行时间的保证。

## 初始化顺序与任务

1. 板级供电/时钟/存储初始化；最小STM例程HSE8→72 MHz、SysTick1 ms；ESP先NVS init，失败直接退出，不自动擦除NVS。
2. 初始化消息对象、参数注册表和平台队列；选择且只选择一个后端。
3. 打开外设与IRQ/DMA；ESP无线读取**实际**私有配置并初始化加密peer，再使用短回调队列。
4. 启动协议所有者循环，先收高优先级请求并处理/tick，后flush波形，再调度外设。
5. 上位机显式连接USB、随机session HELLO，确认目标RESULT后再读写。

ESP独立示例使用`app_main`一个所有者任务、1 tick休眠，当前tick=1 ms；无线回调在Wi-Fi任务上下文，用普通非阻塞队列API，不能用FromISR。接收端I2C同步驱动操作各有25 ms上限，因此任务并非严格1 ms不阻塞；该上限及RF队列丢弃需要实测。

独立STM示例是最小裸机/中断循环，**没有替换既有FreeRTOS电机工程**。接入原RTOS时，把循环工作放入一个高于显示/日志的通信任务；按平台时基调用`tc_service_tick`，去掉独立main/SysTick/clock_init。中断优先级5仅为独立参考配置，接入时按目标的`configMAX_SYSCALL_INTERRUPT_PRIORITY`等统一校核。不要重复定义DMA1_Channel3、EXTI4、I2C2_EV/ER或SysTick向量。

## 最小参数接入

```c
#include "tc.h"
#include "tc_bus.h"

static tc_param params[] = {
    {10, TC_F32, 1, 0x3f800000, 0.0, 20.0}, /* gain = 1.0f */
    {11, TC_U32, 0, 100, 0, 1000}           /* read-only status */
};
static tc_service service;
static bool emit(void *ctx, const uint8_t *slot, bool low, uint32_t now) {
    (void)ctx; (void)low;
    return tc_bus_send(slot, now); /* copies before returning */
}
void comm_init(void) {
    /* initialize board clock first */
    if (!tc_bus_init()) { /* record fault, keep application safe */ return; }
    tc_service_init(&service, params, 2, emit, NULL);
}
void comm_poll(uint32_t monotonic_ms) {
    uint8_t slot[TC_SLOT];
    tc_bus_poll(monotonic_ms);
    for (int n=0; n<4 && tc_bus_receive(slot, monotonic_ms); ++n)
        tc_service_receive(&service, slot, monotonic_ms);
    tc_service_tick(&service, monotonic_ms);
}
```

应用控制通过设置`service.control = my_handler`接入。处理器接收opcode和完整载荷，返回`tc_command_result`；不要在处理器里长时间阻塞。异步业务先返回stage=2，完成后由项目单独发送关联状态；不能先返回stage=3再启动业务。实际控制任务应维护自己的急停、使能、会话租约、指令有效期及重启保护，保留原工程的控制安全语义。

## 波形接入

```c
static tc_wave_buffer samples;
void acquisition_task_snapshot(uint32_t first_us, uint32_t seq) {
    uint16_t ids[] = {10, 11};
    float values[] = {read_completed_sample_0(), read_completed_sample_1()};
    tc_wave(tc_wave_buffer_emit, &samples, current_session, seq, monotonic_ms,
            first_us, 40000, ids, 2, 1, TC_F32, values);
}
/* after servicing pending parameter/control requests: */
tc_wave_flush(&samples, emit, NULL, monotonic_ms, 20);
```

上例`read_completed_sample_*`和`current_session`由项目提供，不是本包实现的ADC驱动；实际可编译完整例程在`firmware/stm32/main.c`，使用明确标注的软件演示值。多任务生产/消费时由接入方提供临界区或消息移交；通用环形队列本身不是多生产者线程安全队列。

## 替换目标STM32时

替换CMSIS设备/启动/链接文件、HSE/PLL/总线时钟、GPIO复用、DMA通道/请求、IRQ向量/优先级、UART波特率寄存器、SPI NSS/DMA逻辑、I2C地址与事件/错误逻辑。F4/H7/G4等的DMA、I2C寄存器和Cache行为不同，**不能只换芯片宏**。有D-cache时加入DMA内存区域、缓存一致性及对齐管理。保留`tc_bus_*`边界，或按该API实现HAL/LL适配；核心协议/参数/波形无需引入原UI和电机文件。

ESP替换板时只修改`config/tc_board.h`和实际配对头。GEEK只运行角色1，普通S3运行角色2。按`docs/INTERFACES.md`核对屏幕/PSRAM/外设冲突。

## 构建与配置

构建脚本仅在本进程设置环境变量，不写注册表、系统环境变量或Git全局配置。它复制的是本包和最小CMSIS参考依赖，不复制整套TargetCarrier。ASCII构建快照避免Windows中文路径工具问题，源码/产物都不写回原工程。

```powershell
python 'C:\Users\y\Desktop\TargetCarrier -统筹\portable_comms\tools\build.py' `
  --work 'C:\Users\y\Documents\Codex\work\tc2-new-build' `
  --target all --backend all --role all `
  --arm-bin 'C:\Users\y\Documents\Codex\work\tc-review\sdk\toolchain-gccarmnoneeabi\bin'
```

以上ARM路径是本机已核对的工具位置；新机器换成本机实际ARM GCC目录。新`--work`目录不得已存在。只编一种时用`--target stm32 --backend 2`或`--target esp --role 2 --backend 3`。源码默认ESP-IDF6.1、CMake4.0.3、Ninja1.12.1；其他版本先验证API，不自动升级。本次参考ESP镜像按4 MB地址空间构建，普通接收板4 MB；GEEK实际16 MB但本镜像仅用前4 MB，未尝试利用其余Flash。

PC源码执行：安装`pc/requirements.txt`，运行`pc/portable_console.py`。PC协议只在显式连接时打开串口，不扫描或自动连接。GUI“后端预期”只是核对项，真正选择通过固件编译宏；接收端STATUS回报实际编译后端，未收到时显示未知。

无线启用：将`config/tc_pairing.example.h`复制为`tc_pairing.local.h`，填写两端各自实际peer STA MAC、相同真实信道与私有PMK/LMK/AUTH_KEY，`TC_RADIO_ENABLED=1`；再明确使用`tools/build.py --paired`。默认构建排除私有头；配对产物含敏感信息，不应公开发布。本次没有实际配对数据，交付为未配对构建。

无断电保存实现。未来需要持久化时另设显式“保存”操作、版本/CRC/双副本、限频和掉电恢复；不要每次在线写参都写Flash。
