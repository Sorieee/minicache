# mini-cache 开发计划

> 配套文档:`docs/design.md`(设计文档)
> 规模预估:单人,合计约 **10 个工作日**(每天 3~4 小时有效编码);每阶段结束均可编译、运行、提交。

---

## 1. 总体路线

```
M0 工程骨架 ──▶ M1 网络层 ──▶ M2 RESP 协议 ──▶ M3 存储引擎(dict+sds)
                                                    │
                    M7 扩展命令+INFO ◀── M6 AOF ◀── M4 命令框架+核心命令 ◀──┐
                            │                            │                │
                            └──▶ M8 加固与压测 ◀── M5 过期机制 ◀──────────┘
```

原则:

1. **每个里程碑交付一个"能跑"的程序**——哪怕是 echo 服务器,也能 telnet 验证;
2. **先通后优**:先用最直白的实现跑通,再引入渐进式 rehash、EMBSTR 等优化;
3. **每个 M 都有明确验收标准**,不达标不开下一个;
4. 每完成一个里程碑:`git tag m<N>` + 简短 CHANGELOG。

---

## 2. 里程碑明细

### M0 工程骨架(0.5 天)

**目标**:空服务器也能编译运行,后续只填肉不动架子。

任务清单:

- [x] 建立目录结构(src/、tests/、docs/),CMakeLists.txt(`-Wall -Wextra -Werror -std=c17`)
- [x] `util.c`:日志宏 `LOGD/LOGI/LOGW/LOGE`(带时间戳与级别)
- [ ] sds 基础版:`sdsnewlen / sdscatlen / sdsfree / sdslen / sdsclear`(预分配翻倍策略)
- [ ] 全局 `server` 结构体定义与 `initServerConfig()`

**交付物**:`cmake -S . -B build && cmake --build build && ./build/mini-cache` 打印启动日志后正常退出。
**验收**:`test_sds` 单元测试通过;零警告编译。

---

### M1 网络层:TCP echo 服务器(1 天)

**目标**:telnet 连上后,发什么回什么(原样 echo),事件循环立起来。

任务清单:

- [ ] socket / bind / listen / 设置非阻塞 / SO_REUSEADDR
- [ ] epoll 封装:`aeCreateFileEvent/aeMain/aeStop`(水平触发)
- [ ] accept 循环(一次把 pending 连接收完,到 EAGAIN)
- [ ] client 结构体、querybuf/replybuf 管理、断连清理(EPOLLRDHUP/EOF)
- [ ] 可写事件:replybuf 未写完时挂 EPOLLOUT,写完摘除
- [ ] SIGINT/SIGTERM 优雅退出;连接数上限(默认 1000)

**交付物**:`telnet 127.0.0.1 6380` echo 正常,多个并发 telnet 互不干扰。
**验收**:100 个并发 nc 连接收发 1000 次无错;Ctrl-C 退出无残留。

---

### M2 RESP 协议解析(1.5 天)

**目标**:把字节流变成 `argv[]`,把回复编码成 RESP;redis-cli 能"连上但查不到数据"。

任务清单:

- [ ] 解析状态机:多 bulk 数组模式(`*N` → `$len` → data → `\r\n` 校验)
- [ ] 半包/粘包处理:数据不足即返回,完整命令立即执行并消费缓冲区
- [ ] 内联命令(inline)支持:非 `*` 开头按空格切分(telnet 可用)
- [ ] 回复辅助函数:`addReply*`(shared.ok / bulk / longlong / error / array)
- [ ] 协议错误处理:回 `-ERR Protocol error` 并关连接
- [ ] querybuf 上限(64MB)保护

**交付物**:echo 服务器升级为:收到合法 RESP 命令打印 `argc/argv` 日志,回复 `+OK`。
**验收**:`test_resp` 覆盖 15+ 用例(正常/半包/粘包/空数组/负长度/坏结尾符);`redis-cli -p 6380 ping` 收到 `+OK` 或规范错误。

---

### M3 存储引擎:dict + 渐进式 rehash(2 天,核心难点)

**目标**:一个与 Redis 同机制的哈希表,纯库代码,不依赖网络层。

任务清单:

- [ ] dictEntry/dictht/dict 结构;FNV-1a 哈希 + 随机种子
- [ ] `dictAdd / dictFind / dictDelete / dictReplace / dictEmpty`
- [ ] 扩容(负载因子 ≥1)/缩容(<0.1)触发条件与 2^n sizing
- [ ] **渐进式 rehash**:rehashidx、单步搬桶 `_dictRehashStep`、定时批量搬 1ms
- [ ] rehash 期间的读写删正确性:查两表、删两表、新增只进 ht[1]
- [ ] 安全迭代器(全量遍历用,KEYS/FLUSHALL 需要)
- [ ] 任意指针 → robj 的泛型处理(用回调存/释放 key 与 value)

