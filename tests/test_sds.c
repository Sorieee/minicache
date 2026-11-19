/* test_sds.c —— sds 模块单元测试(M0)
 *
 * 验收标准:docs/development-plan.md M0 —— test_sds 全绿、零警告编译。
 * 覆盖:
 *   创建/复制/释放(含 NULL 与空串边界)、追加与覆盖(含二进制安全、
 *   自重叠、len==0)、预分配策略(小串翻倍/大串 +1MB)、惰性释放、
 *   尾部 '\0' 哨兵不变量、随机操作序列对照压测(固定种子可复现)。
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sds.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

/* 占位实现返回 NULL:先在入口拦下,给出指向未实现接口的失败信息,
 * 而不是让后续断言段错误 */
static sds require(sds s, const char *what)
{
    if (s == NULL) {
        fprintf(stderr, "FAIL %s:%d: %s 尚未实现(返回 NULL)\n",
                __FILE__, __LINE__, what);
        exit(1);
    }
    return s;
}

/* ---- 创建与复制 ---- */

/* sdsnewlen:长度、内容、哨兵;空串;init == NULL */
static void test_newlen_basic(void)
{
    sds s = require(sdsnewlen("hello", 5), "sdsnewlen");
    CHECK(sdslen(s) == 5);
    CHECK(memcmp(s, "hello", 5) == 0);
    CHECK(s[5] == '\0'); /* 尾部哨兵 */
    sdsfree(s);

    s = require(sdsnewlen("", 0), "sdsnewlen");
    CHECK(sdslen(s) == 0);
    CHECK(s[0] == '\0');
    sdsfree(s);

    /* init == NULL:长度照设,内容不保证初始化,但哨兵必须在 */
    s = require(sdsnewlen(NULL, 8), "sdsnewlen");
    CHECK(sdslen(s) == 8);
    CHECK(s[8] == '\0');
    sdsfree(s);
}

/* sdsempty / sdsnew / sdsdup:便捷入口与深拷贝独立性 */
static void test_new_and_dup(void)
{
    sds e = require(sdsempty(), "sdsempty");
    CHECK(sdslen(e) == 0);
    CHECK(e[0] == '\0');

    sds a = require(sdsnew("abc"), "sdsnew");
    CHECK(sdslen(a) == 3);
    CHECK(memcmp(a, "abc", 3) == 0);
    CHECK(a[3] == '\0');

    /* sdsnew(NULL) 等价空串 */
    sds n = require(sdsnew(NULL), "sdsnew(NULL)");
    CHECK(sdslen(n) == 0);
    CHECK(n[0] == '\0');

    /* 深拷贝:地址不同,互不影响 */
    sds d = require(sdsdup(a), "sdsdup");
    CHECK(sdslen(d) == 3);
    CHECK(memcmp(d, "abc", 3) == 0);
    CHECK(d != a);
    d[0] = 'x';
    CHECK(a[0] == 'a');

    sdsfree(e);
    sdsfree(a);
    sdsfree(n);
    sdsfree(d);
}

/* sdsfree(NULL) 不得崩溃 */
static void test_free_null(void)
{
    sdsfree(NULL);
}

/* ---- 清空与惰性释放 ---- */

static void test_clear_lazy(void)
{
    sds s = require(sdsnew("hello world"), "sdsnew");
    size_t before = sdslen(s) + sdsavail(s); /* 总分配(头部之外) */

    sdsclear(s);
    CHECK(sdslen(s) == 0);
    CHECK(s[0] == '\0');
    CHECK(sdslen(s) + sdsavail(s) == before); /* 惰性释放:不缩容 */

    /* 清空后的空间仍可复用 */
    s = require(sdscat(s, "hi"), "sdscat");
    CHECK(sdslen(s) == 2);
    CHECK(memcmp(s, "hi", 2) == 0);
    sdsfree(s);
}

/* ---- 追加 ---- */

