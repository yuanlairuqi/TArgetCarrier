# TargetCarrier 靶车工程

本工程包含 X 轴、靶车旋转机构、ESP32 无线通信和电脑上位机的源码与相关资料。

## 文件夹

| 文件夹 | 作用 |
|---|---|
| `00_工程导航与汇总` | 工程结构、历史说明和录波相关资料 |
| `01_TargetCarrier -geek-UART` | ESP32 与 STM32 使用 UART 的工程 |
| `02_TargetCarrier -geek-I2C` | ESP32 与 STM32 使用 I²C 的工程 |
| `03_TargetCarrier -geek-SPI` | ESP32 与 STM32 使用 SPI 的工程 |
| `05_上位机` | 各版本上位机程序、源码和运行日志 |

各通信工程内的 `x轴`、`靶车` 和 `ESP32-S3-GEEK` 分别对应 X 轴控制、靶车控制和 GEEK 总机。I²C、SPI 工程中的 `wired_transport` 保存板间通信适配源码。

## 工程树状图

```text
TargetCarrier -geek
├─ 00_工程导航与汇总       工程结构与历史资料
├─ 01_TargetCarrier -geek-UART
│  ├─ x轴                  X 轴控制工程
│  ├─ 靶车                 靶车控制工程
│  └─ ESP32-S3-GEEK        GEEK 总机工程
├─ 02_TargetCarrier -geek-I2C
│  ├─ x轴 / 靶车           两套执行端工程
│  ├─ ESP32-S3-GEEK        GEEK 总机工程
│  ├─ wired_transport      I²C 通信适配源码
│  └─ 任务                 接线、烧录与验证记录
├─ 03_TargetCarrier -geek-SPI
│  ├─ x轴 / 靶车           两套执行端工程
│  ├─ ESP32-S3-GEEK        GEEK 总机工程
│  └─ wired_transport      SPI 通信适配源码
├─ 05_上位机
│  ├─ 01_UART上位机
│  ├─ 02_I2C上位机
│  ├─ 03_SPI上位机
│  ├─ 04_靶车手动360上位机
│  └─ 05_短路径副本日志
└─ README.md
```

## 使用上位机

进入 `05_上位机`，按照实际需要选择 UART、I²C 或 SPI 目录中的启动脚本。UART 目录中的联合通信修复版和诊断版启动脚本使用 `C:\Users\y\Documents\Codex\TargetCarrier-runtime-20261107` 下的运行副本。

靶车 I²C 接线和烧录记录位于 `02_TargetCarrier -geek-I2C\任务`。工程内的部分历史文档仍使用整理前的目录名称，查找时请以当前文件夹为准。

不同分支的固件、接线和上位机版本应配套使用。软件构建或烧录校验记录不能代替实物通信和运动验证。