**交付物**:`tests/test_dict` 全绿。
**验收**:100 万 key 插入时无单次长停顿(记录每次操作耗时,max < 5ms);rehash 中途反复 GET/DEL 结果正确;删除至负载 <0.1 触发缩容。

---

### M4 命令框架 + 核心命令(1 天)

**目标**:**redis-cli 完整可用**——SET/GET/DEL/EXISTS/KEYS/FLUSHALL/PING/ECHO。

任务清单:

- [ ] 命令表 + 分发器(小写化、arity 校验、unknown command 错误)
- [ ] robj(先 RAW/INT 两种编码)+ `db.c` 键空间操作封装
- [ ] 实现:PING、ECHO、SET、GET、DEL、EXISTS、KEYS(*)、FLUSHALL、SELECT(仅 0)、TYPE
- [ ] 与 Redis 一致的语义细节:SET 覆盖时清 TTL;DEL 返回实际删除数
- [ ] stats:total_commands_processed / keyspace_hits / misses

**交付物**:`redis-cli -p 6380` 手工冒烟通过。
**验收**:`integration.sh` 前 8 个用例(SET→GET→DEL→GET→EXISTS→KEYS→FLUSHALL→PING)全部符合 Redis 预期输出。

---

### M5 过期机制(1 天)

**目标**:key 能自动"消失",TTL 可查。

任务清单:

- [ ] `expires` 表;EXPIRE/PERSIST/TTL/PTTL/SET key val EX s
- [ ] 惰性删除:所有读写路径前 `expireIfNeeded()`(注意 KEYS/DEL 也要过滤过期 key)
- [ ] `activeExpireCycle()`:每 100ms 采样 20 个、过期比 >25% 则继续、总时限 1ms
- [ ] TTL 返回值三态:不存在 -2 / 无 TTL -1 / 剩余秒数
- [ ] SET 覆盖旧 key 时清除旧 TTL

**交付物**:`SETEX k 2 v` → 2 秒内 GET 有值 → 之后 GET 空且 EXISTS 0。
**验收**:1000 个随机 TTL 的 key 在 10 分钟内全部被清(activeExpire 日志可观测);TTL 边界值(0、负数)按 Redis 语义报错/生效。

---

### M6 AOF 持久化(1.5 天)

**目标**:`kill -9` 后数据不丢(everysec 策略下最多丢 1 秒)。

任务清单:

- [ ] 写命令 → RESP 格式追加 `aof_buf`;事件循环每轮 write 到文件
- [ ] fsync 三策略:always / everysec(默认)/ no;serverCron 检查落盘时间
- [ ] 启动回放:读文件 → 拆命令 → **fake client 复用命令执行路径** → 日志报告回放条数
- [ ] 文件末尾半条命令:截断 + WARN 继续;`--aof-check` 单独校验模式
- [ ] 优雅退出时 flush aof_buf 并 fsync

**交付物**:带 `--aof-file` 启动,重启后数据完整。
**验收**:
1. 写 5000 key → `kill -9` → 重启 → 全部 GET 正确;
2. 人为截断 AOF 尾部 → 启动 WARN 但成功,前序数据完好;
3. appendfsync=always 模式同样通过用例 1。

---

### M7 扩展命令 + INFO(1 天)

**目标**:补齐常用 string/键命令,服务可观测。

任务清单:

- [ ] INCR / DECR / INCRBY / DECRBY / APPEND / STRLEN(整数编码直算,溢出报错与 Redis 文案一致)
- [ ] SETNX / SETEX / MSET / MGET
- [ ] INFO(server/clients/stats/keyspace/persistence 段,格式仿 Redis)
- [ ] COMMAND(返回精简表,让 redis-cli 不报 unknown)/ DBSIZE / RANDOMKEY

**交付物**:redis-cli `INCR counter` 连打 10 次得到 1..10;`INFO` 输出可读。
**验收**:integration.sh 扩展用例全过;`redis-benchmark -t set,get,incr -n 100000` 正常跑完无协议错。

---

### M8 加固、压测与文档(1 天)

**目标**:能对外说"这是一个正确、健壮的最小 Redis"。

任务清单:

