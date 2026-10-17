cmake_minimum_required(VERSION 3.16)
project(syncwifi_desktop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Everything here consumes the shared wire format + sync math from
# protocol/sync_wifi.h — the same header the Android NDK build compiles.
# Keeping it in one place is what stops the desktop reference
# implementations from drifting away from the phone app.
set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

# ---------------------------------------------------------------------------
# Protocol tools (cross-platform; used to test a master against a client
# over UDP without any phones involved).
# ---------------------------------------------------------------------------
add_executable(sync_master tools/sync_master.cpp)
add_executable(sync_client tools/sync_client.cpp)
foreach(t sync_master sync_client)
    target_include_directories(${t} PRIVATE ${PROTOCOL_DIR})
endforeach()

# Unit tests for the shared sync engine. Pure math, no I/O, no phones -
# this is what makes the drift/jitter policy debuggable without deploying
# to two handsets. Run it after any change to protocol/.
# ui_protocol.h (engine <-> GUI line protocol) lives in desktop/, so that
# dir is on the include path too - the protocol round-trip tests need it.
add_executable(sync_selftest tools/sync_selftest.cpp)
target_include_directories(sync_selftest PRIVATE ${PROTOCOL_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
# MSVC uses /W4, not -Wall (this CMakeLists also configures on Windows for sync_gui).
if(MSVC)
    target_compile_options(sync_selftest PRIVATE /W4)
else()
    target_compile_options(sync_selftest PRIVATE -Wall -Wextra)
endif()
add_test(NAME sync_selftest COMMAND sync_selftest)
enable_testing()

# ---------------------------------------------------------------------------
# Windows-only targets (WASAPI). Guarded so a Linux/macOS configure still
# succeeds — they are built with MSYS2/MinGW or cross-compiled with zig,
# not from this CMakeLists.
# ---------------------------------------------------------------------------
if(WIN32)
    # NOTE: pipewire_sync_win.cpp is self-contained (own wire format); it does
    # NOT use protocol/sync_wifi.h, so no include dir is needed here.
    add_executable(pipewire_sync_win windows/pipewire_sync_win.cpp)
    target_link_libraries(pipewire_sync_win PRIVATE ole32 oleaut32 uuid winmm mmdevapi)
    # Companion diagnostic: dumps every audio-endpoint property, used to
    # find which key identifies Bluetooth devices.
    add_executable(diag_props_win windows/diag_props.cpp)
    target_link_libraries(diag_props_win PRIVATE ole32)
    if(MSVC)
        # Static CRT for every Windows exe here: run_win.bat and CI compile
        # the engine with cl /MT, so keep the CMake twins identical - a /MD
        # build would drag VCRUNTIME140.dll into whatever folder it lands in,
        # which is exactly what the installer is designed to avoid.
        foreach(t pipewire_sync_win diag_props_win)
            set_property(TARGET ${t} PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endforeach()
    endif()
endif()

# ---------------------------------------------------------------------------
# PipeWire reference implementation (Linux). Needs the PipeWire dev package.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    pkg_check_modules(SPA IMPORTED_TARGET libspa-0.2)
endif()

if(PIPEWIRE_FOUND)
    add_executable(pipewire_sync pipewire/pipewire_sync.cpp)
    target_link_libraries(pipewire_sync PRIVATE PkgConfig::PIPEWIRE PkgConfig::SPA)
    target_compile_options(pipewire_sync PRIVATE -Wall -O2)

    add_executable(pipewire_test pipewire/pipewire_test.cpp)
    target_link_libraries(pipewire_test PRIVATE PkgConfig::PIPEWIRE PkgConfig::SPA)
else()
    message(STATUS "PipeWire not found - skipping pipewire_sync/pipewire_test "
                   "(install libpipewire-0.3-dev to build them)")
endif()

# sample is a throwaway capture sanity check from early Linux bring-up.
# Kept for the POSIX builds that historically compiled it; Windows uses
# run_win.bat / the WIN32 targets above instead.
if(NOT WIN32)
    add_executable(sample pipewire/sample.cpp)
endif()

# ---------------------------------------------------------------------------
# sync_gui: standalone Dear ImGui monitor/control panel (Linux + Windows).
#
# Owns the engine as a `--ui` child and talks to it over pipes
# (desktop/ui_protocol.h): pipewire_sync on Linux, pipewire_sync_win.exe
# on Windows. Guarded on SDL2 + GL so a minimal configure still succeeds
# - without them only the engine/tools build.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)
set(SYNC_GUI_SRCS
    gui/sync_gui.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
if(WIN32)
    # MSVC has no pkg-config; SDL2 comes from vcpkg via the toolchain file.
    # CI and the shipped build pass -DVCPKG_TARGET_TRIPLET=x64-windows-static
    # so SDL2 links IN statically; OpenGL ships with the Windows SDK.
    # Same sources, same protocol.
    find_package(SDL2 CONFIG QUIET)
    find_package(OpenGL QUIET)
    if(SDL2_FOUND AND OpenGL_FOUND AND EXISTS ${IMGUI_DIR}/imgui.cpp)
        add_executable(sync_gui ${SYNC_GUI_SRCS})
        # Each backend includes its own header as "backends/..." (see
        # sync_gui.cpp), so the imgui root must be on the path. SDL2 include
        # dirs + defines come from its imported targets (vcpkg CONFIG package).
        # No SDL2main: sync_gui.cpp defines SDL_MAIN_HANDLED and owns main()
        # itself (SUBSYSTEM:WINDOWS via pragma); SDL2main would pull WinMain.
        target_include_directories(sync_gui PRIVATE ${IMGUI_DIR})
        # Prefer the static SDL2 target (x64-windows-static): one exe, no
        # SDL2.dll beside it - that missing DLL was the #1 field bug.
        # Dynamic triplets simply fall back to SDL2::SDL2 (the DLL import).
        if(TARGET SDL2::SDL2-static)
            set(SYNC_GUI_SDL SDL2::SDL2-static)
        else()
            set(SYNC_GUI_SDL SDL2::SDL2)
        endif()
        target_link_libraries(sync_gui PRIVATE ${SYNC_GUI_SDL} OpenGL::GL)
        target_compile_options(sync_gui PRIVATE /W4 /utf-8)
        # Static CRT, but only for static triplets (x64-windows-static ends
        # in "static"; x64-windows-static-md is still /MD and must not flip).
        # vcpkg builds *-static with /MT - any mismatch here is LNK2038.
        # This is what removes the VCRUNTIME140/MSVCP140 dependency, so the
        # installer ships two files and nothing else.
        if(VCPKG_TARGET_TRIPLET MATCHES "static$")
            set_property(TARGET sync_gui PROPERTY MSVC_RUNTIME_LIBRARY
                         "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    else()
        message(STATUS "SDL2/OpenGL/ImGui missing - skipping sync_gui "
                       "(vcpkg manifest desktop/vcpkg.json; pass "
                       "-DVCPKG_TARGET_TRIPLET=x64-windows-static for a "
                       "self-contained exe)")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
        pkg_check_modules(GL QUIET IMPORTED_TARGET gl)
    endif()
    if(SDL2_FOUND AND GL_FOUND AND EXISTS ${IMGUI_DIR}/imgui.cpp)
        add_executable(sync_gui ${SYNC_GUI_SRCS})
        # Each backend includes its own header as "backends/..." (see
        # sync_gui.cpp), so the imgui root must be on the path; SDL2 comes
        # from the imported target.
        target_include_directories(sync_gui PRIVATE ${IMGUI_DIR})
        target_link_libraries(sync_gui PRIVATE PkgConfig::SDL2 PkgConfig::GL)
        target_compile_options(sync_gui PRIVATE -Wall -O2)
    else()
        message(STATUS "SDL2/GL/ImGui missing - skipping sync_gui "
                       "(install libsdl2-dev + libgl-dev; ImGui is vendored)")
    endif()
endif()
