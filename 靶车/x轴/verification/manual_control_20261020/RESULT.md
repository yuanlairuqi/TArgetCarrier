# X轴手动连续运动交付

2026-10-20。仅修改主 TargetCarrier 的X轴及相关说明；ESP源码和协议不变，副本和靶车未操作。

- STM32：FreeRTOS V11.2.0，连续运动宏=1，上电定量动作宏=0，Debug/Release均构建通过；核验最终源文件和产物SHA-256、复位及RTOS向量、编译宏。
- Android：3.3-targetcarrier-xaxis，versionCode=6，原应用ID及调试签名保留；v1/v2/v3签名验证、zipalign、APK元数据检查通过。
- 工程当前源码运行25组回归通过，包括28个硬件确认断言、运动ACK不一致／故障／方向／速度／模式／阶段不匹配、STOP、断连、超时、RTOS启动路径及解码兼容性。
- Release固件Flash13716B，RAM5232B；Debug Flash24672B，RAM5240B。
- 未烧录本轮固件、未安装手机、未验证实际连续电机运动。APPLIED代表STM32软件输出状态，不是电机位置或实测转速。ENA仍未配置。

交付文件在 ../../交付文件；使用说明在 ../../README.md。修改前备份路径见 DEPLOYMENT.json。STM32构建使用ASCII源码快照，具体来源、保留路径及逐文件哈希见各BUILD_RESULT.json。