- [ ] valgrind 全套集成用例,修完所有 definitely lost / invalid read
- [ ] 健壮性用例:垃圾字节、超长 value(10MB)、协议半包挂起、1024 并发连接、启动时 AOF 缺失/损坏
- [ ] 压测基线记录:redis-benchmark SET/GET/INCR 各 10 万次,QPS 与 P99 延迟写进 README
- [ ] README:构建、运行、命令清单、与 Redis 的差异说明、已知限制
- [ ] `git tag v0.1.0`,CHANGELOG

**验收(项目总体 Definition of Done)**:
1. redis-cli 全功能冒烟通过;
2. valgrind 零泄漏;
3. redis-benchmark 正常完成;
4. 断电恢复用例通过;
5. 文档齐备。

---

## 3. 时间总览

| 里程碑 | 内容 | 工期 | 累计 |
|---|---|---|---|
| M0 | 骨架 + sds + 日志 | 0.5d | 0.5d |
| M1 | epoll echo 服务器 | 1d | 1.5d |
| M2 | RESP 解析 | 1.5d | 3d |
| M3 | dict + 渐进式 rehash | 2d | 5d |
| M4 | 命令框架 + SET/GET | 1d | 6d |
| M5 | 过期机制 | 1d | 7d |
| M6 | AOF | 1.5d | 8.5d |
| M7 | 扩展命令 + INFO | 1d | 9.5d |
| M8 | 加固/压测/文档 | 1d | 10.5d |

可压缩项:EMBSTR 编码、内联命令、PERSIST/PTTL、COMMAND → 各省 0.5~1 天,最小可跑通路径约 7 天。
容易超期项:M3(rehash 正确性)与 M6(回放边界),已各留缓冲。

---

## 4. 每日开发节奏建议

1. 从 TODO 清单挑一条,先写测试或手工验证脚本;
2. 实现 → 单测/集成用例绿 → `git commit`(约定:`m4: add SET/GET commands`);
3. 收工前:`ctest --test-dir build` 全绿 + telnet/redis-cli 冒烟一次;
4. 里程碑完成:更新 CHANGELOG,打 tag。

---

## 5. 风险与应对

| # | 风险 | 影响 | 应对 |
|---|---|---|---|
| 1 | RESP 半包/粘包解析出错(最难缠的 bug 源) | 命令错乱、crash | 严格状态机 + 专项单测先行(M2 之前就写好用例);用 redis-cli+telnet 双通道交叉验证 |
| 2 | 渐进式 rehash 期间读写删不一致 | 数据丢失/重复 | 单元测试覆盖"rehash 中途每类操作";先实现整体 rehash 再改渐进,便于对比 |
| 3 | AOF 回放与在线执行行为不一致 | 重启后数据漂移 | 回放必须复用同一命令执行函数(fake client),禁止另写一套逻辑 |
| 4 | 大 value / 大回复阻塞事件循环 | 卡顿、超时 | 输出缓冲 + EPOLLOUT 写回;缓冲区上限断连 |
| 5 | 内存泄漏累积 | 长跑 OOM | zmalloc 统计从 M0 就做;M8 valgrind 兜底;dict 删除路径单测验证 free 次数 |
| 6 | 过期采样活锁(过期 key 巨多) | CPU 打满 | activeExpireCycle 严格限时 1ms/轮 |
| 7 | 范围蔓延(提前做 list/zset/复制) | 项目烂尾 | 严格按里程碑;新想法记入 design.md §13 演进路线 |

---

## 6. 编码约定

- C17,依赖仅 libc + POSIX;禁止引入第三方库;
- 编译选项:`-std=c17 -Wall -Wextra -Werror -O2`(debug 加 `-g -fsanitize=address,undefined`);
- 命名:函数 `模块前缀_动作`(如 `dictAdd`、`addReplyBulk`),类型 `_t` 结尾或小驼峰结构体;
- 每个 .h 有 include guard 与职责注释;公共函数头部注释说明参数与语义;
- 错误处理约定:内存分配失败直接 `OOM()` 打日志并 abort(Redis 同款取舍)。

---

## 7. 验收命令速查(最终态)

```bash
cmake -S . -B build        # 配置
cmake --build build        # 编译 → build/mini-cache
./build/mini-cache --port 6380 --aof-file dump.aof
redis-cli -p 6380 SET hello world
redis-cli -p 6380 GET hello          # → "world"
redis-cli -p 6380 INCR counter
redis-benchmark -p 6380 -t set,get,incr -n 100000 -q
ctest --test-dir build               # 单元测试
bash tests/integration.sh            # 端到端用例
valgrind --leak-check=full ./build/mini-cache --port 6381   # 内存检查
```
