# TargetCarrier X轴 Android 控制端

版本 3.3，应用 ID 保持 `cn.robot.ble.wildfire`。

点击正转／反转后连续运行，停止按钮结束运行；切换到后台或断连后停止控制。界面只在本次连接中已发送的指令、ESP 接收序号、STM32 回报序号、方向、速度和 APPLIED 标志一致时显示输出已确认。刚连接先发 STOP；演示、离线、故障及连续功能关闭时锁定运动。

速度百分比是脉冲频率指令，默认 1–100% 对应100–1000Hz，0%停止。没有编码器反馈，输出确认不表示电机实际转速、位置或到位。

## 构建

```powershell
./build-apk.ps1 -SdkPath 'C:/path/to/android-sdk' -JavaPath 'D:/uu'
```

需要 JDK17、Android35平台和35.0.0构建工具。也保留 Gradle 工程配置。手动脚本产物为 `build/TargetCarrier-XAxis-v3.3-debug.apk`。本机已保留原调试签名到忽略的 `build/robot-debug.jks`，可覆盖安装旧APK；该密钥不进入版本库。首次从全新克隆构建会生成另一把调试签名，覆盖安装需使用原密钥或先卸载旧应用。

交付 APK 在上级 `交付文件`。测试运行方式见上级 README。tests/fixtures 为现有 ESP C 协议测试产生的遥测和脉冲数据样本；用于验证 Android 解码的兼容性。
