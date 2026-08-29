@echo off
REM Double-click to run the current debug build.
REM Quit from the tray icon, or close this window.
setlocal
set EXE=%~dp0build\debug\liquidock.exe
if not exist "%EXE%" (
    echo Not built yet. Run scripts\build.ps1 first:
    echo     powershell -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"
    pause
    exit /b 1
)
taskkill /IM liquidock.exe /F >nul 2>&1
start "" "%EXE%" %*
