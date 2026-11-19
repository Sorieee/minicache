/* test_util.c —— util 模块单元测试(M0)
 *
 * 用临时文件重定向 stdout 捕获日志输出,验证:
 * 级别过滤、行格式(时间戳/级别标签)、参数格式化、超长消息截断。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static char captured[16384];

/* 把 stdout 重定向到临时文件;glibc 的 stdout 是可赋值的 FILE* 变量 */
static FILE *capture_start(void)
{
    fflush(stdout);
    FILE *f = tmpfile();
    if (f == NULL) {
        perror("tmpfile");
        exit(1);
    }
    stdout = f;
    return f;
}

/* 恢复 stdout,返回捕获到的内容(存于全局 captured) */
static const char *capture_stop(FILE *f)
{
    fflush(stdout);
    stdout = stderr; /* 还原:glibc 中 stderr 与初始 stdout 同为标准流 */
    rewind(f);
    size_t n = fread(captured, 1, sizeof(captured) - 1, f);
    captured[n] = '\0';
    fclose(f);
    return captured;
}

/* 默认级别 INFO:DEBUG 被过滤,其余正常输出且带级别标签 */
static void test_level_filter(void)
{
    log_set_level(LOG_LEVEL_INFO);

    FILE *f = capture_start();
    LOGD("debug-should-not-appear");
    LOGI("info-should-appear");
    LOGW("warn-should-appear");
    LOGE("error-should-appear");
    const char *out = capture_stop(f);

    CHECK(strstr(out, "debug-should-not-appear") == NULL);
    CHECK(strstr(out, "info-should-appear") != NULL);
    CHECK(strstr(out, "warn-should-appear") != NULL);
    CHECK(strstr(out, "error-should-appear") != NULL);
    CHECK(strstr(out, "[INFO ]") != NULL);
    CHECK(strstr(out, "[WARN ]") != NULL);
    CHECK(strstr(out, "[ERROR]") != NULL);
}

/* DEBUG 级别:DEBUG 行出现且带源码位置;时间戳符合 [YYYY-MM-DD HH:MM:SS.mmm] */
static void test_debug_format(void)
{
    log_set_level(LOG_LEVEL_DEBUG);

    FILE *f = capture_start();
    LOGD("hello-debug");
    const char *out = capture_stop(f);

    CHECK(strstr(out, "[DEBUG]") != NULL);
    CHECK(strstr(out, "hello-debug") != NULL);
    CHECK(strstr(out, "(test_util.c:") != NULL);

    int y, mo, d, h, mi, s, ms;
    char lvl[8];
    int got = sscanf(out, "[%4d-%2d-%2d %2d:%2d:%2d.%3d] [%5[^]]]",
                     &y, &mo, &d, &h, &mi, &s, &ms, lvl);
    CHECK(got == 8);
    CHECK(y >= 2026);
    CHECK(mo >= 1 && mo <= 12);
    CHECK(d >= 1 && d <= 31);
    CHECK(h >= 0 && h <= 23);
    CHECK(mi >= 0 && mi <= 59);
    CHECK(s >= 0 && s <= 60); /* 闰秒 */
    CHECK(ms >= 0 && ms <= 999);
    CHECK(strcmp(lvl, "DEBUG") == 0);

    log_set_level(LOG_LEVEL_INFO);
}

/* printf 风格参数被正确格式化 */
static void test_format_args(void)
{
    FILE *f = capture_start();
    LOGI("port=%d bind=%s ready=%d", 6380, "127.0.0.1", 1);
    const char *out = capture_stop(f);

    CHECK(strstr(out, "port=6380 bind=127.0.0.1 ready=1") != NULL);
}

/* 超长消息被截断为一条完整日志行,不崩溃 */
static void test_truncation(void)
{
    char big[4000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    FILE *f = capture_start();
    LOGI("%s", big);
    const char *out = capture_stop(f);

    CHECK(strlen(out) < sizeof(big)); /* 远小于 4000,说明确实截断 */
    CHECK(strchr(out, '\n') != NULL); /* 仍是完整一行 */
    CHECK(strstr(out, "truncated") != NULL);
}

int main(void)
{
    test_level_filter();
    test_debug_format();
    test_format_args();
    test_truncation();
    printf("test_util: all checks passed\n");
    return 0;
}
