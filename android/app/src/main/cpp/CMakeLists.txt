cmake_minimum_required(VERSION 3.22.1)
project(syncwifi)

# Reuses the PROVEN prototype engine: protocol/sync_wifi.h (ring + drift PI
# controller, same math as desktop/pipewire/pipewire_sync.cpp). This
# CMakeLists lives in app/src/main/cpp/ and includes the header from the
# repo-level protocol/ directory — the single source of truth for the wire
# format, shared verbatim with the desktop reference implementations.
add_library(native-lib SHARED native-lib.cpp)

target_include_directories(native-lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../protocol)

find_library(log-lib log)
target_link_libraries(native-lib ${log-lib})
