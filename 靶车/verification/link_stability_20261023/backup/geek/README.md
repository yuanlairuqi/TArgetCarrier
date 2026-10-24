# ESP32-S3-GEEK_无线遥控总机

2026-10-22；电脑 USB → GEEK → 加密 ESP-NOW 单播 → X轴 S3 → 原UART v2 → 原STM32 → 驱动器。工程显示名称如标题；ESP-IDF内部构建ID为`ESP32-S3-GEEK_wireless_master`，便于Windows工具链使用ASCII产物名。

本轮仅编写、编译、软件验证，未烧录、未打开实际USB串口、未进行无线/屏幕/电机实测。默认编入无线代码，但没有实物STA MAC和密钥，运行时无线保持禁用；交付BIN不是已配对可运行版。

## 原版与副本

| 角色 | 路径 |
| --- | --- |
| 总机新增工程 | `C:\Users\y\Desktop\TargetCarrier\ESP32-S3-GEEK` |
| 从机原版，保留 | `C:\Users\y\Desktop\TargetCarrier\x轴\esp` |
| 从机兼容副本 | `C:\Users\y\Desktop\TargetCarrier\x轴\esp_无线从机_20261022` |
| STM32原版，保留 | `C:\Users\y\Desktop\TargetCarrier\x轴\野火stm` |
| Vue原版，保留 | `D:\TargetCarrier-PC-Vue` |
| Vue USB独立副本 | `D:\TargetCarrier-PC-Vue-USB_20261022`（协作聊天交付，最终结果见交付说明） |
| 原Tkinter，保留 | `D:\TargetCarrier-PC` |

靶车目录未修改，旧GEEK TCP演示及官方资料未修改。未提交、推送、开PR或更改远程分支。

## 源码改动

- 总机：新增固定目标USB/ESP-NOW中继；GU/1有界拆包；USB断连/心跳锁定；原ST7789驱动改为真实链路信息；独立低优先级LCD任务；配对工具和可复现ASCII快照构建工具。
- 从机副本：`now_radio.c/h`增加控制优先队列、独立波形槽、100ms发送过期控制和MAC统计；`main.c`增加无线波形封装与OLED实际控制源；`pulse_capture.c/h`增加复用现有采样的输出sink；`robot_control.c`将异常/重复STOP规范为模式0/时间0。原`now_protocol.c`、UART协议、桥接确认联合判定和BLE接口保留。
- 从机`sdkconfig.defaults`编入无线，空配对字段不启用无线；新增2MiB应用分区，避免BLE/WiFi共存靠近原1MiB上限。4MiB Flash配置沿用从机原工程，不据此认定实物容量。
- Vue：复用原Controller/Channel/PulseAssembler及界面，增加USB客户端/入口与分层诊断；严格硬件确认、冻结/导出一致性及历史采样标识由独立优化副本完成。Android STOP重试问题仍记录在旧审查报告，本任务未修改Android原版。

## 配对与USB配置

先确认两板真实 Wi-Fi **STA** MAC，不能使用BLE地址、AP MAC或例子地址。本轮没有查询板上MAC。GEEK固件的USB信息包会返回STA MAC；从机可在后续获准硬件联调时读取STA MAC。选定当地允许且两端一致的2.4GHz信道。本轮默认编译信道1，不表示已验证无线环境。

配对工具必须显式给出真实参数。如下命令中的变量需先由你填入真实值；脚本拒绝相同/零/组播MAC和覆盖现有配对文件：

```powershell
python 'C:\Users\y\Desktop\TargetCarrier\ESP32-S3-GEEK\configure_pairing.py' `
  --master-mac $MasterStaMac --slave-mac $SlaveStaMac --channel $Channel --node 1 `
  --slave-copy 'C:\Users\y\Desktop\TargetCarrier\x轴\esp_无线从机_20261022' `
  --pc-copy 'D:\TargetCarrier-PC-Vue-USB_20261022'
```

仅在两份副本写`pairing.local.defaults`，同一PMK/LMK/AUTH_KEY、互为单播peer、相同node/channel；电脑副本写`usb.local.json`。这些文件已列入忽略规则。总机Kconfig复用字段名`ROBOT_ESPNOW_CONTROLLER_MAC`，在总机中此字段实际填写**从机STA MAC**，在从机中填写总机STA MAC。可用设备ID与客户端node参数扩展寻址，但本版只维护一个目标，没有多机调度或运动广播。

启动最终Vue USB EXE，使用USB搜索选择GEEK的COM口并连接。BLE入口保留。连接后必须得到从机和STM32有效STOP确认才能开始；不要把总机信息包或MAC发送成功当作停止确认。GEEK LCD与上位机没有真实电机位置传感器。

## 编译

本轮在ASCII隔离目录编译，再用哈希核对交付源码和产物，避免复制旧CMake缓存。工具链为本机`C:\Espressif\v6.1\esp-idf`、`C:\Espressif\tools`；构建工具只设置子进程环境，不改系统环境或Git全局配置。

重新编译到**不存在的新ASCII输出目录**：

```powershell
python 'C:\Users\y\Desktop\TargetCarrier\ESP32-S3-GEEK\build_snapshot.py' --out 'C:\tc-build\geek-new'
python 'C:\Users\y\Desktop\TargetCarrier\ESP32-S3-GEEK\build_snapshot.py' `
  --source 'C:\Users\y\Desktop\TargetCarrier\x轴\esp_无线从机_20261022' --out 'C:\tc-build\slave-new'
```

已生成真实配对文件时加`--paired`；工具使用全新sdkconfig和`SDKCONFIG_DEFAULTS=sdkconfig.defaults;pairing.local.defaults`，避免旧sdkconfig盖过新配对。输出含`result.json`、`source-sha256.json`、`build.log`、`src/build`。工具从不执行flash/monitor/esptool设备操作。配对构建产物含私密参数，不应公开提交。

Vue构建命令及独立验证见电脑副本README/build脚本。完整字节布局与限制见`通信协议.md`。

## 后续硬件联调（本轮未执行）

1. 断开驱动器PUL/DIR/ENA，核对板型、固件、USB枚举及LCD显示，读取两板STA MAC；使用真实参数生成配对并重新编译。后续烧录需另行明确授权。
2. 仅连接电脑USB与两块S3，确认固定peer/信道、双向状态、拒绝错node/旧会话、BLE占用与释放、拔USB/关闭上位机后的租约撤销；检查MAC成功和应用STATUS计数分别变化。
3. 核对原3.3V UART：S3 G4→STM PB11、G5←PB10、共地，115200/8N1；验证未APPLIED回包不能解锁、STM离线/恢复只重新STOP，连续待机不会自动运动。
4. 核对S3 G6←STM PA8、G7←PB0，先用逻辑分析仪交叉验证采样来源/频率/丢失标志；检查无线拥塞/波形大量回传时STOP仍可处理、两块从机OLED及GEEK LCD。
5. 只有前述链路通过且实际驱动器版本、电源、输入电平和机械限位明确后，再单独安排低速电机实测。APPLIED、GPIO脉冲均不能替代这一步。
