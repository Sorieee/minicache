/* test_sds.c —— sds 模块单元测试(M0)
 *
 * 验收标准:docs/development-plan.md M0 —— test_sds 全绿、零警告编译。
 * 覆盖:
 *   创建/复制/释放(含 NULL 与空串边界)、追加与覆盖(含二进制安全、
 *   自重叠——含跨 realloc 迁移、len==0)、预分配策略(小串翻倍/大串 +1MB,
 *   含 1MB 边界精确值)、惰性释放(覆盖变短后复用、alloc 单调不缩)、
 *   便捷入口容错(NULL/空串)、尾部 '\0' 哨兵不变量、
 *   随机操作序列对照压测(固定种子可复现,含自重叠覆盖)。
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

/* ---- 覆盖拷贝补强 ---- */

/* len == 0 不解引用 t(NULL 也不行);便捷版空串;等长覆盖不 realloc;
 * t == s 零偏移自拷贝;覆盖变短后追加必须复用原有空间(alloc 不缩) */
static void test_cpylen_edge(void)
{
    /* sdscpylen(s, NULL, 0):等价 sdsclear,不得解引用 t */
    sds s = require(sdsnew("hello"), "sdsnew");
    s = require(sdscpylen(s, NULL, 0), "sdscpylen");
    CHECK(sdslen(s) == 0);
    CHECK(s[0] == '\0');

    /* sdscpy(s, ""):同样清空 */
    s = require(sdscpy(s, ""), "sdscpy");
    CHECK(sdslen(s) == 0);
    CHECK(s[0] == '\0');

    /* 等长覆盖:alloc 恰好装下(alloc == len),不得 realloc,总分配不变 */
    s = require(sdscpy(s, "hello"), "sdscpy");
    size_t total = sdslen(s) + sdsavail(s);
    s = require(sdscpylen(s, "world", 5), "sdscpylen");
    CHECK(sdslen(s) == 5);
    CHECK(memcmp(s, "world", 5) == 0);
    CHECK(s[5] == '\0');
    CHECK(sdslen(s) + sdsavail(s) == total);

    /* t == s:零偏移自拷贝,内容原样 */
    s = require(sdscpylen(s, s, 5), "sdscpylen");
    CHECK(sdslen(s) == 5);
    CHECK(memcmp(s, "world", 5) == 0);
    CHECK(s[5] == '\0');
    sdsfree(s);

    /* 覆盖变短后的空闲空间必须能被追加复用(惰性释放的第二种形态) */
    s = require(sdsnew("hello world"), "sdsnew");      /* alloc=11 */
    s = require(sdscpylen(s, "abc", 3), "sdscpylen");  /* len=3,avail=8 */
    s = require(sdscat(s, "defghij"), "sdscat");       /* target=10<=alloc:复用 */
    CHECK(sdslen(s) == 10);
    CHECK(memcmp(s, "abcdefghij", 10) == 0);
    CHECK(sdslen(s) + sdsavail(s) == 11);              /* 全程未 realloc */
    sdsfree(s);
}

/* t 在自身 buf 内且扩容触发 realloc 迁移:t 不得悬垂(ASan/UBSan 兜底抓 UAF)。
 * 锁定 sdscpylen 的"偏移重锚"修复:迁移后偏移仍指向同一段字节 */
static void test_cpylen_overlap_realloc(void)
{
    static char big[3000];
    memset(big, 'A', sizeof(big));

    sds s = require(sdsnewlen(big, sizeof(big)), "sdsnewlen"); /* alloc=3000,avail=0 */
    memset(s, 'B', 100);          /* 内容:前 100 字节 'B',其余 'A' */

    /* 从偏移 2000(内容 'A')自拷贝 3500 字节:alloc 3000 < 3500 → 必然 realloc;
     * 旧块 [2000..3000) 的 1000 个 'A' 随整块迁移,再被拷到新块头部 */
    s = require(sdscpylen(s, s + 2000, 3500), "sdscpylen");
    CHECK(sdslen(s) == 3500);
    for (size_t i = 0; i < 1000; i++)
        CHECK(s[i] == 'A');
    CHECK(s[1000] == '\0');       /* 旧哨兵(偏移 3000)迁移后应落在 1000 */
    CHECK(s[3500] == '\0');       /* 新哨兵 */
    sdsfree(s);
}

