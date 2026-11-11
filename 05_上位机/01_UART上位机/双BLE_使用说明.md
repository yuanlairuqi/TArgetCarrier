# TargetCarrier 双 BLE 直连 5.0

日期：2026-10-25。电脑分别直连 x 轴 ESP 和靶车桥接 ESP，靶车不再需要原 ESP 主机。

## 启动和操作

在 D:\TargetCarrier-Parallel 双击“启动双BLE控制台.lnk”或“启动统一控制台.cmd”。
app/TargetCarrier-Unified-DualBLE/TargetCarrier-Unified.exe 必须和同级 _internal 一起保存。
旧版程序和入口备份保留；使用时关闭占用相同 BLE 设备的其他上位机或手机连接。

1. x 轴控制页：搜索并选择 TC-XAxis；靶车控制页：单独搜索并选择 TC-Target，再“连接并锁定”。
2. 先看 BLE 连接、STM32 新鲜反馈和电机停止反馈。靶车手动控制先解除急停，再明确使能。
3. 并行程序页为两台设备分别追加动作/等待模块，各自顺序执行；可单独启动或启动两条程序。
4. 运行中追加/修改仅影响待执行模块，当前动作保持原计时。正转 10 秒期间追加反转 10 秒，反转在本线当前动作结束并收到停止反馈后执行。
5. 单独停止一条线不停止另一条。断线、失焦、故障或全部停止中止程序，恢复后需手动重新启动。
6. x 轴限位后保持通信；两路输入释放稳定至少 100ms，且 STM32 已停止后，手动限位复位。复位不自动运动。

靶车保留角度、速度、kp/kd、静态/正弦/扫描/圆形/8字/手动六种轨迹及往返巡航。
轨迹由电脑生成，经 BLE 桥接和原 17 字节 UART 协议传给 STM32/CAN。
当前角度范围 yaw -180～180°、pitch -30～60°；界面速度范围 yaw ±90°/s、pitch ±60°/s。
定时运动不保证持续转动或机械到位，并行启动存在电脑通信时差。

## 两台设备必须使用对应固件

|设备|广播名|BLE 服务 UUID|UART|成品应用|
|---|---|---|---|---|
|x 轴 ESP32-S3|TC-XAxis|6e400001-b5a3-f393-e0a9-e50e24dcca9e|TX GPIO4 / RX GPIO5，115200|firmware/x/esp32_robot.bin|
|靶车桥接 ESP32-S3|TC-Target|6e410001-b5a3-f393-e0a9-e50e24dcca9e|TX GPIO1 / RX GPIO2，921600|firmware/target/target_ble_bridge.bin|

靶车 GPIO1 → STM32 PA10(RX)，GPIO2 ← STM32 PA9(TX)，两端 GND 共地；3.3V UART 逻辑。
x 轴保持原有 GPIO4/5 接线，不能套用靶车 GPIO1/2。
两台 ESP 使用不同服务，按服务 UUID 筛选。PC 不再填写靶车 IP。
靶车特征 UUID 命令/反馈分别为 6e410002 / 6e410003，其余尾段与服务相同。
本次不修改 STM32、原靶车 ESP 主机和旧 Arduino 桥接源。旧 ESP-NOW 桥接固件不提供本次 BLE 服务。
两新固件均为独立 BLE 方案，未启用 ESP-NOW；不能用来替代原 ESP-NOW 从机而期待原无线控制继续工作。

## 连接超时诊断

每个应用目录 logs/connection.log 记录连接尝试和异常。
连接/服务发现：使用扫描得到的设备对象，关闭 Windows GATT 服务缓存，最多两次、每次 20 秒。
两台设备的扫描和连接建立互斥，但已有连接的通知和运动控制仍由各自线程处理。
日志“尚未进入 STM32 握手”表示卡在 BLE 连接或服务发现，需先确认固件、供电、是否被其他端占用。
BLE 已连接但无 STM32 新鲜状态时，查对应 UART 接线/波特率、STM32 固件和下游反馈。
软件未修改 Windows 注册表、驱动、服务或系统蓝牙设置。

## 验证结果和范围

172 项上位机测试通过，包含双线并行、运行中追加、断线不续跑、实际 ARM 回显、停止/使能反馈和关闭资源清理。
新轨迹与原 C 生成器的 600 个样本比对通过；桥接 C guard 测试通过；两套 ESP-IDF 固件编译通过；EXE 构建和离线界面检查通过。
verification/ 保留原始日志、构建信息、扫描和界面人工复核记录。SHA256.json 校验本交付的文件。
先前只读 BLE 扫描成功，但匹配设备为空。没有真实双 BLE 连接、烧录、UART/CAN 或电机测试。
x 轴“连接超时”的实机根因尚未确认，当前已烧录固件版本也尚未确认；测试通过不能视为实机修复确认。
控制器零速/失能反馈只作为程序调度条件，不等于已验证机械停止。

## 源码与构建

source/pc 为上位机；source/x 为 x 轴 ESP-IDF 项目；source/target/esp32_bridge/ble_direct 为靶车新 IDF 项目。
靶车共享库位于 source/target/lib，保持相对层级，不要单独复制 ble_direct 后丢失共享库。
使用 ESP-IDF 6.1、esp32s3 构建；source/build_idf.ps1 记录本机工具路径，环境变量只对当前进程生效。
示例：在 IDF 6.1 终端进入项目，运行 idf.py set-target esp32s3 后 idf.py build。
未打包 sdkconfig、本机私有配置和编译缓存，干净构建从 sdkconfig.defaults 生成配置。
原工程源码与启动入口的修改前副本在 backups/；D 盘入口额外备份到 _backups/双BLE_5.0_20261025。
