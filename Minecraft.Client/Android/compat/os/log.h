#pragma once

#include <android/log.h>

typedef void *os_log_t;
#define OS_LOG_DEFAULT ((os_log_t)0)
#define os_log(log, ...) __android_log_print(ANDROID_LOG_INFO, "mcle", __VA_ARGS__)
#define os_log_error(log, ...) __android_log_print(ANDROID_LOG_ERROR, "mcle", __VA_ARGS__)
