# PostgreSQL CPU PMC 大作业

在 PostgreSQL 源码中实现课程的 HaloPMC 基础功能与低开销读取扩展。工作分支为 `homework/halo-pmc`，基于本次克隆的 `cf44f3fa19`（PostgreSQL 20devel）。

## 已完成的功能

| 作业要求 | 实现 |
|---|---|
| 增加 PMC 源码和头文件并编译 | `src/backend/utils/misc/halo_pmc.c`、`src/include/utils/halo_pmc.h`，接入 GNU make 与 Meson |
| 完成初始化 | 在连接后端初始化 GUC；`sql/init.sql` 创建和填充 `public.pmc_name` 事件表 |
| 配置要观察的计数器 | 支持课件的 `halo_watch_pmc_list`、`halo_pmc_list`，以及规范名称 `halo_pmc.events` |
| 打开计数器 运行目标代码 输出结果 | 默认观察 `MultiExecPrivateHash`，输出指令、周期、L1D 事件及命中率估算 |
| 循环中的低开销观察 | 在元组散列与插入前后读快照，累加差值；支持 mmap + RDPMC，循环内不反复启停计数器 |

## 直接复现实验

在 WSL 的仓库根目录执行：

```sh
sh homework/pmc/build.sh
sh homework/pmc/run_demo.sh
```

构建使用 `-O2 -g`、调试符号及断言，安装到仓库内的 `.pmc-install`。脚本创建独立测试数据库，只开放临时目录中的 Unix socket，结束时停止该数据库，不占用现有数据库端口。数据文件和构建产物保留在 `.pmc-build` 内以便检查。

本次机器的依赖已安装。换到新的 Ubuntu 环境时，需要 GCC、make、bison、flex、readline/zlib/ICU 开发包，以及开启 TAP 测试所需的 Perl IPC::Run。对应包可安装为：

```sh
sudo apt-get install build-essential bison flex libreadline-dev zlib1g-dev \
  libicu-dev libipc-run-perl
```

演示 SQL 是 `homework/pmc/sql/demo.sql`，包含初始化、强制单后端 Hash Join、普通观察、RDPMC 循环观察和 `read()` 对照。查询结果均为 **8990**。原始输出及验证结果见 [results](results/summary.md)。

## 在自己的测试数据库中使用

请连接由本仓库 `.pmc-install/bin/postgres` 启动的实例，以数据库超级用户执行以下操作：

```sql
-- 在 psql 中，按实际位置指定这个初始化脚本
\i homework/pmc/sql/init.sql

SET halo_pmc.enabled = on;
SET halo_pmc.target = hash;
SET max_parallel_workers_per_gather = 0;
SET enable_mergejoin = off;
SET enable_nestloop = off;

-- 与实践课第 9 页相同的配置方式，事件名不区分大小写
SET halo_watch_pmc_list = Instructions,
  PERF_COUNT_HW_CACHE_L1D_RD, PERF_COUNT_HW_CACHE_L1D_MISS_RD;

-- 先创建合适的数据表，再执行 Hash Join
SELECT count(*) FROM r JOIN s ON r.id = s.id WHERE s.value > 100;

-- 扩展：每个建表元组都观察散列与插入代码
SET halo_pmc.sample_every = 1;
SET halo_pmc.read_method = auto;
SELECT count(*) FROM r JOIN s ON r.id = s.id WHERE s.value > 100;

SET halo_pmc.enabled = off;
```

`instructions` 和 `cycles` 始终加入观察组。其余事件从 `public.pmc_name` 加载，在计数器启用前完成查表与配置。设置空事件列表时恢复默认四项事件，不要求先创建事件表。

`halo_pmc.events`、`halo_pmc_list`、`halo_watch_pmc_list` 的最后一次赋值决定下一次观察使用的事件集合。两个无点号的课程名称只用于连接内的 `SET`；在 `postgresql.conf` 或连接启动选项中使用带点号的 `halo_pmc.events`。事件表内容改变后，重新 `SET` 事件列表使当前后端重新加载。

## 可用参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `halo_pmc.enabled` | `off` | 超级用户启用观察，普通查询默认不打开 PMC |
| `halo_pmc.target` | `hash` | `hash` 观察私有 Hash 建表；`executor` 观察 ExecutorRun |
| `halo_pmc.events` | 空字符串 | 逗号分隔的事件表名称；最多 8 项，包含自动加入的指令和周期 |
| `halo_pmc.sample_every` | `0` | 每 N 个目标片段读一对快照；0 只观察完整区域 |
| `halo_pmc.read_method` | `auto` | `auto` 优先 RDPMC、不可用时用 read；`read` 强制系统调用；`rdpmc` 不可用时跳过该片段 |

各参数均为超级用户设置。参数在连接后端初始化，在第一次真正观察时为当前线程创建事件，不在 postmaster 中提前打开后再交给子进程。

## 低开销读取的实现

事件组在目标函数进入时启用一次，退出时关闭一次。循环内的操作是：

```text
快照 A
目标代码：计算散列值并插入 Hash 表
快照 B
累计 B - A
```

