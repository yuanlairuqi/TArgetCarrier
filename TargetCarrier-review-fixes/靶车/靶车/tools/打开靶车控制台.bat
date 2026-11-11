@echo off
rem ============================================================
rem  TargetCar control GUI launcher (double-click to open)
rem  Uses system Python 3.12 (has tkinter). pyserial is borrowed
rem  from PlatformIO's penv if the system Python lacks it.
rem ============================================================
chcp 65001 >nul
setlocal

set "PY=%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
set "PYW=%LOCALAPPDATA%\Programs\Python\Python312\pythonw.exe"

if not exist "%PY%" set "PY=python"
if not exist "%PYW%" set "PYW="

if exist "%PYW%" (
    start "" "%PYW%" "%~dp0dm_gui.py" %*
) else (
    "%PY%" "%~dp0dm_gui.py" %*
)
