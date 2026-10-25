# UBBench

UBBench 是用于 URMA 时延和带宽测试的性能工具，基于 urma_perftest 演进。涵盖收发（send）、读（read）、写（write）、原子操作（atomic）四类语义，每种语义支持时延（lat）和带宽（bw）测试。区分 server 端和 client 端分别启动 `ub_bench` 进程开展测试并输出结果。

`ub_bench` 兼容 urma_perftest 的命令与参数，并在此基础上新增带宽与时延联合测量（逐包采集完成时延并输出 p99/p999 等分位数）、QPS 限速与扫描（自动寻找带宽-时延拐点）、多线程并行测试等能力。

## 目录

- [编译](#编译)
- [快速开始](#快速开始)
- [特性](#特性)
  - [时延直方图](#时延直方图)
  - [两种带宽模式](#两种带宽模式)
- [命令](#命令)
- [参数](#参数)
  - [新增参数](#新增参数)
  - [原有参数](#原有参数)
- [约束](#约束)
  - [urma_perftest 原有约束](#urma_perftest-原有约束)
  - [ub_bench 新增约束](#ub_bench-新增约束)
- [示例](#示例)
- [报告格式](#报告格式)

## 编译

### 依赖

| 依赖项 | 说明 |
|--------|------|
| UMDK (liburma + liburma_common) | URMA 用户态驱动库 |
| urma 头文件 | urma_api.h / urma_types.h / urma_opcode.h 等，路径 `/usr/include/ub/umdk/urma/` |
| CMake ≥ 3.10 | 构建系统 |
| C 编译器 (C99) | gcc / aarch64-linux-gnu-gcc |
| pthread / libm | 线程 + 数学库 |

> 目标平台：Linux（x86_64 / aarch64）。需先安装 UMDK 以提供 urma 头文件与用户态驱动库。

### 编译步骤

```bash
cd UBBench
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物：`build/ub_bench`

### 编译选项

| 选项 | 说明 |
|------|------|
| `ASAN=enable` | 启用 AddressSanitizer + LeakSanitizer + UBSan |
| `CROSS_COMPILE=<prefix>` | 交叉编译工具链前缀 |

## 快速开始

最小可用命令（TCP 管理通道）：

```bash
# server 端（本机）
ub_bench write_lat -d <DEV_NAME> -s 65536 -n 100000

# client 端（连接 server）
ub_bench write_lat -d <DEV_NAME> -s 65536 -n 100000 -S <SERVER_IP>
```

其中 `<DEV_NAME>` 为 UB 设备名（如 `ubep_beta`，可用 `urma_admin show -a` 查询），`<SERVER_IP>` 为 server 端 IP。带宽测试同理，把 `write_lat` 换成 `write_bw` 即可。

四类语义 × 两种模式共 8 个子命令：

|  | 时延 (lat) | 带宽 (bw) |
|---|---|---|
| 收发 send | `send_lat` | `send_bw` |
| 读 read | `read_lat` | `read_bw` |
| 写 write | `write_lat` | `write_bw` |
| 原子 atomic | `atomic_lat` | `atomic_bw` |

> 原子操作类型默认 `cas`（可用 `-A faa` 切换），`-s` 默认即 8 且不允许改为其他值。

## 特性

`ub_bench` 在 urma_perftest 基础上新增以下特性：

| 特性 | 参数 | 说明 |
|------|------|------|
| 带宽 + 时延同时采集 | sequential 模式默认 | 逐包采集完成时延，同时输出带宽和时延分位数（p99/p999/p9999/p99999） |
| 顺序模式 | `--mode sequential`（默认） | 1 WR 在途逐包发送，支持 QPS 限速和时延采集 |
| 流水线模式 | `--mode pipeline` | 深队列批量收发，对标 urma_perftest（`--bw-only` 时完全等价） |
| QPS 限速 | `--qps <Q>` | 每线程精确发包间隔，总 QPS = threads × qps |
| QPS 扫描 | `--sweep <start:end:step>` | 线性扫描不同 QPS，自动找到带宽时延拐点 |
| 多线程 | `--threads <N>` | 每线程独立 jetty + JFC，无锁并行收发 |
| 纯带宽模式 | `--bw-only` | 跳过时延采集，pipeline 模式下等价 urma_perftest |

### 时延直方图

sequential 模式逐包采集 WR 完成时延（`tcompleted - tposted`），写入 log2 直方图：

- **桶设计**：log2 量级 + 线性子桶。每个 2 倍区间内 512 个线性子桶（`HIST_SUB_BITS=9`），共 28 个量级（`HIST_MAX_MAG=28`），上限 2^28 cycles（≈111ms @2.4GHz），总桶数 28×512=14336。
- **百分位算法**：nearest-rank。从最大桶向下累计计数，累计超过 `(1-p)·N` 时返回该桶代表的时延值。
- **内存开销**：每线程约 56KB；多线程在结束时通过 `hist_merge` 合并为全局直方图后输出分位数。
- **精度说明**：BW 模式的时延是 WR 完成时间（post → CQE），不除以 2，测量方式与 `read_lat`/`atomic_lat` 一致（`rtt_factor=1`）。区别于 `write_lat`/`send_lat` 的 ping-pong 模型（`rtt_factor=2`，报告 RTT/2 单向估算）。如需对标 urma_perftest LAT 基线，使用 `*_lat` 命令。

### 两种带宽模式

| | 顺序模式（sequential，默认） | 流水线模式（pipeline） |
|---|---|---|
| 发包方式 | post 1 WR → poll 1 CQE → QPS 间隔 → post 下一个 | post 满队列(128) → 批量 poll(16) → 重复 |
| jfs_depth | 1（强制） | 128（默认） |
| cq_mod | 1（强制） | 100（默认） |
| 时延采集 | 逐包精确（tcompleted - tposted → hist） | user_ctx 携带时戳，CQE 回传 |
| `--qps` | 支持 | 不支持 |
| `--sweep` | 支持 | 不支持 |
| `--bw-only` | 可关闭时延采集 | 可关闭时延采集 |
| `-y`（infinite） | 不支持 | 支持 |
| 等价 urma_perftest | 否 | `--mode pipeline --bw-only` 等价 |

## 命令

```
Usage: ub_bench command [command options]
  ub_bench   UB benchmark tool
Command syntax:
  read_lat                    Test for read latency.
  write_lat                   Test for write latency.
  send_lat                    Test for send latency.
  atomic_lat                  Test for atomic latency.
  read_bw                     Test for read bandwidth.
  write_bw                    Test for write bandwidth.
  send_bw                     Test for send bandwidth.
  atomic_bw                   Test for atomic bandwidth.
```

## 参数

`ub_bench` 参数分为两类：继承自 urma_perftest 的原有参数，以及 `ub_bench` 新增参数。

### 新增参数

```
Options (ub_bench extensions):
  --mode <mode>               BW test mode: sequential(default) or pipeline.
  --threads <N>               Worker thread count, default 1. Each thread owns a jetty+JFC.
  --qps <Q>                   Per-thread QPS (0=unlimited), BW sequential mode only.
                              Total QPS = threads * qps.
  --sweep <start:end:step>    Linear QPS sweep, per-thread. Each step adds <step>.
  --sweep-duration <sec>      Seconds per sweep step, default 10.
  --bw-only                   Skip latency collection, pure bandwidth test.
```

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--mode` | enum | sequential | BW 测试模式：sequential / pipeline |
| `--threads` | uint32 | 1 | 工作线程数，每线程独立 jetty + JFC（1 线程 = 1 jetty） |
| `--qps` | uint64 | 0 | 每线程 QPS 限速（0 = 不限速），仅 sequential 模式。采用 busy-wait 自旋实现，每个 worker 线程 100% CPU 占用，精度高但耗 CPU |
| `--sweep` | string | — | QPS 扫描 `start:end:step`，仅 sequential 模式 |
| `--sweep-duration` | uint32 | 10 | 每步扫描持续时间（秒）。每步按固定时长运行（由 `time_limit_cycles` 控制，非迭代数）；总扫描点数 = `(end-start)/step + 1` |
| `--bw-only` | flag | false | 跳过时延采集，纯带宽测试 |

### 原有参数

`ub_bench` 保留 urma_perftest 的全部参数：

```
Options:
  -a, --all[order]            Run sizes from 2 till 2^23,
                              default 2^12 for send, 2^16 for others, order: exponent of 2.
  -A, --atomic_type <type>    Specify atomic type, {cas|faa}.
  -b, --simplex_mode          Run with simplex mode(jfs/jfr), duplex jetty mode for reserved.
  -B, --bidirection           Measure bidirectional bandwidth (default unidirectional).
  -c, --jfc_inline            Enable jfc_inline to upgrade latency performance.
  -C, --jfc_depth <dep>       Size of jfc depth (default 4096 for bw, 1024 for ip bw, 1 for lat.
  -d, --dev <dev_name>        The name of ubep device.
  -D, --duration <second>     Run test for a customized period of seconds, this cfg covers iters.
  -e, --use_jfce              use jfc event.
  --eid_idx                   Specified eid index of device.
  -E, --err_timeout <time>    the timeout before report error, ranging from [0, 31],
                              the actual timeout in usec is caculated by: 4.096*(2^err_timeout).
  -f, --use_flat_api          Choose to use flat API, only works in SIMPLEX mode.
  -F, --cpu_freq_f            To report warnings when CPU frequency drifts, default as NOT.
  -h, --help                  Show help info.
  -I, --inline_size <size>    Max size of message to be sent in inline (default 0).
  -j, --share_jfr <true/false> share jfr on create jetty.
  -J, --jettys <num of jetty> Num of jettys(default 1).
  -K, --token_policy <policy> default 0: NONE, 1: PLAIN_TEXT, 2: SIGNED, 3: ALL_ENCRYPTED.
  -n, --iters <iters>         Number of exchanges (at least 5, default 10000).
  -N, --no_peak               Cancel peak-bw calculation.
  -l, --jfs_post_list <size>  Post list of send WQEs of <list size> size.
  -L, --lock_free             Jetty's interior is unlocked.
  -O, --priority              set the priority of JFS, ranging from [0, 15].
  -p, --trans_mode <mode>     Transport mode: 0 for RM(default), 1 for RC, 2 for UM.
  -P, --port <id>             TCP mode: server port, default 21115.
                              UB mode: required server mgmt jetty id, same value on both sides.(less than 1024)
  -Q, --cq_mod <num>          Generate Cqe only after <--cq_mod> completion.
  -r, --jfr_post_list <size>  Post list of receive WQEs of <list size> size.
  -R, --jfr_depth <dep>       Size of jfr depth (default 512 for BW, 1 for LAT).
  -s, --size <size>           Size of message to exchange (default 2).
  -S, --server <addr>         TCP mode: server ip for bind or connect, default: 127.0.0.1.
                              UB mode: server mgmt eid (required for client).
  -T, --jfs_depth <dep>       Size of jfs depth (default 128 for BW, 1 for LAT).
  -u, --uboe                  Enable uboe (default false), sip, dip are required, dscp, vlan, sl are optional.
  -V, --version               Show the version.
  -w, --warm_up               Choose to use warm_up function, only for read/write/atomic bw test.
  -y, --infinite[second]      Run perftest infinitely, only available for BW test.
                              Print period for infinite mode, default 2 seconds.
                              In infinite BW mode, pressing Ctrl-C on either side notifies the peer to exit.
  --inf_period_ms             Print period (ms) for infinite mode. Must be a multiple of 50.
                              if set, value of infinite will be overwrite.
  --rate_limit <rate>         Set the maximum rate of sent packages. default unit is [Gbps].
  --rate_units <units>        Set the units for rate, MBps (M), Gbps (G)(default) or Kpps (P).
  --burst_size <size>         Set the amount of pkts to send in a burst when using rate limiter.
  --order_type <type>         Order type: 0 for default order,
                   1 for OT (target order), 2 for OI(init order), 3 for OL(layer order), 4 for NO(no order).
  --mgmt_proto <tcp|ub>       Management channel protocol, default: tcp.
  --enable_credit             enable send credit, default: disable.
  --credit_threshold <num>    Exceed the threshold and do not send, default: jfr_depth * 3 / 4.
  --credit_notify_cnt <num>   Notify the send side after recv packets, default: jfr_depth / 4.
  --jettys_pre_jfr <num>      How many jettys share a jfr, default: jettys.
  --seg_pre_jetty             Enable a segment for each Jetty, default: disable.
  --enable_imm                Enable immediate data for write or send, default: disable.
  --enable_err_continue       Enable continue running when cr erros, default: disable.
  --enable_notify            Enable write_with_notify, default: disable.
  --enable_sync_stream        Enable synchronized multi-stream transmission.
  --enable_user_tp            Enable user tp for UB device, if enable,UVS is not required. default: disable.
  --oor_en                    Enable out of order for user_tp, default: disable.
  --spray_en                  Enable multipathing for user_tp, default: disable.
  --cc_en                     Enable congestion control for user_tp, default: disable.
  --cc_alg <num>              Set congestion Control Algorithm for user_tp, [0, 7], default: 0.
  --retry_num  <num>          Set retry num for user_tp, default: 7.
  --ack_timeout <num>         Set ack timeout for user_tp, default: 15.
  --sge_num <num>             Set sge_num for wr, default: 1.
  --enable_write_dirty <time> Enable write dirty and set the period of write dirty, default: disable.
  --pair_num <num>            Enable multiplayer model and set the number of connection, default: disable.
  --sip <ip>                  Set source ip address.
  --dip <ip>                  Set dest ip address.
  --dscp <dscp>               Set dscp .
  --vlan <vlan_id>            Set vlan_id .
  --sl <sl>                   Set sl .
  --async_import              Enable asynchronous connection establishment
  --tp_aware                  Enable tp aware connect, default: disable.
  --tp_reuse                  Reuse tp in RM mode if enable tp aware, default: disable.
  --ctp                       Use ctp, default: disable.
  --jetty_id                  Set the jetty_id, default: 0.
  --wait_jfc_timeout          Set timeout parameter for urma_wait_jfc (in milliseconds),
                              timeout = 0: return immediately even if no events are ready,
                              timeout = -1: an infinite timeout,
                              default: 1000(1s).
  --page_size                 Set page size, default: 4096.
  --hugepage_size <size>      Page size for allocated memory. Only support 2MB or 1GB currently.
  --mgmt_addr <eid>           UB only: local eid string for mgmt channel (required in UB mode).
  --bond_mode                 Set bonding device mode, support: standalone, active_backup, balance.
                              default: standalone.
  --bond_level                Set bonding device level, support: iodie, port, default: iodie.
  --bw_unit <unit>            Set bandwidth display unit: KB, MB, or GB (default: MB).
  --va <address>              Allow user to set address for local_buf, (Both decimal and hexadecimal).
  --share_jfs                 Create ONLY ONE jfs in multi-play-multi mode bw test.
  --min_size                  Set min size in random io size, only for bw mode.
  --max_size                  Set max size in random io size, only for bw mode.
```

## 约束

### urma_perftest 原有约束

1. 冲突的配置原则上不允许同时配置，例如 -a 和 -s、-D 和 -n 等；
2. 参数 -d 和 client 端的 -S 为必填项；
3. 写操作不支持配置 -e；
4. 读/原子操作未使用 inline_size，配置 -I 会提示不生效，并且 inline_size 强制取 0；
5. -J 多个 jetty 仅能在带宽测试中配置，在时延测试中配置会返回错误；
6. -N 无峰值计算配置仅用于带宽测试，时延测试配置会返回失败；
7. 当前对 -n、-T、-J、-R、-Q 等参数有严格的范围限制，超出会告警提示并返回失败；
8. 原子操作语义配置 -s 不等于 8 时，产生告警提示并返回失败；
9. 当配置 jfs_post_list/jfr_post_list 超过 1，在带宽测试迭代模式(-n)下，若迭代次数不能整除 jfs_post_list/jfr_post_list 则产生告警提示并返回失败；
10. 当配置 jfs_post_list 超过 1，在带宽测试中，cq_mod 为 0 时，将强制取其等于 jfs_post_list；
11. 当配置 jfs_post_list 超过 1，在带宽测试中，cq_mod 不为 0 时，若 jfs_post_list 不能整除 cq_mod，则产生告警提示并返回失败；
12. 双边操作 -B 只能配合带宽测试，时延测试配置 -B 会报错；
13. 配置 -f 只能在配置 -b 的前提下，否则会报错；

### ub_bench 新增约束

1. `--mode pipeline` 仅用于 BW 测试；
2. `--qps` 仅用于 BW sequential 模式，与 `--sweep`、`--rate_limit` 互斥；
3. `--sweep` 仅用于 BW sequential 模式，不支持 send_bw（服务端收包为计数驱动，无法匹配时间驱动的 sweep 步长）和 bidirection；
4. `--mode sequential` 不支持 `--infinite`(-y)、`--bidirection`(-B)、`--all`(-a)；
5. `--threads > 1` 仅用于 BW 测试，不支持 `--infinite`(-y)；
6. `--bidirection`(-B) 不支持 sequential 模式、多线程、pipeline + 非 bw_only；
7. `--bw-only` 仅用于 BW 测试；
8. sequential 模式强制 `jfs_depth=1`、`cq_mod=1`、`jfr_post_list=1`（用户配置的 -T、-Q、-r 被覆盖）；同时自动设 `no_peak=false`（非 bw_only 且非 duration 模式时）；
9. pipeline + 非 bw_only 强制 `cq_mod=1`，并自动设 `no_peak=false`（非 duration 模式时）；此时若 `jettys > 1` 且 `threads == 1`，自动提升 `threads = jettys`（每 jetty 一个 worker）；
10. `--threads > 1` 时强制 `jettys = threads`（1 线程 = 1 jetty）；
11. send_bw 服务端：sequential + 非 credit + 非 jfce 时支持多线程收包；pipeline / credit / jfce 模式强制 `threads=1`（需共享 CQ）；sequential + 多线程 + `--share_jfr` 时自动设 `jettys_pre_jfr=1`（每 jetty 独立 jfr/JFC，支持 per-thread 收包）；
12. `--sweep` 与 `--qps`、`--rate_limit` 互斥；sweep 模式下 `time_type` 被清零（既非 duration 也非 iterations），由每步 `time_limit_cycles` 控制时长。

## 示例

### 时延测试

```bash
# server 端
ub_bench write_lat -d <DEV_NAME> -s 65536 -n 100000

# client 端
ub_bench write_lat -d <DEV_NAME> -s 65536 -n 100000 -S <SERVER_IP>
```

### 带宽 + 时延联合测试（sequential 模式，默认）

```bash
# server 端
ub_bench write_bw -d <DEV_NAME> -D 10

# client 端
ub_bench write_bw -d <DEV_NAME> -D 10 -S <SERVER_IP>
```

### 多线程带宽测试

```bash
# server 端
ub_bench write_bw -d <DEV_NAME> --threads 4 -D 10

# client 端
ub_bench write_bw -d <DEV_NAME> --threads 4 -D 10 -S <SERVER_IP>
```

### send_bw 多线程

```bash
# server 端
ub_bench send_bw -d <DEV_NAME> --threads 4 -D 10

# client 端
ub_bench send_bw -d <DEV_NAME> --threads 4 -D 10 -S <SERVER_IP>
```

### QPS 限速测试

```bash
# 每线程 100K QPS，4 线程，总 400K QPS
ub_bench write_bw -d <DEV_NAME> --threads 4 --qps 100000 -D 10 -S <SERVER_IP>
```

### QPS 扫描（找带宽时延拐点）

```bash
# 从 100K 到 1M QPS，步长 100K，每步 10 秒
ub_bench write_bw -d <DEV_NAME> --threads 4 \
    --sweep 100000:1000000:100000 --sweep-duration 10 -S <SERVER_IP>
```

### 纯带宽测试（pipeline 模式，对标 urma_perftest）

```bash
# server 端
ub_bench write_bw -d <DEV_NAME> --mode pipeline --bw-only -y

# client 端
ub_bench write_bw -d <DEV_NAME> --mode pipeline --bw-only -y -S <SERVER_IP>
```

## 报告格式

### 客户端报告

sequential 模式与 pipeline 模式的输出格式不同：

**sequential 模式**：

| 列 | 说明 |
|---|---|
| bytes | 消息大小 |
| iterations | 总收发包数 |
| QPS_actual | 实际吞吐速率（包/秒） |
| BW_avg | 平均带宽 |
| MsgRate | 消息速率（Mpps） |
| t_min / t_max | 最小 / 最大时延（微秒） |
| t_median / t_avg | 中位数 / 平均时延（微秒） |
| 99% / 99.9% / 99.99% / 99.999% | 时延分位数（微秒，nearest-rank） |

> `--bw-only` 时省略 t_min~99.999% 时延列。

**pipeline 模式**：

| 列 | 说明 |
|---|---|
| actual_QPS | 实际吞吐速率（包/秒） |
| target_QPS | 目标 QPS（pipeline 模式不支持 `--qps`，该列恒为 0） |
| BW_avg | 平均带宽 |
| MsgRate | 消息速率（Mpps） |
| t_min / t_median / t_avg | 最小 / 中位 / 平均时延（微秒） |
| P99 / P99.9 / P99.99 / P99.999 / Pmax | 时延分位数及最大值（微秒） |

### 服务端报告

| 命令 | 服务端输出 | 说明 |
|------|-----------|------|
| send_bw | BW only（无时延列） | 服务端独立计数，client 端输出完整报告（BW + 时延） |
| write_bw / read_bw / atomic_bw | 完整报告（BW + 时延） | 服务端不独立测量，数据来自客户端 sync |

### 时延语义

BW 模式的时延是 WR 完成时间（post → CQE），不除以 2，测量方式与 `read_lat`/`atomic_lat` 一致（`rtt_factor=1`）。区别于 `write_lat`/`send_lat` 的 ping-pong 模型（`rtt_factor=2`，报告 RTT/2 单向估算）。如需对标 urma_perftest LAT 基线，使用 `*_lat` 命令。