`HashBuild` 的整段计数还包括从子计划取元组和建表收尾；`segment_raw_counts` 只累加被选中的散列/插入片段。每 N 个元组采样时，该项只代表采样到的片段，不自动外推为全部元组。

快速读取使用内核 perf mmap 元数据页：检查 `cap_user_rdpmc`、有效索引和计数器位宽，用 seqlock 验证一致性，执行带执行屏障的 RDPMC，并将正确符号扩展后的值加上内核 offset。没有硬编码物理计数器编号。序列锁最多重试 64 次，无法取得有效快照时跳过或按配置回退。

每次快照读取全部活动事件，所以输出的 `rdpmc_calls` 是**快照次数**，不是 RDPMC 指令条数。`read_calls` 是分组 `read()` 次数。本机四事件、8990 个元组、每元组一对快照时：RDPMC 快照 17980 次，`read()` 仅有完整区间起止的 2 次。

快照本身仍有开销，不同事件也不是在完全同一时刻读取。片段差值包含读取路径的部分开销；很短的代码应扩大观察范围，并用多次实验核对。完整区域输出使用区间的 `time_enabled/time_running` 缩放；循环片段明确输出原始计数，不把零运行时间或缺失事件填成零。

## 事件表和 CPU 差异

`pmc_name` 保存 `name`、`pmctype`、`pe_config`、`config1`、`config2` 以及 CPU 匹配条件和说明。可以在核对 CPU 官方事件定义后增加行，随后通过参数选用。

实践课示例的 `MEM_INST_RETIRED_ALL_STORES` 是 Intel 的退休存储事件。初始化脚本提供经过限定的 Skylake family 6/model 94 示例，编码为 `0x82d0`。本次机器是 AMD family 26/model 68，使用独立名称 `AMD_LS_DISPATCH_STORES`（`0x229`）或 `AMD_LS_DISPATCH_ALL`（`0x729`），它们统计派发操作，包含推测执行，不能解释为退休存储指令。CPU 匹配不满足时明确拒绝配置。

L1D 百分比采用 `100 × (1 - READ.MISS / READ.ACCESS)`，属于 Linux 通用事件映射口径的估算，具体含义依 CPU 与内核而异，不是 L1 指令缓存命中率，也不保证等于严格的退休 load 命中概率。

## 测试和结果

```sh
make -C homework/pmc test
make -C homework/pmc sanitize
make -C homework/pmc benchmark
sh homework/pmc/run_demo.sh

# 完整 PostgreSQL 回归测试
make -C .pmc-build/autoconf check
```

已执行 19 项数据库集成测试和 243 项 PostgreSQL 回归测试，均通过；计数器核心的故障注入、RDPMC 一致性检查以及 AddressSanitizer/UndefinedBehaviorSanitizer 检查均通过。GNU make 与 Meson/Ninja 的 postgres 构建均已验证。实测和限制见 [实验报告](results/summary.md)。

计数器不可用时产生提示并保留正常查询结果；不支持的可选事件输出 `null`。`auto` 可以回退到系统调用，强制 `rdpmc` 时会显示跳过的样本数。程序不自动更改系统 perf 权限。原始事件列表不存在、表结构不符合要求或 CPU 不匹配属于配置错误，会明确报错。

当前测量仅覆盖当前后端线程。默认目标是 `MultiExecPrivateHash`，没有汇总并行 Hash 或并行 worker；演示中已关闭并行。嵌套执行避免重复输出；错误时取消观察并清理状态，下一条查询可正常重新观测。退出后端时关闭文件描述符及 mmap 映射。

## 文件与提交

核心实现位于 `src/backend/utils/misc/halo_pmc.c` 和 `src/include/utils/halo_pmc.h`。初始化接入 `src/backend/tcop/postgres.c`，执行器及 Hash 建表观测接入 `execMain.c`、`nodeHash.c`。本目录保存初始化 SQL、示例、测试和实验输出。

源码和实验结果在本地分支中准备好后，可自行提交并推送：

```sh
git status
git add src/backend/utils/misc/halo_pmc.c src/include/utils/halo_pmc.h \
  src/backend/utils/misc/Makefile src/backend/utils/misc/meson.build \
  src/backend/tcop/postgres.c src/backend/executor/execMain.c \
  src/backend/executor/nodeHash.c homework/pmc README.md
git commit -m "Add CPU PMC observation and low-overhead RDPMC snapshots"
git push -u origin homework/halo-pmc
```

`.pmc-build` 和 `.pmc-install` 为本次机器的构建产物，应保留在本地。大体积课件和课程视频作为输入保留在原目录，不放进源码仓库。

## 资料

- 课程《PG北大内核课程_实践课》：第 9 页使用流程，第 11～13 页数据与接口，第 15 页循环内观察扩展。
- [Linux perf_event_open 手册及 mmap/RDPMC 协议](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- [Linux perf 权限说明](https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html)
- [Linux 6.18 AMD Zen5 事件定义](https://github.com/torvalds/linux/blob/v6.18/tools/perf/pmu-events/arch/x86/amdzen5/load-store.json)
- [Intel Skylake 官方事件定义](https://github.com/intel/perfmon/blob/main/SKL/events/skylake_core.json)
