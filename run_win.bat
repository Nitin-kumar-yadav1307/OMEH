@echo off
setlocal
cd /d "%~dp0"

rem --- find g++: use PATH (MSYS2 ucrt64 shell), fall back to common install ---
where g++ >nul 2>&1
if errorlevel 1 (
    if exist "C:\msys64\ucrt64\bin\g++.exe" (
        set "PATH=C:\msys64\ucrt64\bin;%PATH%"
    ) else if exist "C:\msys64\mingw64\bin\g++.exe" (
        set "PATH=C:\msys64\mingw64\bin;%PATH%"
    ) else (
        echo [run_win] g++ not found. Install MSYS2 ucrt64 or run from its shell.
        exit /b 1
    )
)

echo [run_win] building pipewire_sync_win.exe ...
g++ -O2 -std=c++17 -Wall pipewire_sync_win.cpp -o pipewire_sync_win.exe ^
    -lole32 -loleaut32 -luuid -lwinmm
if errorlevel 1 (
    echo [run_win] build FAILED
    exit /b 1
)

echo [run_win] starting: pipewire_sync_win.exe %*
echo.
pipewire_sync_win.exe %*
exit /b %ERRORLEVEL%
