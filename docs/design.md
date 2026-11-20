# mini-cache 设计文档

> 用 C 语言实现的类 Redis 内存键值数据库(最小可用版本 → 逐步演进)
> 版本:v0.1 草案 | 日期:2026-11-16

---

## 1. 项目概述

### 1.1 目标

实现一个兼容 Redis 协议(RESP)的单机内存 KV 存储,使得:

1. **协议兼容**:可直接用 `redis-cli` 连接并执行命令,亦可用 `telnet` 内联命令交互;
2. **核心语义对齐**:键空间、过期(TTL)、错误回复格式与 Redis 行为一致;
3. **简单可读**:代码总量控制在约 3000~5000 行,每行代码都可解释"为什么这样写";
4. **可演进**:架构预留持久化、扩展数据结构(string 之外)的扩展点。

### 1.2 非目标(第一阶段明确不做)

- 主从复制、哨兵、集群
- Lua 脚本、事务(MULTI/EXEC)、发布订阅
- 除 string 外的复杂数据类型(list/hash/set/zset)——架构上预留,编码上后置
- 多线程 IO、io_uring
- RDB 快照(AOF 优先,RDB 列入远期)

### 1.3 名词约定

| 名词 | 含义 |
|---|---|
| RESP | Redis Serialization Protocol,Redis 客户端/服务端通信协议 |
| 键空间 (keyspace) | db 中所有 key → value 的映射 |
| 过期表 (expires) | key → 过期时间戳(毫秒)的映射 |

---

## 2. 总体架构

### 2.1 架构分层

```
                 ┌─────────────────────────────────────────┐
   redis-cli     │              mini-cache 进程             │
      │          │                                         │
 TCP ─┼─────────▶│  网络层 net.c                            │
      │          │  epoll 事件循环 / accept / read / write   │
      │          ├─────────────────────────────────────────┤
      │          │  协议层 resp.c                           │
      │          │  RESP 请求解析(状态机)/ 回复编码          │
      │          ├─────────────────────────────────────────┤
      │          │  命令层 commands.c                       │
      │          │  命令表查找 / 参数校验 / 各命令实现          │
      │          ├─────────────────────────────────────────┤
      │          │  存储层 db.c + dict.c + sds.c + object.c │
      │          │  键空间 / 过期表 / 哈希表 / 动态字符串      │
      │          ├─────────────────────────────────────────┤
      └──────────│  持久化层 aof.c                          │
                 │  写命令追加 / 启动回放 / fsync 策略        │
                 └─────────────────────────────────────────┘
```

### 2.2 核心设计决策

| 决策点 | 选择 | 理由 |
|---|---|---|
| 并发模型 | 单线程 Reactor(epoll LT) | 命令天然原子、无锁、无数据竞争;Redis 6 之前同样如此 |
| epoll 触发模式 | 水平触发(LT) | 语义简单,读写不完整不会丢事件,最不容易写出 bug |
| 键值容器 | 开链哈希表 + 渐进式 rehash | 复刻 Redis 核心机制:rehash 分摊到每次操作,避免长停顿 |
| 字符串 | 自实现 sds | O(1) 取长度、二进制安全、预分配减少 realloc |
| 协议 | RESP2 数组式命令 | 与 redis-cli 直接兼容,解析器是手写增量状态机 |
| 过期 | 惰性删除 + 主动采样删除 | 与 Redis 相同的组合策略 |
| 持久化 | AOF(append only),fsync=everysec | 实现简单、数据安全;启动时回放重建键空间 |
| 回复写回 | 输出缓冲 + 可写事件 | 大回复不会因 socket 缓冲满而阻塞整个事件循环 |

---

## 3. 网络层设计(net.c / server.c)

### 3.1 事件循环

单线程 `epoll` 循环,伪代码:

```
main:
    解析命令行参数(端口、绑定地址、AOF 路径)
    initServer(): 创建监听 socket(非阻塞、SO_REUSEADDR)、注册读事件
    while (server.running):
        timeout = 距下次 cron 的时间(默认 100ms)
        n = epoll_wait(epoll_fd, events, MAX_EVENTS, timeout)
        for ev in events:
            if ev == listen_fd:  acceptClient()          // 循环 accept 到 EAGAIN
            elif ev 读就绪:       readQueryFromClient()
            elif ev 写就绪:       sendReplyToClient()
        serverCron():                                     // 每 ~100ms
            activeExpireCycle()                           // 主动过期采样
            flushAOF()                                    // 按策略 fsync
```

