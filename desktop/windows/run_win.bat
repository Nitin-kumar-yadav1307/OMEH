@echo off
setlocal
cd /d "%~dp0"

:: MSVC (Developer Command Prompt) is required: win_config.h compiles
:: against Windows SDK headers (<mmdeviceapi.h>, <audioclient.h>,
:: <endpointvolume.h>, <propsys.h>) that MinGW/gcc does NOT ship.
:: If cl.exe is not already on PATH, locate and initialise the VS env.
where cl.exe >nul 2>&1 || (
    for /f "tokens=*" %%I in (
        "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
        -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
        -find "VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if exist "%%I" call "%%I"
    )
)

if not exist "pipewire_sync_win.exe" (
    where cl.exe >nul 2>&1
    if errorlevel 1 (
        echo.
        echo  [run_win] MSVC not found.
        echo  Install "Build Tools" or "Desktop development with C++"
        echo  from the Visual Studio Installer / https://visualstudio.microsoft.com/downloads/
        echo  Then rerun this file.
        echo.
        exit /b 1
    )
)

echo [run_win] building pipewire_sync_win.exe ...
cl /nologo /W4 /O2 /std:c++17 /DNDEBUG ^
    pipewire_sync_win.cpp ^
    /link ole32.lib oleaut32.lib uuid.lib winmm.lib ^
    /Fe:pipewire_sync_win.exe
if errorlevel 1 (
    echo [run_win] BUILD FAILED
    exit /b 1
)

echo.
echo [run_win] launch: pipewire_sync_win.exe
pipewire_sync_win.exe %*
exit /b %ERRORLEVEL%