/* sdscatlen:普通追加、含 '\0' 的字节串、len==0 不解引用、便捷版 */
static void test_catlen(void)
{
    sds s = require(sdsnew("hello"), "sdsnew");
    s = require(sdscatlen(s, " world", 6), "sdscatlen");
    CHECK(sdslen(s) == 11);
    CHECK(memcmp(s, "hello world", 11) == 0);
    CHECK(s[11] == '\0');

    /* 二进制安全:追加的字节可含 '\0' */
    s = require(sdscatlen(s, "\0x", 2), "sdscatlen");
    CHECK(sdslen(s) == 13);
    CHECK(s[11] == '\0');
    CHECK(s[12] == 'x');
    CHECK(s[13] == '\0');

    /* len == 0:t 不应被解引用(NULL 也不行) */
    s = require(sdscatlen(s, NULL, 0), "sdscatlen");
    CHECK(sdslen(s) == 13);

    /* C 串便捷版 */
    s = require(sdscat(s, "!!"), "sdscat");
    CHECK(sdslen(s) == 15);
    CHECK(memcmp(s, "hello world\0x!!", 15) == 0);
    sdsfree(s);
}

/* 多次追加强制反复 realloc:内容不得丢失(返回值必须接住) */
static void test_cat_realloc_chain(void)
{
    sds s = require(sdsempty(), "sdsempty");
    char chunk[1024];
    memset(chunk, 'z', sizeof(chunk));

    for (int i = 0; i < 300; i++) {
        s = require(sdscatlen(s, chunk, sizeof(chunk)), "sdscatlen");
        CHECK(sdslen(s) == (size_t)(i + 1) * sizeof(chunk));
    }
    CHECK(memcmp(s, chunk, sizeof(chunk)) == 0);       /* 开头 */
    CHECK(memcmp(s + sdslen(s) - sizeof(chunk), chunk, sizeof(chunk)) == 0);
    CHECK(s[sdslen(s)] == '\0');
    sdsfree(s);
}

/* ---- 覆盖拷贝 ---- */

/* sdscpylen:变短不缩容、变长触发扩容、自重叠、便捷版与 NULL */
static void test_cpylen(void)
{
    sds s = require(sdsnew("hello world"), "sdsnew");
    size_t alloc_before = sdslen(s) + sdsavail(s);

    /* 覆盖为更短内容:len 更新,总分配不缩 */
    s = require(sdscpylen(s, "abc", 3), "sdscpylen");
    CHECK(sdslen(s) == 3);
    CHECK(memcmp(s, "abc", 3) == 0);
    CHECK(s[3] == '\0');
    CHECK(sdslen(s) + sdsavail(s) == alloc_before);

    /* 覆盖为更长内容:触发扩容,内容完整 */
    char big[128];
    memset(big, 'x', sizeof(big));
    big[0] = 'y';
    s = require(sdscpylen(s, big, sizeof(big)), "sdscpylen");
    CHECK(sdslen(s) == sizeof(big));
    CHECK(memcmp(s, big, sizeof(big)) == 0);
    CHECK(s[sizeof(big)] == '\0');
    sdsfree(s);

    /* 自重叠:t 指向 s 内部,memmove 语义("0123456789" ← s+3 的 "34567") */
    s = require(sdsnew("0123456789"), "sdsnew");
    s = require(sdscpylen(s, s + 3, 5), "sdscpylen");
    CHECK(sdslen(s) == 5);
    CHECK(memcmp(s, "34567", 5) == 0);
    sdsfree(s);

    /* 便捷版与 NULL 语义 */
    s = require(sdsnew("something long-ish"), "sdsnew");
    s = require(sdscpy(s, "hi"), "sdscpy");
    CHECK(sdslen(s) == 2);
    CHECK(memcmp(s, "hi", 2) == 0);
    CHECK(s[2] == '\0');
    s = require(sdscpy(s, NULL), "sdscpy(NULL)");
    CHECK(sdslen(s) == 0);
    CHECK(s[0] == '\0');
    sdsfree(s);
}

/* ---- 预分配策略 ---- */

