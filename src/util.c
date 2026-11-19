/* util.c —— 通用工具实现(M0) */

#include "util.h" /* 自有头文件最先包含,保证其自洽(不依赖别的头先被包含) */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static log_level_t g_level = LOG_LEVEL_INFO;

static const char *level_names[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

log_level_t log_set_level(log_level_t level)
{
    log_level_t old = g_level;
    if (level >= LOG_LEVEL_DEBUG && level <= LOG_LEVEL_ERROR)
        g_level = level;
    return old;
}

void log_write(log_level_t level, const char *file, int line,
               const char *fmt, ...)
{
    static const char trunc[] = " ...(truncated)";

    if (level < g_level)
        return;
    if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_ERROR)
        level = LOG_LEVEL_INFO; /* 非法级别按 INFO 处理,防止越界取 level_names */

    /* 毫秒级本地时间戳 */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    /* 消息体最长 1024 字节,超长截断 —— 日志行不应无限长 */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0)
        msg[0] = '\0';
    else if ((size_t)n >= sizeof(msg))
        memcpy(msg + sizeof(msg) - sizeof(trunc), trunc, sizeof(trunc));

    if (level == LOG_LEVEL_DEBUG) {
        /* __FILE__ 可能带目录前缀,只保留文件名 */
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        fprintf(stdout, "[%s.%03d] [%-5s] %s (%s:%d)\n",
                ts, (int)(tv.tv_usec / 1000), level_names[level], msg, base, line);
    } else {
        fprintf(stdout, "[%s.%03d] [%-5s] %s\n",
                ts, (int)(tv.tv_usec / 1000), level_names[level], msg);
    }
}

void oom(const char *what)
{
    LOGE("out of memory: %s", what);
    abort();
}
