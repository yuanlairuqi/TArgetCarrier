# 两台 ESP32-S3 烧录说明（本次未执行）

先关闭所有占用串口和 BLE 的软件。电机驱动电源关闭，断开驱动器 PUL/DIR/ENA，避免调试期间动作。
一次只接待刷的一块 ESP。必要时按住 BOOT，点按 RESET 后松开 BOOT；烧录完成按 RESET。
下面 COMxx 改成实际端口。两固件编译布局均为 4MB / DIO / 80MHz，先用 flash-id 确认实际芯片为 ESP32-S3 且 Flash 容量至少 4MB；板卡和外设引脚需与实际硬件一致。
以 x 轴为例，在 PowerShell 执行：

```powershell
$espPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
$espPort = 'COMxx'
$fwDir = 'D:\TargetCarrier-Parallel\firmware\dual-ble-5.0\x'
& $espPython -m esptool --chip esp32s3 --port $espPort flash-id
# 确认芯片/容量后备份前4MB（更大Flash需按实测容量备份），保存成功再刷。
& $espPython -m esptool --chip esp32s3 --port $espPort read-flash 0 0x400000 "$fwDir\before-x-4MB.bin"
# 检查上一条 exit code 为0且备份大小4194304字节。
& $espPython -m esptool --chip esp32s3 --port $espPort --baud 460800 write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x0 "$fwDir\bootloader\bootloader.bin" 0x8000 "$fwDir\partition_table\partition-table.bin" 0x10000 "$fwDir\esp32_robot.bin"
& $espPython -m esptool --chip esp32s3 --port $espPort verify-flash 0x0 "$fwDir\bootloader\bootloader.bin" 0x8000 "$fwDir\partition_table\partition-table.bin" 0x10000 "$fwDir\esp32_robot.bin"
```

靶车桥接 ESP（更换为它的 COM 口，不能把靶车固件刷到 x 轴）：

```powershell
$espPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
$espPort = 'COMxx'
$fwDir = 'D:\TargetCarrier-Parallel\firmware\dual-ble-5.0\target'
& $espPython -m esptool --chip esp32s3 --port $espPort flash-id
& $espPython -m esptool --chip esp32s3 --port $espPort read-flash 0 0x400000 "$fwDir\before-target-4MB.bin"
# 确认备份成功再执行以下两条。
& $espPython -m esptool --chip esp32s3 --port $espPort --baud 460800 write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m 0x0 "$fwDir\bootloader\bootloader.bin" 0x8000 "$fwDir\partition_table\partition-table.bin" 0x10000 "$fwDir\target_ble_bridge.bin"
& $espPython -m esptool --chip esp32s3 --port $espPort verify-flash 0x0 "$fwDir\bootloader\bootloader.bin" 0x8000 "$fwDir\partition_table\partition-table.bin" 0x10000 "$fwDir\target_ble_bridge.bin"
```

不要执行 erase-flash；本次三段写入不主动擦除NVS，首次迁移前仍应备份。每条命令失败就停止，先排查再继续。
写入/校验成功只证明 Flash 内容，之后仍须分别验证广播、连接、STM32新鲜状态、停止确认，再由用户安排低速电机验证。
