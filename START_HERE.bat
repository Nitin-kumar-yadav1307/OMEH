@echo off
chcp 65001 >nul
cd /d "%~dp0"

if not exist "pipewire_sync_win.exe" (
    echo.
    echo  pipewire_sync_win.exe not found in this folder.
    echo  Did you unzip the whole folder? The .exe must sit next to this file.
    echo.
    pause
    exit /b 1
)

echo ============================================================
echo   pipewire_sync - plays the same audio on BOTH earbuds
echo ============================================================
echo.
echo  1. Make sure BOTH Bluetooth earbuds are connected
echo     (Settings ^> Bluetooth ^> they should say "Connected").
echo.
echo  2. This window will show a status line every few seconds.
echo.
echo  3. To STOP, press q in this window (or close it).
echo     Your speakers and volume go back to normal afterwards.
echo.
echo ============================================================
echo.
pipewire_sync_win.exe %*
echo.
echo  (the program has stopped - speakers/volume restored)
pause
