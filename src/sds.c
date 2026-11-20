/* sds.c —— 动态字符串 sds 实现(M0)
 *
 * 接口契约见 src/sds.h,每个函数的 TODO 注释列出实现要点。
 *
 * 通用约定(所有函数都要遵守):
 *   - 头部与 buf 同一次 malloc,内存布局:
 *         [ len(4) | alloc(4) | flags(1) | buf[0] ... buf[alloc-1] | '\0' ]
 *     buf 从块内偏移 offsetof(struct sdshdr, buf) 开始;
 *     分配字节数 = offsetof(struct sdshdr, buf) + alloc + 1
 *     (packed 后与 sizeof(struct sdshdr) 相等,两种写法均可);
 *   - malloc/realloc 失败一律 oom("函数名")(util.h),不返回;
 *   - realloc 会移动整块内存:头部与 buf 地址都变,必须用返回值重建;
 *   - free 必须传 malloc 返回的首地址(SDS_HDR(s)),不是 s 本身。
 */

#include "sds.h" /* 自有头文件最先包含,保证其自洽 */
#include "util.h" /* oom:分配失败统一走这里 */

#include <string.h>
#include <stdlib.h>

/* ---- 创建与释放 ---- */

sds sdsempty(void)
{
    /* TODO: 创建空串 ""
     * 1. malloc 头部 + 1 字节('\0');
     * 2. len = alloc = 0,buf[0] = '\0';
     * 3. 返回 buf 地址。
     * (提示:委托 sdsnewlen(NULL, 0) 亦可)
     */
    return sdsnewlen(NULL, 0);
}

sds sdsnewlen(const void *init, size_t initlen)
{
    /* TODO: 按 init[0..initlen) 创建新 sds,创建时恰好分配
     * 1. malloc:头部 + initlen + 1,失败 oom("sdsnewlen");
     * 2. 填头部:len = initlen,alloc = initlen,flags = 0;
     * 3. init != NULL 时 memcpy initlen 字节(NULL 时不碰内容);
     * 4. buf[initlen] = '\0'(尾部哨兵,纯文本 API 依赖它);
     * 5. 返回 buf 地址。
     */
    if (initlen > UINT32_MAX)   /* len/alloc 是 uint32_t:超限会静默截断,按 OOM 处理 */
        oom("sdsnewlen");

    struct sdshdr *sh = malloc(offsetof(struct sdshdr, buf) + initlen + 1);
    if (sh == NULL)
        oom("sdsnewlen");

    sh->len = initlen;
    sh->alloc = initlen;
    sh->flags = 0;              /* malloc 不清零,预留字段按契约置 0 */

    if (init)                   /* NULL 时不碰内容(契约:不保证初始化) */
        memcpy(sh->buf, init, initlen);
    sh->buf[initlen] = '\0';    /* 哨兵无条件写,initlen == 0 时就是 buf[0] */

    return sh->buf;
}