### 3.2 连接与缓冲

```c
typedef struct client {
    int fd;
    sds querybuf;          /* 输入缓冲:累积原始字节,解析后消费 */
    sds replybuf;          /* 输出缓冲:待写出的 RESP 回复 */
    int argc;              /* 本条命令参数个数 */
    sds *argv;             /* 参数(二进制安全 sds 数组) */
    time_t last_interaction;
    struct client *prev, *next;
} client;
```

- **读取**:`read()` 到 EAGAIN 为止,追加进 `querybuf`;单条命令执行后从 `querybuf` 头部消费,防止粘包堆积。
- **写回**:命令执行产生的回复追加到 `replybuf`,先尝试立即 `write()`;未写完(`EAGAIN`)则注册 EPOLLOUT,可写时继续写——避免大回复阻塞事件循环。
- **限额**:`querybuf > 64MB` 或 `replybuf > 16MB` 时断开连接(防恶意客户端打爆内存)。

---

## 4. 协议层设计(resp.c)

### 4.1 RESP2 五种类型

| 类型 | 格式 | 示例 |
|---|---|---|
| Simple String | `+str\r\n` | `+OK\r\n` |
| Error | `-ERR msg\r\n` | `-ERR wrong number of arguments\r\n` |
| Integer | `:n\r\n` | `:1\r\n` |
| Bulk String | `$len\r\ndata\r\n` | `$5\r\nhello\r\n`;不存在时 `$-1\r\n` |
| Array | `*n\r\n` + n 个元素 | 客户端请求即此格式 |

客户端发来的命令统一是 **Bulk String 数组**:
`SET key value` → `*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n`

### 4.2 解析状态机

解析器必须处理 **粘包与半包**(一次 read 可能包含半条命令或多条命令):

```
parseQueryBuffer(client):
    loop:
        if 未处于解析中:
            查找首个 '\n':
              若首字节为 '*':进入"数组模式",读元素个数
              否则:进入"内联命令模式"(telnet 友好),按空白切分整行
        数组模式:
            逐个读 "$len\r\ndata\r\n":
              len 未到齐 → return(等待更多数据)
              data 不足 len+2 → return
              校验结尾必须是 \r\n,否则回复协议错误并关闭
            收齐 argc 个参数 → executeCommand()
        消费掉已解析字节,继续尝试解析下一条(粘包)
```

要点:

1. **增量解析**:任何一步数据不足就整体返回,已确认完整的字节才从缓冲区消费;
2. **内联命令**:第一个字节不是 `*` 时按文本行处理(空格切分),保证 `telnet` 可用;
3. **单次调用可执行多条命令**:循环直到缓冲区剩余内容不足一条完整命令。

### 4.3 错误回复约定(与 Redis 对齐)

| 场景 | 回复 |
|---|---|
| 参数个数错误 | `-ERR wrong number of arguments for 'get' command` |
| 未知命令 | `-ERR unknown command 'foo'` |
| 对错误类型操作 | `-WRONGTYPE Operation against a key holding the wrong kind of value`(预留) |
| 值非整数做 INCR | `-ERR value is not an integer or out of range` |

---

## 5. 存储层设计

### 5.1 sds —— 动态字符串(sds.c)

```c
struct sdshdr {
    uint32_t len;      /* 已用长度 */
    uint32_t alloc;    /* 总分配长度(不含头和 \0) */
    char flags;        /* 头部类型(短字符串用小头) */
    char buf[];        /* 柔性数组,尾部恒有 \0 */
};
```

- `sdsnewlen / sdscatlen / sdscpylen / sdsfree / sdslen / sdsavail`
- **预分配**:追加后 `alloc < 1MB` 时翻倍,否则每次多加 1MB;
- **惰性释放**:缩短时不 realloc,记入 free 空间(最简版可先直接 realloc,标注 TODO);
- **二进制安全**:长度由 `len` 决定,值里可以含 `\0`、图片字节等。

### 5.2 dict —— 渐进式 rehash 哈希表(dict.c)

