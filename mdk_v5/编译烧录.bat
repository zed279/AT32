@echo off
cd /d "C:\Users\zed\Desktop\AT32F403A_Project\mdk_v5"

echo [1/3] Checking Keil uVision...
tasklist /FI "IMAGENAME eq UV4.exe" 2>nul | findstr /I "UV4.exe" >nul
if not errorlevel 1 (
    echo [!] Keil uVision is running. Please CLOSE it first, then run this script again.
    pause
    exit /b 1
)

echo [2/3] Building with UV4...
"C:\Keil_v5\UV4\UV4.exe" -b AT32F403A_Project.uvprojx -j0 -o build.log
if errorlevel 2 (
    echo [X] BUILD FAILED - see build.log
    pause
    exit /b 2
)
echo [OK] Build finished.

echo [3/3] Flashing with probe-rs (DAP-Link)...
"C:\Users\zed\Desktop\工程化工具链\probe-rs\probe-rs.exe" run --chip AT32F403AVGT7 objects\template.axf
if errorlevel 1 (
    echo [X] FLASH FAILED
    pause
    exit /b 3
)
echo [OK] Flashed and running.
pause
exit /b 0
