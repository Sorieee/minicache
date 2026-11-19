#ifndef UTIL_H
#define UTIL_H

/*
 * util.h —— 通用工具:分级日志、OOM 处理(M0)
 *
 * 日志输出到 stdout(docs/design.md §10),行格式:
 *   [2026-11-16 18:20:33.123] [INFO ] mini-cache starting...
 * DEBUG 级别额外带源码位置,便于定位:
 *   [2026-11-16 18:20:33.123] [DEBUG] conn accepted (net.c:57)
 *
 * 默认级别 INFO,可用 log_set_level() 调整。
 */

#include <stdio.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

/* 调整全局日志级别:低于该级别的日志被忽略;返回旧级别 */
log_level_t log_set_level(log_level_t level);

/* 各 LOGx 宏的底层实现:带毫秒时间戳与级别。
 * format 属性让 -Wall 在编译期检查 printf 参数。 */
void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* 内存分配失败:打印日志并终止进程(与 Redis 同款取舍,见 docs/design.md §8) */
void oom(const char *what);

/* 带时间戳与级别的日志宏;调用方不必写换行符与 %n 参数 */
#define LOGD(...) log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOGI(...) log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOGW(...) log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOGE(...) log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif /* UTIL_H */