```c
typedef struct dictEntry {
    void *key;
    void *val;
    struct dictEntry *next;    /* 开链法 */
} dictEntry;

typedef struct dictht {
    dictEntry **table;
    unsigned long size;        /* 桶数,恒为 2^n */
    unsigned long sizemask;    /* size - 1 */
    unsigned long used;        /* 元素数 */
} dictht;

typedef struct dict {
    dictht ht[2];              /* ht[1] 仅在 rehash 期间使用 */
    long rehashidx;            /* -1 = 未在 rehash */
} dict;
```

关键行为(复刻 Redis):

| 操作 | 规则 |
|---|---|
| 哈希函数 | SipHash 或 MurmurHash(最简版可用 FNV-1a + 种子) |
| 扩容触发 | `used / size >= 1`(无 BGSAVE 时),扩到第一个 `>= used*2` 的 2^n |
| 缩容触发 | `used / size < 0.1`,缩到第一个 `>= used` 的 2^n |
| 渐进式 rehash | 置 `rehashidx=0`;此后每次 dict 操作**顺带搬移 1 个桶**;`serverCron` 空闲时批量搬 1ms;期间查询查两张表,新增只进 ht[1];`rehashidx == -1` 结束 |
| 遍历 | 第一阶段只支持全量遍历 `dictGetSafeIterator`(低风险;不实现逆二进制迭代器) |

> 渐进式 rehash 是本项目最有学习价值的模块, 单元测试必须覆盖:扩容、缩容、rehash 中读写删、rehash 中再触发扩容。

### 5.3 键空间与过期(db.c)

```c
typedef struct redisDb {
    dict *dict;       /* keyspace: sds(key) -> robj(value) */
    dict *expires;    /* sds(key) -> long long(毫秒级过期时间) */
} redisDb;
```

第一阶段单 db(`SELECT 0` 之外的 dbid 回错),不做多 db。

### 5.4 值对象(object.c)

```c
typedef struct robj {
    unsigned type:4;      /* OBJ_STRING(第一阶段唯一) */
    unsigned encoding:4;  /* OBJ_ENCODING_INT / EMBSTR / RAW */
    int refcount;
    void *ptr;            /* sds 或 long */
} robj;
```

- string 三种编码:`INT`(≤20 字节且是整数,直接存 long)、`EMBSTR`(≤44 字节,sds 头与 robj 一次 malloc)、`RAW`;
- 最简实现可以先只有 RAW/INT,EMBSTR 作为优化项;
- `INCR` 等命令利用 INT 编码直接做整数运算。

### 5.5 过期策略

- **写 TTL**:`EXPIRE/SETEX/SET ... EX` 写入 `expires` 表;
- **惰性删除**:每次读写 key 前调 `expireIfNeeded(key)`:已过期 → 从 `dict`、`expires` 删除并视为不存在(写命令语义 = 新建);
- **主动删除** `activeExpireCycle()`(serverCron 中,限时 ≤1ms):
  1. 从 `expires` 随机采样 20 个 key;
  2. 过期的删除;
  3. 若过期比例 > 25%,立即再采样一轮;总时间超限则退出;
- 退出时进程无需清理(数据在内存,进程结束即释放)。

---

## 6. 命令层设计(commands.c)

### 6.1 命令表

```c
typedef struct redisCommand {
    char *name;                              /* 小写 */
    int arity;                               /* 正数=精确;负数=>=|arity| */
    int flags;                               /* CMD_WRITE / CMD_READONLY / CMD_ADMIN */
    int (*proc)(client *c);
} redisCommand;

static redisCommand commandTable[] = {
    {"get",      2,  CMD_READONLY, getCommand},
    {"set",     -3,  CMD_WRITE,    setCommand},
    {"del",     -2,  CMD_WRITE,    delCommand},
    {"exists",  -2,  CMD_READONLY, existsCommand},
    {"ping",    -1,  CMD_READONLY, pingCommand},
    {"echo",     2,  CMD_READONLY, echoCommand},
    {"incr",     2,  CMD_WRITE,    incrCommand},
    {"expire",   3,  CMD_WRITE,    expireCommand},
    {"ttl",      2,  CMD_READONLY, ttlCommand},
    {"keys",     2,  CMD_READONLY, keysCommand},   /* 仅支持 "*" */
    {"flushall", 1,  CMD_WRITE,    flushallCommand},
    {"info",    -1,  CMD_READONLY, infoCommand},
    {"command", -1,  CMD_READONLY, commandCommand}, /* 让 redis-cli 正常启动 */
    /* ...按里程碑递增... */
    {NULL, 0, 0, NULL}
};
```

