# X轴双OLED状态显示

2026-10-21：STM32 与 ESP32-S3 各连接一块独立四针 I2C OLED。仅修改 TargetCarrier/x轴；未操作靶车或其他副本。

## 硬件与型号边界

| 主板 | OLED SCL | OLED SDA |
| --- | --- | --- |
| 野火 STM32F103C8T6 | PB6 | PB7 |
| ESP32-S3 | G9 / GPIO9 | G8 / GPIO8 |

两块OLED各自与所属主板共地，不把两块主板的屏幕信号线并接。VCC需按实际模块规格确认；SCL/SDA上拉电压必须适合3.3V GPIO，不能因模块支持5V供电就把5V上拉接到S3。现有UART G4/G5、采样G6/G7和PA8/PB0输出保持不变。

用户尚未确认OLED芯片与分辨率。软件默认是 **SSD1306、128×64**，提供 **SH1106、128×32/128×64** 配置；这不是对实物型号的识别。地址0表示依次尝试7位0x3C和0x3D，不会自动识别驱动芯片或分辨率；不要填写已左移的0x78。SH1106采用页寻址和2列偏移。

## 显示内容

使用英文/数字小字体，64像素高显示8行，32像素高每3秒交替显示前4行和后4行。默认每500ms刷新。

STM32：标题、UART在线与接收帧数、运动状态、SET HZ、DIR与OUT、运动/串口错误、运行秒数、LOGIC STATUS ONLY。

- SET HZ来自当前正在运行的TIM1配置，OUT关闭时显示0。它是软件配置频率，不是实测脉冲频率；DIR为当前GPIO输出设置。换向等待阶段不显示旧的输出频率。
- ERR M是本机运动故障布尔值，R/T分别为UART收发错误累计次数；UART OFF表示未连接或超时。运动状态包括STOP、连续输出、换向保持/等待等。
- 上电无动作、连续控制和STOP的现有策略未改变。

ESP：标题、BLE/STM在线状态、控制来源、请求方向和速度百分比、STM输出应答状态、PUL实测Hz/DIR、ESP/STM错误码、运行秒数。

- SRC指当前BLE/ESP-NOW控制权状态，不表示该来源正在发送运动命令。
- ACK必须匹配当前revision和命令序号，并具有PEER_APPLIED标志；WAIT/OFFLINE不能视为已执行。ACK表示STM软件/定时器输出确认，不是电机到位或实际运动反馈。
- PUL频率来自G6的两个相邻上升沿及MCPWM硬件时间戳，不需要手机订阅波形；DIR是读取G7的当前输入电平。
- 超过500ms无新上升沿，或恢复信号后还没有两个新上升沿，显示NO RECENT EDGES；不会保留旧频率。此提示不是电机停止证明。
- CAPTURE OFF表示采集未配置或初始化失败。ERR E与STM错误码沿用原协议，详情参见esp/main/robot_control.h及野火stm/App/Link/robot_peer.h。

## 任务与失败处理

STM32 OLED为优先级1的独立静态任务，ControlTask优先级2。ControlTask每250ms发布小型状态快照，屏幕任务只在短临界区复制数据，排版和所有I2C发送均在临界区外完成。PB6/PB7使用可被抢占的软件I2C，约100kHz；DWT提供微秒延时，SCL拉高等待上限2ms，不关中断完成整帧。没有引入HAL I2C驱动，.ioc将这两个引脚保留为开漏GPIO；重新生成后仍由App/Display负责软件I2C。

ESP OLED为优先级1的独立任务，telemetry优先级5。状态快照通过独立的短临界区发布；I2C使用ESP-IDF硬件master驱动、100kHz，每次发送/探测超时20ms。屏幕初始化、缺失、NACK、掉线不触发ESP_ERROR_CHECK或运动命令，也不占用控制互斥锁进行I2C。屏幕连接失败每3秒重试。GPIO配置冲突或总线初始化失败仅停用显示。STM32可用SWD查看g_oled_online、g_oled_address、g_oled_errors；ESP使用oled日志。

这些是软件设计和测试范围，实际任务时序、I2C信号、电平与OLED显示仍须硬件验证。

## 宏与构建

STM32配置入口：cmake/oled.cmake。现有ASCII构建脚本增加以下参数：

```powershell
python tools/build_stm32.py --config Release --oled ON --oled-driver SSD1306 --oled-height 64 --oled-address 0 --oled-ms 500
# 切换SH1106或关闭显示
python tools/build_stm32.py --config Release --oled-driver SH1106
python tools/build_stm32.py --config Release --oled OFF
```

若工具不在PATH，仍需传现有的--cmake、--gcc、--ninja完整路径。OLED参数映射到APP_OLED_*编译宏，修改后重新构建、烧录。不能在运行时通过宏改变配置。CubeMX重新生成后检查优先级、中断和原项目保留区，不要让生成代码覆盖FreeRTOS端口。

ESP配置入口：main/Kconfig.projbuild的Local diagnostic OLED菜单，或sdkconfig中的CONFIG_ROBOT_OLED_*；sdkconfig.defaults提供首次配置默认值。ROBOT_OLED_SH1106控制芯片类型，HEIGHT仅允许32或64，ADDRESS仅允许0/60/61，SCL/SDA默认9/8，PERIOD_MS默认500。保留原UART、BLE、无线配对配置。用已有ESP-IDF v6.1环境在独立ASCII工作副本执行idf.py build，使用新的构建目录。OLED宏变更同样需要重新构建和烧录。

## 验证与交付

本轮构建和测试日志位于verification/oled_20261021，固件在其中的firmware子目录；RESULT.json记录源码/产物校验。STM32 Debug/Release、OLED关闭、SH1106 32像素配置，以及ESP默认OLED构建均应以实际日志为准。代码仍以两个独立CMake工程构建。

主机测试入口：

```powershell
python tools/test_oled.py --gcc D:/mingw/mingw64/bin/gcc.exe
```

该入口检查两份OLED渲染代码的完整帧、清屏、32像素分页、SH1106偏移、错误提前退出、无效配置、最大文本宽度，以及采样频率的起始、过期、恢复、计数器回绕和异常时间；不模拟实际屏幕或I2C电气波形。原有25组控制回归单独执行。

没有烧录、没有验证OLED实际显示，没有电机硬件测试。不要把已有硬件中的旧固件视为已包含新功能。刷入前应断开驱动信号，先验证两个屏幕型号/显示方向、UART和停止处理，再进行后续联调；本轮不执行烧录。

驱动命令参考原厂数据手册：[SSD1306](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)、[SH1106](https://www.displayfuture.com/Display/datasheet/controller/SH1106.pdf)；ESP硬件I2C参考[ESP-IDF v6.1官方文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/peripherals/i2c.html)。实物面板配置仍待确认。
