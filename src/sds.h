#ifndef SDS_H
#define SDS_H

/*
 * sds.h —— 动态字符串 sds(M0)
 *
 * 二进制安全的字符串类型,设计见 docs/design.md §5.1:
 *   - 头部(sdshdr)与数据一次 malloc,头部记 len/alloc,取长度 O(1);
 *   - buf 尾部恒有 '\0',可直接喂给 printf 等纯文本 API,
 *     但长度以 len 为准,内容可含 '\0' 与任意字节(二进制安全);
 *   - 预分配:扩容时若 alloc < SDS_MAX_PREALLOC 则至少翻倍,
 *     否则每次多留 1MB,减少连续追加时的 realloc 次数;
 *   - 惰性释放:缩短只减 len 不缩容,省下的空间记入 sdsavail()。
 *
 * 调用方约定:
 *   - sds 是"句柄":任何可能扩容的操作(sdscatlen/sdscpylen)都可能
 *     realloc 并返回新地址,必须使用返回值,旧指针随即失效;
 *   - 头部与数据同一次分配,不得对 sds 直接 free 之外的内存操作。
 */

#include <stddef.h> /* size_t、offsetof */
#include <stdint.h> /* uint32_t */

typedef char *sds;

/* 头部结构:藏在 buf 之前,与数据一次分配。
 * packed(Redis 同款):去掉尾部填充,使 sizeof == offsetof(buf) == 9。
 * 不 pack 的话 sizeof = 12,而 buf 前只占 9 字节 —— -O2 下 GCC 能推断出
 * malloc 的真实大小,比 sizeof 小的整块访问会被 -Warray-bounds 判越界 */
struct __attribute__((__packed__)) sdshdr {
    uint32_t len;   /* buf 已用长度(不含末尾 '\0') */
    uint32_t alloc; /* 为 buf 分配的总长度(不含头部与 '\0') */
    char flags;     /* 头部类型;基础版恒为 0,预留将来短串用更小的头 */
    char buf[];     /* 柔性数组;buf[len] 恒为 '\0' */
};

/* 由 sds 反查头部。packed 后 sizeof == offsetof(buf),这里仍写 offsetof,
 * 表达"buf 前面的部分是头部",与分配公式互为镜像 */
#define SDS_HDR(s) ((struct sdshdr *)((s) - offsetof(struct sdshdr, buf)))

/* 编译期锁住 packed 不变量:谁去掉 __packed__ 这里立即编译失败 */
_Static_assert(sizeof(struct sdshdr) == offsetof(struct sdshdr, buf),
               "sdshdr 必须保持 packed:sizeof 应等于头部大小(不含 buf)");

/* 这两个函数只读头部、与内存管理无关,故内联在头文件里,
 * 让所有调用方 O(1) 取长度(设计目标,见 docs/design.md §2);
 * 其余方法(涉及分配的)在 sds.c 中实现 */
static inline size_t sdslen(const sds s)
{
    return SDS_HDR(s)->len;
}

static inline size_t sdsavail(const sds s)
{
    return SDS_HDR(s)->alloc - SDS_HDR(s)->len;
}

/* 预分配阈值:alloc 超过它之后扩容不再翻倍,改为每次多加 1MB(Redis 同款) */
#define SDS_MAX_PREALLOC (1024 * 1024)

/* ---- 创建与释放 ---- */

/* 创建空字符串(仅头部),随后用 sdscatlen 填内容 */
sds sdsempty(void);

/* 用 init[0..initlen) 创建新 sds;二进制安全,len 可含 '\0';
 * 创建时恰好分配(alloc == initlen,预分配只在扩容时发生);
 * init == NULL 时 buf 内容不保证初始化 */
sds sdsnewlen(const void *init, size_t initlen);

/* 用 C 字符串创建(内部 strlen);init == NULL 等价于空串 */
sds sdsnew(const char *init);

/* 深拷贝一份,与原串不共享任何内存(dict 存 key 时常用) */
sds sdsdup(const sds s);

/* 释放整块分配(头部 + 数据);s == NULL 时直接返回 */
void sdsfree(sds s);

/* ---- 修改 ---- */

/* 清空但不缩容(惰性释放):len 置 0、buf[0] 置 '\0',空间留给下次复用 */
void sdsclear(sds s);

/* 把 t[0..len) 追加到 s 末尾;空间不足时按预分配策略扩容。
 * len == 0 时不解引用 t、原样返回。
 * 返回(可能已迁移的)新指针,调用方必须使用返回值 */
sds sdscatlen(sds s, const void *t, size_t len);

/* sdscatlen 的 C 字符串便捷版(长度取 strlen,t 必须 '\0' 结尾) */
sds sdscat(sds s, const char *t);

/* 覆盖式拷贝:丢弃 s 原内容,写入 t[0..len)。
 * 复用原有空间、不主动缩容;同样可能 realloc,须使用返回值;
 * t 与 s 允许重叠(按 memmove 语义原地改写) */
sds sdscpylen(sds s, const char *t, size_t len);

/* sdscpylen 的 C 字符串便捷版;t == NULL 等价于 sdsclear(s) */
sds sdscpy(sds s, const char *t);

#endif /* SDS_H */
