@echo off
rem ============================================================
rem  TargetCar desktop app (Vue dashboard in an app window)
rem  Double-click to open. Needs python + pyserial (borrowed from
rem  PlatformIO penv when the system Python lacks it).
rem ============================================================
chcp 65001 >nul
setlocal
set "PYW=%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe"
if not exist "%PYW%" set "PYW=pythonw"
start "" "%PYW%" "%~dp0tc_app.py" %*