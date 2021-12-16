#pragma once

#include "conn.h"
#include <stdio.h>

#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "[%s] Check failed: %s\n", #cond); \
            exit(1);                                           \
        }                                                      \
    } while (0)

enum LOG_LEVEL {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
};

#ifndef NDEBUG
#define DEBUG(...) \
    vhost_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#define DEBUG(...)
#endif
#define INFO(...) \
    vhost_log(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define WARN(...) \
    vhost_log(LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define ERROR(...) \
    vhost_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define FALTA(...) \
    vhost_log(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

void DumpHex(const void *data, size_t size);

void vhost_log(enum LOG_LEVEL level, const char *file, const int line,
               const char *func, const char *fmt, ...);