分发流程:`小写化 argv[0]` → `查表` → `校验 arity` → `expireIfNeeded(涉及的 key)` → `proc(c)`。

### 6.2 第一阶段命令集与语义

| 命令 | 语义 / 回复 |
|---|---|
| `PING [msg]` | `+PONG` 或 bulk msg |
| `ECHO msg` | bulk msg |
| `SET key val [EX s]` | `+OK`;带 EX 则同时写 expires;SET 覆盖旧值并**清除旧 TTL**(与 Redis 一致) |
| `GET key` | bulk 值;不存在 `$-1`;类型非 string 回 WRONGTYPE(预留) |
| `DEL key [key ...]` | `:被删除个数`(含惰性过期删除计数) |
| `EXISTS key [key ...]` | `:存在的个数`(可重复计) |
| `INCR key` | 原子自增;不存在则建 0 再 +1;非整数报错;`:新值` |
| `DECR key` | 同上取反 |
| `EXPIRE key seconds` | `:1` 成功;key 不存在 `:0` |
| `TTL key` | 剩余秒;无 TTL `-1`;不存在 `-2` |
| `PERSIST key` | 移除 TTL,`:1/:0` |
| `KEYS pattern` | 第一阶段仅 `*`:全部 key 数组 |
| `FLUSHALL` | 清空 db,`+OK` |
| `TYPE key` | `+string` / `+none` |
| `INFO [section]` | text 块:版本、键数、命中/未命中、连接数、内存(RSS 或分配统计) |
| `SELECT i` | 仅 `i==0` 成功(兼容 redis-cli 启动行为) |

### 6.3 命令执行与 AOF 传播

```
executeCommand:
    查表 → arity 校验 → proc(c) 执行
    若命令带 CMD_WRITE 且执行成功:
        将 *原始参数*(argv)重新编码为 RESP 数组追加到 aof_buf
```

---

## 7. 持久化设计(aof.c)

### 7.1 AOF(第一阶段唯一持久化方式)

- **写路径**:写命令成功后,以 RESP 数组格式(`*3\r\n$3\r\nSET\r\n...`)追加进内存 `aof_buf`;事件循环每轮或每 `serverCron` 将 `aof_buf` `write()` 到文件;
- **fsync 策略**(配置项 `appendfsync`):
  - `always`:每次写命令后 fsync(最安全最慢)
  - `everysec`(默认):后台记录上次 fsync 时间,超过 1s 则 fsync
  - `no`:交给操作系统
- **启动恢复**:读 AOF 文件 → 按行拆成 RESP 命令 → **伪造一个 fake client** 逐条走正常命令执行路径(复用命令层,不另写回放逻辑)→ 校验最后一条完整性(崩溃时可能写半条,截断后告警继续);
- **文件损坏处理**:默认截断到上一条完整命令后继续启动(打印告警);提供 `--repair` 语义留作 TODO;
- **AOF 重写**:非目标,列入远期(键空间越大 AOF 越大,重写是必要演进方向)。

### 7.2 RDB(远期)

方向:定期 `fork()` 子进程顺序化快照,COW 保证父进程不被阻塞;或最简版"定时 stop-the-world 序列化"。第一阶段不做。

---

## 8. 内存与资源管理

- 内存来源:`malloc/free` 直接使用;预留 `zmalloc` 包装层统计 `used_memory`(为 INFO 与 maxmemory 铺路);
- 对象生命周期:robj 引用计数(共享整数对象 0~9999 可选);dict 删除 entry 时释放 key(sds)与 val(robj);
- 限额:第一阶段仅做 querybuf/replybuf 上限;`maxmemory` + LRU 淘汰列入远期;
- 退出:`SIGINT/SIGTERM` → 置 `server.running=0` → flush AOF → 关闭所有连接 → 退出。

---

## 9. 配置与运行

