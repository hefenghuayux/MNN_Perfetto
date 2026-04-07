@echo off
setlocal

:retry
echo [INFO] Stopping ADB server before shell attempt...
adb kill-server

echo [INFO] Trying to enter adb shell...
adb shell exit >nul 2>&1

if %errorlevel%==0 (
    echo [INFO] Stopping ADB server before interactive shell...
    adb kill-server
    echo [INFO] Device is ready. Entering adb shell...
    adb shell
    goto :eof
)

echo [INFO] Not ready yet. Retrying in 5 seconds...
timeout /t 5 /nobreak >nul
goto retry
