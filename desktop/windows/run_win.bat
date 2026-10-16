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

:: Optional GUI (sync_gui.exe): same ImGui+SDL2 panel as Linux, driving the
:: engine above over the --ui pipe. Needs SDL2 from vcpkg:
::   vcpkg install sdl2 --triplet x64-windows
:: then pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake
:: below. Skipped silently when SDL2 is absent - the console engine above
:: is the required piece; the GUI is a convenience monitor.
:: Our own --no-gui flag may appear anywhere in %*; strip it into
:: ENGINE_ARGS so the engine/GUI never sees it.
set "WANT_GUI=1"
set "ENGINE_ARGS="
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--no-gui" ( set "WANT_GUI=" ) else (
    if defined ENGINE_ARGS ( set "ENGINE_ARGS=%ENGINE_ARGS% %~1" ) else ( set "ENGINE_ARGS=%~1" )
)
shift
goto parse_args
:args_done
if defined WANT_GUI (
    where cmake >nul 2>&1
    if not errorlevel 1 (
        if defined VCPKG_ROOT (
            set "TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
        ) else (
            set "TOOLCHAIN=%ProgramFiles%\vcpkg\scripts\buildsystems\vcpkg.cmake"
        )
        if exist "%TOOLCHAIN%" (
            echo [run_win] building sync_gui.exe (SDL2 GUI) ...
            cmake -S "%~dp0\.." -B "%~dp0\..\build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" >nul
            cmake --build "%~dp0\..\build" --config Release --target sync_gui
            if errorlevel 1 (
                echo [run_win] GUI build failed - engine still OK, continuing without GUI.
            ) else (
                copy /y "%~dp0\..\build\Release\sync_gui.exe" "%~dp0sync_gui.exe" >nul 2>&1
                copy /y "%~dp0\..\build\sync_gui.exe" "%~dp0sync_gui.exe" >nul 2>&1
            )
        ) else (
            echo [run_win] vcpkg toolchain not found - skipping sync_gui.exe.
            echo           (install vcpkg + "vcpkg install sdl2 --triplet x64-windows", then rerun)
        )
    ) else (
        echo [run_win] cmake not on PATH - skipping sync_gui.exe (engine still built).
    )
)

echo.
if exist "%~dp0sync_gui.exe" (
    echo [run_win] launch: sync_gui.exe (GUI, owns the engine as a --ui child)
    if defined ENGINE_ARGS ( "%~dp0sync_gui.exe" %ENGINE_ARGS% ) else ( "%~dp0sync_gui.exe" )
) else (
    echo [run_win] launch: pipewire_sync_win.exe (console; pass --no-gui to skip the GUI attempt)
    if defined ENGINE_ARGS ( pipewire_sync_win.exe %ENGINE_ARGS% ) else ( pipewire_sync_win.exe )
)
exit /b %ERRORLEVEL%

