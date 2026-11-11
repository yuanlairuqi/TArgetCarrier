@echo off
rem 诊断版 6.1.3（写耗时/通知速率/ACK 停滞/EXT1-3 计数）。
rem 说明：工作区目录树带有 "Low Mandatory Level" 完整性标签（代理沙箱遗留），
rem pythonnet/.NET Framework 从该目录加载 Python.Runtime.dll 会失败
rem （Failed to resolve Python.Runtime.Loader.Initialize），与本项目代码无关；
rem 旧版 6.1.2 同样现象。因此统一从运行副本启动（与现有 启动*.cmd 一致）：
setlocal
set "RUNTIME=C:\Users\y\Documents\Codex\TargetCarrier-runtime-20261107\diagnosis"
if not exist "%RUNTIME%\TargetCarrier-Capture.exe" (
  echo 未找到诊断版运行副本：%RUNTIME%\TargetCarrier-Capture.exe
  echo 请把 上位机\TargetCarrier-Unified\app-diagnosis-20261107 复制到该目录后再启动。
  pause
  exit /b 1
)
rem A 组对照：取消下面一行的注释（或在命令行里 set）即可关闭 PULSE 订阅
rem set "TC_PULSE_SUBSCRIBE=off"
start "" "%RUNTIME%\TargetCarrier-Capture.exe"
endlocal