sds sdsnew(const char *init)
{
    /* TODO: C 字符串便捷版
     * init == NULL → 等价空串;否则 initlen = strlen(init)。
     * (提示:算好长度后委托 sdsnewlen)
     */
    size_t initlen = init == NULL ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

sds sdsdup(const sds s)
{
    /* TODO: 深拷贝:新分配、与原串不共享内存
     * (提示:委托 sdsnewlen(s, sdslen(s)),二进制安全由 sdsnewlen 保证)
     */
    if (s == NULL) {
        return NULL;
    }
    return sdsnewlen(s, sdslen(s));
}

void sdsfree(sds s)
{
    /* TODO: 释放整块分配
     * s == NULL 直接返回(调用方习惯不判空);否则 free(SDS_HDR(s))。
     */
    if (s == NULL) {
        return;
    }
    free(SDS_HDR(s));
}

/* ---- 修改 ---- */

void sdsclear(sds s)
{
    /* TODO: 清空但不缩容(惰性释放)
     * len 置 0,buf[0] 置 '\0',alloc 保持不变 ——
     * 省下的空间体现为 sdsavail() 变大,留给后续追加复用。
     */
    SDS_HDR(s)->len = 0;
    SDS_HDR(s)->buf[0] = '\0';
}

sds groupsds(sds s, size_t target) {
    if (target > UINT32_MAX)        /* 与 sdsnewlen 同款:len/alloc 是 uint32_t */
            oom("groupsds");

    size_t newalloc = target < SDS_MAX_PREALLOC
                              ? target * 2
                              : target + SDS_MAX_PREALLOC;

        /* realloc 返回新块首地址(= 新头部),不是 buf;旧指针随即失效,
         * 旧内容整体原样迁移,无需搬移(空闲区不必初始化) */
    struct sdshdr *sh = realloc(SDS_HDR(s), offsetof(struct sdshdr, buf) + newalloc + 1);
    if (sh == NULL) {
        oom("groupsds");
    }
    sh->alloc = newalloc;           /* len 在下面统一更新 */
    s = sh->buf;
    return s;
}
sds sdscatlen(sds s, const void *t, size_t len)
{
    /* TODO: 追加 t[0..len) 到 s 末尾
     * 1. len == 0 → 原样返回 s(不解引用 t);
     * 2. sdsavail(s) >= len → 有现成空间,跳到第 4 步;
     * 3. 扩容(realloc):
     *      target = sdslen(s) + len;                       新总长
     *      newalloc = target < SDS_MAX_PREALLOC
     *                     ? target * 2                     小串:翻倍
     *                     : target + SDS_MAX_PREALLOC;     大串:多给 1MB
     *      realloc(SDS_HDR(s), offsetof(buf) + newalloc + 1),失败 oom;
     *      realloc 后地址已变:用返回值重建指针并更新新头部的 alloc;
     * 4. memcpy 到 buf 末尾(sdslen(s) 处),len 累加,buf[len] = '\0';
     * 5. 返回(可能已迁移的)新 buf 地址。
     */
    if (len == 0) {
        return s;               /* 不解引用 t */
    }

    size_t target = sdslen(s) + len;    /* 追加后的新总长 */
    if (sdsavail(s) < len) {            /* 现有空间不足,先扩容 */
        s = groupsds(s, target);
        if (s == NULL) {
            oom("sdscatlen");
        }
    }

    /* 到这里保证 avail >= len:两个分支汇合,一条写路径 */
    memcpy(s + sdslen(s), t, len);
    SDS_HDR(s)->len = target;
    s[target] = '\0';
    return s;
}

sds sdscat(sds s, const char *t)
{
    /* 长度取 strlen(t),委托 sdscatlen。
     */
     if (t == NULL) {
        return s;
     }
    size_t tlen = strlen(t);
    return sdscatlen(s, t, tlen); 
}

sds sdscpylen(sds s, const char *t, size_t len)
{
    /* TODO: 覆盖式拷贝,丢弃 s 原内容
     * 1. sdsavail(s) < len → 按 sdscatlen 第 3 步同款策略扩容
     *    (此时 target = len,不需要旧长度);
     * 2. memmove(s, t, len) —— 必须用 memmove 而非 memcpy:
     *    t 允许指向 s 自身内部(原地改写,如 sdscpylen(s, s + 3, 5));
     * 3. len 置 len,buf[len] = '\0';alloc 不缩(复用原有空间);
     * 4. len == 0 时不解引用 t,效果等价 sdsclear;
     * 5. 返回(可能已迁移的)新 buf 地址。
     */
    (void)s;
    (void)t;
    (void)len;
    return NULL; /* TODO: 实现后删除占位 */
}

sds sdscpy(sds s, const char *t)
{
    /* TODO: sdscpylen 的 C 串便捷版
     * t == NULL → 等价 sdsclear(s) 后返回 s;
     * 否则委托 sdscpylen(s, t, strlen(t))。
     */
    (void)s;
    (void)t;
    return NULL; /* TODO: 实现后删除占位 */
}