```
./mini-cache --port 6380 --bind 127.0.0.1 --aof-file appendonly.aof \
             --appendfsync everysec --daemonize no
```

- 第一阶段允许全部默认值(端口 6380,避免和本机真 Redis 冲突;AOF 可关);
- 不做 redis.conf 解析(远期兼容),先命令行参数 + 少量常量宏。

---

## 10. 可观测性

- 日志:分级 `LOG_DEBUG/INFO/WARN/ERROR`,输出到 stdout,格式 `时间 级别 消息`;
- INFO 内容(第一阶段):
  ```
  # Server
  mini-cache_version:0.1.0
  uptime_in_seconds:123
  # Clients
  connected_clients:3
  # Stats
  total_commands_processed:1024
  keyspace_hits:900
  keyspace_misses:124
  expired_keys:17
  # Keyspace
  db0:keys=5,expires=2
  # Persistence
  aof_enabled:1
  aof_current_size:4096
  ```

---

## 11. 目录结构(规划)

```
mini-cache/
├── Makefile
├── docs/
│   ├── design.md            # 本文档
│   └── development-plan.md  # 开发计划
├── src/
│   ├── main.c               # 入口:参数解析、初始化、信号
│   ├── server.h / server.c  # 全局状态 server、事件循环、serverCron
│   ├── net.h / net.c        # 监听、accept、读写事件处理
│   ├── client.h / client.c  # client 生命周期、缓冲区管理
│   ├── resp.h / resp.c      # RESP 解析(状态机)与回复编码辅助
│   ├── dict.h / dict.c      # 哈希表 + 渐进式 rehash
│   ├── sds.h / sds.c        # 动态字符串
│   ├── object.h / object.c  # robj 与编码
│   ├── db.h / db.c          # 键空间操作、过期逻辑
│   ├── commands.h / commands.c  # 命令表与全部命令实现
│   ├── aof.h / aof.c        # AOF 追加、落盘、启动回放
│   └── util.h / util.c      # 日志、字符串工具、随机采样
└── tests/
    ├── test_sds.c           # sds 单元测试
    ├── test_dict.c          # dict 单元测试(扩缩容/rehash 中操作)
    ├── test_resp.c          # 解析器:半包/粘包/非法协议用例
    ├── test_expire.c
    ├── integration.sh       # redis-cli 端到端脚本
    └── run_tests.sh
```

依赖:仅 libc + POSIX socket/epoll,不引入第三方库;目标平台 Linux(C17,`-Wall -Wextra -Werror`)。

---

## 12. 测试与验收策略

| 层次 | 手段 | 标准 |
|---|---|---|
| 单元测试 | tests/ 下针对 sds、dict、resp 的断言式用例,`run_tests.sh` 全绿 | dict 必须 rehash 过程中读写删均正确 |
| 端到端 | `redis-cli -p 6380` 执行脚本化命令序列,比对期望输出 | 全部第一/二阶段命令行为与 Redis 文档一致 |
| 兼容性 | `redis-benchmark -p 6380 -t set,get,incr -n 100000` 可正常跑完 | 无协议错误、无崩溃 |
| 健壮性 | telnet 发半条命令、超长 value、非 RESP 垃圾字节、并发 100 连接 | 不崩溃、不泄漏 |
| 内存检查 | `valgrind --leak-check=full` 跑集成脚本 | 0 definitely lost |
| 持久化 | SET 若干 → `kill -9` → 重启 → GET 校验 | everysec 下最多丢 1s 数据 |

---

## 13. 演进路线(远期方向,不在本期)

1. list(quicklist)/hash(dict 编码)/set(intset+dict)/zset(skiplist)
2. RDB + BGSAVE、AOF 重写
3. maxmemory + 8 种淘汰策略
4. MULTI/EXEC 事务(单线程下即命令排队)
5. 主从复制(`PSYNC` 简化版)
6. 多线程 IO / 共享对象 / 内存编码优化(EMBSTR、ziplist 思想)

---

## 附录 A:一次完整交互的字节级示例

```
C: *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
S: +OK\r\n
C: *2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
S: $3\r\nbar\r\n
C: *2\r\n$3\r\nTTL\r\n$3\r\nfoo\r\n
S: -1\r\n
C: SET foo bar          ← telnet 内联命令
S: +OK\r\n
```
