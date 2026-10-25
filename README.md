# mem_snapshot

Linux/glibc 下的 C 堆内存快照工具。启动时通过 `LD_PRELOAD` 拦截分配函数，按指针记录分配大小与调用栈；释放时删除记录；向进程发送 `SIGUSR2` 后输出仍存活的分配。

**未释放不等于泄漏。** 快照也包含正常存活对象及运行库分配。结合业务结束点、记录 ID、大小及多次快照判断。`dropped > 0` 表示发生过漏记，结果不完整。

## 设计与当前状态

详细取舍见 [DESIGN.md](DESIGN.md)，实测环境和结果见 [VALIDATION.md](VALIDATION.md)。核心是 64 分片、65536 个哈希桶和分片节点池；按指针查找平均 O(1)，最坏仍取决于桶内链长。free 不展开栈；分配时默认展开最多 16 层。全量栈追踪开销明显，应在可接受性能影响的诊断环境使用。

这是第一版可运行实现，不是经过长期生产验证的分配器替代品。保留真实 glibc 的分配/释放行为，仅附加记录。当前验证平台为 x86-64 WSL Linux、glibc 2.43；其他 glibc 版本及架构尚未验证。

## 编译与验收

需要 Linux、glibc、C11 编译器、Make；测试及离线解析需要 Python 3、binutils 的 addr2line。不需要第三方 Python 包。

```bash
make
make test
make benchmark
```

编译使用 `-Wall -Wextra -Werror`。基准原始样本写到 `build/benchmark.json`。Windows 用户在 WSL 中运行；生成的 `.so` 不能用于原生 Windows 进程。

## 演示

在构建目录所在的 Linux shell 运行以下命令。演示程序会故意遗漏一个 1234 字节对象；这些命令只给该演示进程设置预加载，不将 LD_PRELOAD 导出到整个 shell。

```bash
LD_PRELOAD="$PWD/build/libmem_snapshot.so" ./build/demo &
demo_pid=$!
```

先等终端出现 `Demo PID=...` 再发信号，避免信号早于初始化：

```bash
kill -USR2 "$demo_pid"
```

输出默认写入 `/tmp/mem_snapshot.<PID>.log`。先查看末行，看到 `END seq=1` 后再运行解析命令：

```bash
tail -n 1 "/tmp/mem_snapshot.$demo_pid.log"
python3 tools/symbolize.py "/tmp/mem_snapshot.$demo_pid.log"
kill -TERM "$demo_pid"
wait "$demo_pid"
```

最后的 `wait` 因进程由 SIGTERM 结束而返回非零属于演示预期。不要把上述 PID 变量替换为其他进程。解析结果应包含 `intentional_leak` 及 `examples/demo.c` 行号。

## 接入自己的程序

无需包含头文件或把 malloc 改名。将库放在启动命令前：

```bash
LD_PRELOAD=/absolute/path/libmem_snapshot.so ./your_program
```

以上路径与程序名是占位示例，执行前替换为实际值。另一个终端对该程序实际 PID 执行 `kill -USR2 PID`。信号可以重复发送；每份完整快照以 `BEGIN seq=N` 开始、`END seq=N` 结束。退出前等待 END；本库没有自动退出快照。

建议应用保留 `-g` 调试信息；`-rdynamic` 有助于日志显示主程序中的符号名，保留帧指针有助于栈展开。优化、内联、裁剪符号或缺少展开信息可能影响栈完整性；记录的是返回地址，不保证每层都有函数名/源码行。

离线解析应在原工作目录运行，并使用**生成快照时完全相同的可执行文件、共享库和调试信息**。模块后来被替换可能得到错误行号；当前格式没有模块 build ID 校验。解析器区分 PIE/共享对象的相对地址与非 PIE 的绝对虚拟地址，用返回地址减一定位调用点。无法解析时保留原始地址。

## 配置

| 环境变量 | 默认值 | 含义 |
|---|---|---|
| `MS_OUTPUT` | `/tmp/mem_snapshot.<PID>.log` | 新建输出文件路径；已存在则拒绝启用，不覆盖 |
| `MS_DEPTH` | `16` | 栈深度 0–32；0 只记录地址/大小/ID，无调用栈 |
| `MS_CAPACITY_PER_SHARD` | `4096` | 每片最大记录数；256–65536，必须是 256 的倍数 |

