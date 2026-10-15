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
target_compile_options(sync_selftest PRIVATE -Wall -Wextra)
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
    target_link_libraries(pipewire_sync_win PRIVATE ole32 oleaut32 uuid winmm)
    # Companion diagnostic: dumps every audio-endpoint property, used to
    # find which key identifies Bluetooth devices.
    add_executable(diag_props_win windows/diag_props.cpp)
    target_link_libraries(diag_props_win PRIVATE ole32)
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

# sample is a throwaway capture sanity check from early bring-up.
add_executable(sample pipewire/sample.cpp)

# ---------------------------------------------------------------------------
# sync_gui: standalone Dear ImGui monitor/control panel (Linux).
#
# Owns pipewire_sync as a `--ui` child and talks to it over pipes
# (desktop/ui_protocol.h). Guarded on SDL2 + GL so a minimal configure
# still succeeds - without them only the engine/tools build.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
    pkg_check_modules(GL QUIET IMPORTED_TARGET gl)
endif()
if(SDL2_FOUND AND GL_FOUND AND EXISTS ${IMGUI_DIR}/imgui.cpp)
    add_executable(sync_gui
        gui/sync_gui.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
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