/* 便捷入口的容错语义(锁定当前实现:超出头文件基本契约的宽容行为) */
static void test_cat_null_empty(void)
{
    sds s = require(sdsnew("abc"), "sdsnew");
    size_t total = sdslen(s) + sdsavail(s);

    s = require(sdscat(s, NULL), "sdscat(NULL)");      /* 原样返回,不崩溃 */
    CHECK(sdslen(s) == 3);
    s = require(sdscat(s, ""), "sdscat(\"\")");        /* 空串:len/alloc 不变 */
    CHECK(sdslen(s) == 3);
    CHECK(sdslen(s) + sdsavail(s) == total);
    CHECK(memcmp(s, "abc", 3) == 0);

    CHECK(sdsdup(NULL) == NULL);                       /* 容错:返回 NULL */
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

/* 1MB 阈值两侧的精确值:锁定 groupsds 公式的严格 < 判断 ——
 * target < 1M 翻倍(2*target),target >= 1M 改为 target + 1M。
 * 用 sdsnewlen 构造精确 avail==0 的起点,排除历史分配的干扰 */
static void test_prealloc_boundary(void)
{
    static char chunk[SDS_MAX_PREALLOC + 4096];
    memset(chunk, 'x', sizeof(chunk));

    /* target = 1M - 512:仍在翻倍区 → total == 2*target */
    sds s = require(sdsnewlen(chunk, SDS_MAX_PREALLOC - 1024), "sdsnewlen");
    s = require(sdscatlen(s, chunk, 512), "sdscatlen");
    CHECK(sdslen(s) == SDS_MAX_PREALLOC - 512);
    CHECK(sdslen(s) + sdsavail(s) == 2 * (SDS_MAX_PREALLOC - 512));
    sdsfree(s);

    /* target 恰 == 1M:不满足严格 <,走 +1MB 分支 → total == 2M */
    s = require(sdsnewlen(chunk, SDS_MAX_PREALLOC - 512), "sdsnewlen");
    s = require(sdscatlen(s, chunk, 512), "sdscatlen");
    CHECK(sdslen(s) == SDS_MAX_PREALLOC);
    CHECK(sdslen(s) + sdsavail(s) == 2 * (size_t)SDS_MAX_PREALLOC);
    sdsfree(s);

    /* 单次追加直接越过 1M:一步到位,不翻倍 → total == len + 1M */
    s = require(sdsempty(), "sdsempty");
    s = require(sdscatlen(s, chunk, sizeof(chunk)), "sdscatlen");
    CHECK(sdslen(s) == sizeof(chunk));
    CHECK(sdslen(s) + sdsavail(s) == sizeof(chunk) + SDS_MAX_PREALLOC);
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
    size_t prev_total = 0;      /* 上轮 len+alloc,用于检查 alloc 单调不缩 */
    sds s = require(sdsempty(), "sdsempty");

    for (int iter = 0; iter < 3000; iter++) {
        uint32_t op = rng() % 100;
        if (op < 50 || ref_len == 0) {
            /* 追加 1~200 个随机字节(可含 '\0') */
            size_t n = rng() % 200 + 1;
            for (size_t i = 0; i < n; i++)
                ref[ref_len + i] = (char)(rng() & 0xff);
            s = require(sdscatlen(s, ref + ref_len, n), "sdscatlen");
            ref_len += n;
        } else if (op < 75) {
            /* 覆盖为随机长度:变长/变短都可能触发扩容 */
            ref_len = rng() % 512 + 1;
            for (size_t i = 0; i < ref_len; i++)
                ref[i] = (char)(rng() & 0xff);
            s = require(sdscpylen(s, ref, ref_len), "sdscpylen");
        } else if (op < 85) {
            /* 清空(惰性释放)后再继续用 */
            ref_len = 0;
            sdsclear(s);
        } else if (op < 92) {
            /* C 串便捷入口 */
            memcpy(ref + ref_len, "hello", 5);
            s = require(sdscat(s, "hello"), "sdscat");
            ref_len += 5;
        } else {
            /* 自重叠覆盖:t 取自身 buf 中段,memmove 语义原地改写;
             * 偏移/长度随机组合,可命中"需要扩容"的迁移重锚路径 */
            size_t k = rng() % ref_len;
            size_t n = rng() % (ref_len - k) + 1;
            memmove(ref, ref + k, n);
            ref_len = n;
            s = require(sdscpylen(s, s + k, n), "sdscpylen(overlap)");
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

        /* alloc 全程单调不缩:任何操作都不得释放已分配空间(惰性释放) */
        size_t total = sdslen(s) + sdsavail(s);
        CHECK(total >= prev_total);
        prev_total = total;
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
    test_cpylen_edge();
    test_cpylen_overlap_realloc();
    test_cat_null_empty();
    test_prealloc();
    test_prealloc_boundary();
    test_binary_roundtrip();
    test_stress();
    printf("test_sds: all checks passed\n");
    return 0;
}
