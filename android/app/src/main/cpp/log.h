#pragma once

#include <android/log.h>

#define KHVR_TAG "KHVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, KHVR_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, KHVR_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, KHVR_TAG, __VA_ARGS__)
