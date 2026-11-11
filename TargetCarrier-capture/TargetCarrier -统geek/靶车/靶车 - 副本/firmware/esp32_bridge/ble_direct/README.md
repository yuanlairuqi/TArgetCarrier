# 靶车桥接 BLE 独立版

使用 ESP-IDF 6.1 构建此目录，保留旁边 firmware/lib 共享库的相对层级。
服务 6e410001-b5a3-f393-e0a9-e50e24dcca9e，广播 TC-Target。
UART TX=GPIO1 RX=GPIO2，921600，沿用原 STM32/CAN 协议。
这是新独立 IDF 工程，不使用上层旧 Arduino/ESP-NOW main.cpp。完整接线、烧录和验证范围见交付根目录使用说明.md。
