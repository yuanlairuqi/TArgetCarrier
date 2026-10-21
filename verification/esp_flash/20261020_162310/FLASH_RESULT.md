# 主工程 ESP32-S3 烧录记录

时间：2026-10-20 16:23（Asia/Shanghai）。范围仅 C:/Users/y/Desktop/TargetCarrier/x轴/esp。

## 烧录结果

- 设备：COM13，ESP32-S3-PICO-1 rev0.2，MAC 7c:e8:b1:e5:1d:ac，实际Flash 8MB、PSRAM 8MB。
- Secure Boot、Flash Encryption 均关闭；本次未修改eFuse。
- 使用本次功能检查中通过构建的主工程固件，烧录前逐文件确认主工程源码仍与构建快照一致；不是副本的灯效固件。
- 使用工程原有4MB分区/镜像头配置，DIO、80MHz；实际8MB已完整备份。
- 写入地址：bootloader 0x0000，分区表0x8000，应用0x10000。
- esptool写入校验、独立verify-flash三段校验通过；另读回前565248字节，三段逐字节完全一致。
- 回读范围内未涉及的扇区与备份一致，未执行整片擦除。
- 原8MB备份：C:\Users\y\Desktop\TargetCarrier\_backups\esp32_flash_20261020_162310\flash_before_8MB.bin
- 备份SHA-256：c84dc4b70c06c1a7c8ef5f7645faee290e51ad588b9e3e4544021b02595f34e8

## 启动与功能边界

用户已确认单击PWR复位。随后捕获536条持续GATT通知发起记录，状态句柄18及采样句柄21均有活动。结合当前应用仅在已连接且订阅后发出通知的代码路径，确认应用运行及BLE订阅连接；不等于已确认手机界面显示或每包送达。最早的Ready和UART初始化日志未捕获，实际STM32 UART ACK仍未验证。

主工程启用BLE、UART1 G4 TX/G5 RX（115200 8N1）和G6/G7脉冲采样；ESP-NOW仍关闭。启动时会向STM32发送STOP握手。STM32连续运动开关不因本次S3烧录而改变。本次未确认手机端显示或通知逐包送达，未验证STM32 UART ACK或电机动作。

主工程源码未修改。日志和逐段SHA-256见同目录JSON及log文件；设备原始Flash备份放在Git忽略的_backups中，不应上传。
