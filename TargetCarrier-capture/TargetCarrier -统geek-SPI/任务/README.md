# SPI 通信工程交付

完成日期：2026-10-28（Stamp-S3Bat 实物板型引脚修订）。工程目录：`C:\Users\y\Desktop\TargetCarrier -统geek-SPI`。

接收端板型依据用户照片确认为 **M5Stack Stamp-S3Bat / S3BAT**，侧边可接 G1～G11。[官方引脚资料](https://docs.m5stack.com/en/core/Stamp-S3Bat)；GPIO16仅在细间距BTB口，不用于本版接线。此前含GPIO12～16的接线分配已撤回，旧配置备份位于任务/板卡引脚修正_20261028/backup，不可再照旧表接线。

本副本将 x轴、靶车两组 ESP32-S3 ↔ STM32 的**实际业务链路**改为 SPI。ESP32 为主机，STM32 为从机；同一固件仅启用一种有线后端。没有自动切换或多接口并行机制。

## 工程入口

| 工程 | 用途 | 本次结果 |
|---|---|---|
| ESP32-S3-GEEK | USB ↔ ESP-NOW 无线主机 | 无线链路保持原实现，重新编译 |
| x轴/esp_无线从机_20261022 | x轴 ESP-NOW 从机 | 下行和回传均改为 SPI，编译通过 |
| x轴/esp | x轴直连 BLE 备选从机 | 下行和回传均改为 SPI，编译通过；与上一项二选一 |
| x轴/野火stm | STM32F103C8T6、HAL/FreeRTOS | 业务协议与电机处理保留，有线接口已替换，编译通过 |
| 靶车/靶车/firmware/esp32_bridge/ble_direct | 靶车 ESP BLE/ESP-NOW 从机 | 业务链路改为 SPI，编译通过 |
| 靶车/靶车/firmware/stm32_f103 | STM32F103、STM32duino/FreeRTOS | 有线接口已替换，编译通过 |

旧 Arduino ESP32 bridge/master、旧脚本、历史构建输出、快捷方式和复制前的文档仍随完整工程保留；它们不是本次交付入口，也不能据其 UART 字样判断新固件的物理接口。新构建请使用 `tools/build_wired.py`，交付固件只认本目录 `固件_未配对`。

## 交付结构

```text
工程根目录/
  wired_transport/     C 协议封装、ESP/STM 事务驱动、板级配置和测试
  tools/build_wired.py 构建入口，只编译，不烧录
  tools/test_wired.py  软件测试入口
  任务/
    README.md
    接线与验收.md
    协议与移植.md
    验证结果.md
    通信改动源码.zip
    固件_未配对/      六个固件工程的输出
    验证/            构建日志、测试日志、来源和 SHA256
    接口移植_20261028/backup/  修改前适配文件
```

上位机和 GEEK 仍使用原 USB/无线报文，x轴 32 字节、靶车 17 字节业务报文原样封装，界面不依赖新底层驱动。本次没有重写或重新编译上位机，也没有新增波形采集、参数表或持久化需求；已有业务参数/状态经新链路传输。完整的通用调参模块仍在复制过来的 `portable_comms` 中，新业务链路的权威实现是 `wired_transport`。

## 构建与测试

在本工程根目录运行，输出必须选择新的纯 ASCII 目录，示例路径使用后再次构建需更换末尾目录名：

```powershell
python tools/build_wired.py --target all --out C:/Users/y/Documents/Codex/work/spi_fresh
python tools/test_wired.py --out C:/Users/y/Documents/Codex/work/spi_tests_fresh
```

`--target` 可选 `stm`、`esp`、`ble`、`master`、`all`。`esp` 含三个从机工程；`all` 再含两组 STM32 和 GEEK。脚本将本副本源码复制到指定 ASCII 构建目录，显式设置共享模块路径，不从基准目录取源码，不向基准目录写输出。

本机依赖：ESP-IDF 6.1、Xtensa GCC 15.2、ARM GCC 14.3.1、CMake、Ninja；STM32duino/FreeRTOS 依赖通过 `--sdk` 指定，本机默认 `C:/Users/y/Documents/Codex/work/tc-review/sdk`。主机测试默认 GCC 为 `D:/mingw/code/mingw64/bin/gcc.exe`，可传 `--gcc`。换电脑应修改脚本中的本机工具路径，SDK/引脚/时钟不属于通信核心。

默认不会读取本地配对配置，交付的 ESP 固件均为**未配对编译产物**，不可当作已配好实际 MAC/密钥的成套固件。若需生成实际 ESP-NOW 配对版本，使用副本中三个无线工程已有的真实 `pairing.local.defaults` 并加 `--paired`。脚本不生成 MAC/密钥，源码压缩包不包含这些私有配置。BLE 备选工程无需该配对文件。未执行任何烧录命令。

## 版本边界

复制时核对了 3397 个非 `.git` 文件，两份副本与当时基准一致。最近比对发现基准目录后续有 14 个已有文件变化、431 个新增文件、0 个缺失文件，包括灯效组件和上位机更新，详情见 `验证/baseline-final.json`。本次命令未向基准目录写入；无法将最终全目录差异等同于本次改动，也不声称基准全程未发生变化。两份副本没有自动合并这些后续更新，GPIO10已由用户保留为输出信号，后续合并功能时需保持该保留；本板板载RGB实际由M5PM1管理，不能套用外置GPIO10灯效。

所有结论为源码、编译和主机软件模型层面；没有实物连线、烧录、总线波形或电机测试。