配置在启动时读取一次。默认总容量 262144 条；每片独立限制，因此地址散列分布不均时可能更早漏记。节点按需分批 mmap，释放后复用；节点池保留到进程结束。库本身会增加内存占用，不能仅凭进程 RSS 增长判断应用泄漏。

初始化失败会向 stderr 报错并停用追踪，应用仍继续执行；此时不要发送本工具的信号，SIGUSR2 可能仍是默认终止行为。真实分配函数解析失败则退出 127，防止空指针调用。

## 输出解释

`BEGIN` 包括对象数 `count`、请求大小总和 `bytes`、累计漏记次数 `dropped`、节点存储字节数 `metadata_bytes`、节点大小 `node_size`、最大桶链长 `max_chain`、初始化引导区占用 `bootstrap_bytes` 和配置深度 `depth`。`metadata_bytes` 是节点结构的字节总数，不是 RSS，不含 mmap 页舍入、静态桶、复制缓冲及其他运行库内存。

`ALLOC` 给出对象 ID、指针、请求大小、实际栈深度；接下来的 `FRAME` 给出原始 PC、模块基址、可识别的符号和模块路径。`MAP` 保存输出时的进程映射。ID 在成功 realloc 后重新生成；失败 realloc 保留原 ID。地址可能复用，比较快照时不能仅用地址推断同一个对象。

输出文件权限为 0600，内容可能包含程序路径及地址。文件采用独占新建，已有文件、同一路径启动第二个进程、无权限目录等情况会使追踪停用。END 只表示完整写入已成功，不代表已 fsync 到物理介质。磁盘写入失败会报错且不输出完整 END；当前进程的后续输出保持失败状态，需要修复问题后重新启动采集。

## 并发及覆盖边界

- 信号处理函数只向非阻塞管道写通知；后台线程复制记录后再输出。大量信号允许合并，并不保证每个信号一份快照。
- 快照复制时短暂获取所有分片锁，复制成本随记录数增长；文件 I/O 不占表锁。它是追踪表的一致切面，分配/重分配的在途操作可能暂时不在表中。精确业务对账需先让业务分配静止。
- 跟踪 malloc/free/calloc/realloc 及常见对齐分配入口；提供 cfree/free_sized/free_aligned_sized 兼容释放入口。strdup/reallocarray 等依赖 glibc 内部调用这些入口，不能由接口名称推断全部覆盖。
- 保留 free 的 errno；calloc 溢出不登记；非零 realloc 失败保留旧记录；零大小 realloc 按 glibc 行为处理。这些是 glibc 目标约定，不推广为所有 C 实现的规则。
- SIGUSR2 若在本库初始化前已被占用，拒绝启用；应用初始化后覆盖该处理函数、屏蔽 SIGUSR2 或关闭本库文件描述符会影响快照。调用者需协调该信号和描述符生命周期。
- fork 子进程关闭追踪描述符并停用追踪，父进程继续；子进程 exec 后可重新启用。未验证 `_Fork`、vfork 后的业务执行、运行中 dlopen/dlclose 本库或其他分配器叠加。
- 快照输出期间，应用应避免并发 dlclose 其他共享库：dladdr 返回的模块名/符号名可能随卸载失效，第一版未保证该场景安全。已在输出前卸载的模块可能只能保留原始地址。
- 构造函数之前和本库内部递归产生的分配不记录；启动初始化有界引导区为 1 MiB，只用于符号解析递归，free 不回收该区。
- 静态链接、隐藏内部入口、私有内存池、直接 mmap 及安全执行模式下被忽略的 LD_PRELOAD 不在本工具覆盖内。栈展开依赖运行库，调用频繁时可明显降低吞吐。

## 原理资料

- [glibc：替换 malloc](https://sourceware.org/glibc/manual/latest/html_node/Replacing-malloc.html)
- [dlsym / RTLD_NEXT](https://man7.org/linux/man-pages/man3/dlsym.3.html)
- [异步信号安全](https://man7.org/linux/man-pages/man7/signal-safety.7.html)
