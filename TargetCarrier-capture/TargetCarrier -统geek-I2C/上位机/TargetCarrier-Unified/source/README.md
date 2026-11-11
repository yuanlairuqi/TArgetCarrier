# TargetCarrier 统一上位机 4.2

Vue + Python，保留原 USB/BLE x 轴、HTTP 靶车及 TC/2 调参入口。新增两条独立程序线：各自顺序执行，可并行运动并在运行中追加或编辑待执行模块。单独停止只中止本线，故障/失焦/全部停止联停两条。

调度逻辑位于 linear_program.py，双线协调位于 unified.py；详见上一级 使用说明.md。使用 build-unified.ps1 打包，验证命令：python -m unittest discover -s tests -v。构建不代表实机通过。
