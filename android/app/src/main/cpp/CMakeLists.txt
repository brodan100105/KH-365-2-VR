cmake_minimum_required(VERSION 3.22)
project(khdaysvr C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The platform-independent VR core (settings, rigs, combat, demo game), shared with the tests.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core ${CMAKE_BINARY_DIR}/khvr_core)

# android_native_app_glue ships with the NDK.
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

# The Khronos OpenXR loader comes from Maven Central as a prefab package (see app/build.gradle).
find_package(OpenXR REQUIRED CONFIG)

add_library(khdaysvr SHARED
    main.cpp
    renderer.cpp
    vr_app.cpp
    xr_platform.cpp
)
# {XR_TYPE_...} initializers leave `next` to zero on purpose (the OpenXR idiom).
target_compile_options(khdaysvr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
# NativeActivity looks up ANativeActivity_onCreate, which the glue defines; keep it exported.
set_target_properties(khdaysvr PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
target_link_libraries(khdaysvr PRIVATE
    khvr_core
    native_app_glue
    OpenXR::openxr_loader
    android
    EGL
    GLESv3
    log
)
