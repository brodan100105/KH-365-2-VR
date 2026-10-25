# The platform layer (DS hardware stand-ins) and the guest it runs: the decompiled game as C
# from guest/build_guest.py (KHVR_GUEST_DIR).
cmake_minimum_required(VERSION 3.22)
project(khvr_platform C CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(KHVR_GUEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/guest/wasm2c" CACHE PATH
    "wasm2c output of guest/build_guest.py")
if(NOT EXISTS "${KHVR_GUEST_DIR}/khdays.h")
    message(FATAL_ERROR "No guest in ${KHVR_GUEST_DIR}: run guest/build_guest.py first")
endif()

file(GLOB KHVR_GUEST_SOURCES "${KHVR_GUEST_DIR}/*.c")
add_library(khdays_guest STATIC ${KHVR_GUEST_SOURCES}
    third_party/wasm2c/wasm-rt-impl.c third_party/wasm2c/wasm-rt-exceptions-impl.c)
target_include_directories(khdays_guest PUBLIC "${KHVR_GUEST_DIR}" third_party/wasm2c)
# Bounds checks instead of guard pages (no signal handlers to install on Android), and room for
# the guest's deep call chains.
target_compile_definitions(khdays_guest PUBLIC
    WASM_RT_MEMCHECK_BOUNDS_CHECK=1 WASM_RT_MEMCHECK_GUARD_PAGES=0 WASM_RT_MAX_CALL_STACK_DEPTH=20000)
# The generated code is always optimized, even in debug builds of the app.
target_compile_options(khdays_guest PRIVATE -O2 -g0 -w -fno-strict-aliasing)

add_library(khvr_platform STATIC
    src/backtrace.cpp
    src/guest.cpp
    src/hle_arm7.cpp
    src/hle_gx.cpp
    src/hle_io.cpp
    src/hle_rom.cpp
    src/hle_thread.cpp
    src/log.cpp
    src/mobiclip_tables_0.c
    src/mobiclip_tables_1.c
    src/movie.cpp
    src/platform.cpp
    src/render2d.cpp
    src/snd_calc_volume.c
    src/snd_tables.c
    src/sound.cpp
    src/sound_seq.cpp
)
target_include_directories(khvr_platform PUBLIC include PRIVATE src)
# Movie pictures: the decomp's portable MobiClip decoder and its tables (movie.cpp).
set(KHVR_DECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
target_sources(khvr_platform PRIVATE "${KHVR_DECOMP_DIR}/libs/mobiclip/video/portable/mobiclip_reference.cpp")
target_include_directories(khvr_platform PRIVATE "${KHVR_DECOMP_DIR}/libs/mobiclip/video/portable")
set_source_files_properties(src/mobiclip_tables_0.c src/mobiclip_tables_1.c src/snd_tables.c src/snd_calc_volume.c PROPERTIES
    INCLUDE_DIRECTORIES "${KHVR_DECOMP_DIR}/include" COMPILE_OPTIONS "-w")
set_source_files_properties("${KHVR_DECOMP_DIR}/libs/mobiclip/video/portable/mobiclip_reference.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(khvr_platform PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(khvr_platform PUBLIC khdays_guest Threads::Threads)
if(ANDROID)
    target_link_libraries(khvr_platform PUBLIC log)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Desktop runner: boots a ROM headless and reports how far the game gets.
    add_executable(khdays_run tools/khdays_run.cpp)
    target_link_libraries(khdays_run PRIVATE khvr_platform)
endif()