/* 创建恰好分配;小串扩容翻倍;越过 1MB 后改为每次 +1MB */
static void test_prealloc(void)
{
    /* 创建:恰好分配(alloc == len,avail == 0) */
    sds s = require(sdsnewlen("aaaa", 4), "sdsnewlen");
    CHECK(sdsavail(s) == 0);

    /* 小串扩容:翻倍(newlen=5 → 至少留出 2*5) */
    s = require(sdscatlen(s, "b", 1), "sdscatlen");
    CHECK(sdslen(s) == 5);
    CHECK(sdslen(s) + sdsavail(s) >= 2 * sdslen(s));
    sdsfree(s);

    /* 大串:累计越过 SDS_MAX_PREALLOC 后,不再翻倍,
     * 总分配不应远超 len + 1MB(留 4096 余量容忍取整) */
    s = require(sdsempty(), "sdsempty");
    char chunk[1024];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 1100; i++) /* 约 1.07MB,保证越过阈值 */
        s = require(sdscatlen(s, chunk, sizeof(chunk)), "sdscatlen");
    CHECK(sdslen(s) == 1100ul * 1024);
    CHECK(sdslen(s) + sdsavail(s) <= sdslen(s) + SDS_MAX_PREALLOC + 4096);
    sdsfree(s);
}

/* ---- 二进制安全 ---- */

/* 0~255 全字节值经追加/覆盖后原样保真 */
static void test_binary_roundtrip(void)
{
    char all[256];
    for (int i = 0; i < 256; i++)
        all[i] = (char)i;

    sds s = require(sdsempty(), "sdsempty");
    s = require(sdscatlen(s, all, sizeof(all)), "sdscatlen");
    CHECK(sdslen(s) == 256);
    CHECK(memcmp(s, all, sizeof(all)) == 0); /* 首尾都是 '\0' 也不影响 */

    char half[3] = { 0, 1, 0 };
    s = require(sdscatlen(s, half, sizeof(half)), "sdscatlen");
    CHECK(sdslen(s) == 259);
    CHECK(s[256] == 0 && s[257] == 1 && s[258] == 0);
    CHECK(s[259] == '\0');
    sdsfree(s);
}

/* ---- 随机对照压测 ---- */

/* 线性同余伪随机:固定种子,失败可复现 */
static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 16;
}

/* 随机操作序列跑 3000 轮,每轮与普通 C 缓冲区参考实现逐字节对照,
 * 能抓住绝大多数扩容/长度/拷贝的簿记错误 */
static void test_stress(void)
{
    static char ref[1 << 17];
    size_t ref_len = 0;
    sds s = require(sdsempty(), "sdsempty");

    for (int iter = 0; iter < 3000; iter++) {
        uint32_t op = rng() % 100;
        if (op < 55 || ref_len == 0) {
            /* 追加 1~200 个随机字节(可含 '\0') */
            size_t n = rng() % 200 + 1;
            for (size_t i = 0; i < n; i++)
                ref[ref_len + i] = (char)(rng() & 0xff);
            s = require(sdscatlen(s, ref + ref_len, n), "sdscatlen");
            ref_len += n;
        } else if (op < 80) {
            /* 覆盖为随机长度:变长/变短都可能触发扩容 */
            ref_len = rng() % 512 + 1;
            for (size_t i = 0; i < ref_len; i++)
                ref[i] = (char)(rng() & 0xff);
            s = require(sdscpylen(s, ref, ref_len), "sdscpylen");
        } else if (op < 90) {
            /* 清空(惰性释放)后再继续用 */
            ref_len = 0;
            sdsclear(s);
        } else {
            /* C 串便捷入口 */
            memcpy(ref + ref_len, "hello", 5);
            s = require(sdscat(s, "hello"), "sdscat");
            ref_len += 5;
        }

        /* 每轮全量对照 */
        CHECK(sdslen(s) == ref_len);
        CHECK(memcmp(s, ref, ref_len) == 0);
        CHECK(s[ref_len] == '\0');

        /* 接近参考缓冲上限先收缩,给下轮追加留足余量 */
        if (ref_len > sizeof(ref) - 8192) {
            ref_len = rng() % 1024;
            s = require(sdscpylen(s, ref, ref_len), "sdscpylen");
        }
    }
    sdsfree(s);
}

int main(void)
{
    test_newlen_basic();
    test_new_and_dup();
    test_free_null();
    test_clear_lazy();
    test_catlen();
    test_cat_realloc_chain();
    test_cpylen();
    test_prealloc();
    test_binary_roundtrip();
    test_stress();
    printf("test_sds: all checks passed\n");
    return 0;
}
