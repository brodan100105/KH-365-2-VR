# Platform-independent VR core: settings, view rigs, motion combat, input mapping.
# Built into the Quest app and, on a desktop, into the unit tests.
cmake_minimum_required(VERSION 3.22)
project(khvr_core CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(khvr_core STATIC
    src/canvas.cpp
    src/combat.cpp
    src/demo_game.cpp
    src/input_mapper.cpp
    src/settings.cpp
    src/settings_panel.cpp
    src/view_rig.cpp
)
target_include_directories(khvr_core PUBLIC include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(khvr_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(khvr_core_tests tests/core_tests.cpp)
    target_link_libraries(khvr_core_tests PRIVATE khvr_core)
    add_test(NAME khvr_core_tests COMMAND khvr_core_tests)
    add_executable(khvr_settings_doc tools/settings_doc.cpp)
    target_link_libraries(khvr_settings_doc PRIVATE khvr_core)
endif()
