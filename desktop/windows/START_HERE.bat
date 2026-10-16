@echo off
chcp 65001 >nul
cd /d "%~dp0"

if not exist "pipewire_sync_win.exe" (
    echo.
    echo  pipewire_sync_win.exe not found in this folder.
    echo  Did you unzip the whole folder? Every file must sit next to this file.
    echo  (Easiest: run SyncWifi-Setup-1.0.0.exe from the Releases page - it
    echo  installs everything and adds Start Menu / Desktop shortcuts.)
    echo.
    pause
    exit /b 1
)

:: Prefer the GUI when it shipped alongside the engine (same folder, built
:: by run_win.bat when SDL2/vcpkg is present, or attached to a Release).
:: It owns pipewire_sync_win.exe as a --ui child and shows the per-device
:: table + log panel; the console engine below is the same audio with
:: keyboard controls instead of buttons.
if exist "sync_gui.exe" (
    echo  Starting the visual monitor (sync_gui.exe) ...
    echo  (self-contained single exe, no DLLs needed;
    echo   to use the black console window instead, run run_win.bat --no-gui)
    echo.
    sync_gui.exe %*
    echo.
    echo  (the program has stopped - speakers/volume restored)
    pause
    exit /b 0
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
echo     (Tip: build sync_gui.exe via run_win.bat for a proper GUI.)
echo     Your speakers and volume go back to normal afterwards.
echo.
echo ============================================================
echo.
pipewire_sync_win.exe %*
echo.
echo  (the program has stopped - speakers/volume restored)
pause
