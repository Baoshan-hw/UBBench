# UBBench

URMA 性能基准测试工具，基于 urma_perftest 演进，新增带宽与时延同时采集、QPS 限速、QPS 扫描等特性。

## 1. 功能概述

涵盖收发（SEND）、读（READ）、写（WRITE）、原子操作（ATOMIC）四类语义，每种语义支持时延和带宽测试。区分 server 端和 client 端分别起 ub_bench 进程开展测试并输出测试结果。

### 与 urma_perftest 的关系

- 完全兼容 urma_perftest 的所有命令和参数
- LAT 模式（write_lat/read_lat/send_lat/atomic_lat）代码与 urma_perftest 一致
- BW 模式新增两种发包方式：
  - **顺序模式（sequential，默认）**：post 一个 → 等完成 → QPS 间隔 → post 下一个，逐包精确采集时延
  - **流水线模式（pipeline）**：post 满队列 → 批量 poll → 重复，最大带宽吞吐

### 新增特性

| 特性 | 参数 | 说明 |
|------|------|------|
| 带宽+时延同时采集 | 默认行为 | 顺序模式逐包采集时延（histogram），同时输出带宽 |
| QPS 限速 | `--qps` | 每线程限速，总 QPS = threads × qps |
| QPS 扫描 | `--sweep` | 线性扫描 QPS，自动找到带宽时延拐点 |
| 多线程 | `--threads` | 每线程独立 jetty + CQ，无锁并行 |
| 纯带宽模式 | `--bw-only` | 跳过时延采集，最大化带宽 |

## 2. 命令格式

```
Usage: ub_bench command [command options]
ub_bench URMA benchmark tool
Command syntax:
  read_lat       Test for read latency.
  write_lat      Test for write latency.
  send_lat       Test for send latency.
  atomic_lat     Test for atomic latency.
  read_bw        Test for read bandwidth.
  write_bw       Test for write bandwidth.
  send_bw        Test for send bandwidth.
  atomic_bw      Test for atomic bandwidth.
```

## 3. 使用示例

### 时延测试

```bash
# server 端
ub_bench write_lat -d <DEV_NAME> -s [SIZE] -n [ITERATIONS]

# client 端
ub_bench write_lat -d <DEV_NAME> -s [SIZE] -n [ITERATIONS] -S <SERVER_IP>
```

### 带宽测试（pipeline 模式，对标 urma_perftest）

```bash
# server 端
ub_bench write_bw -d <DEV_NAME> -s [SIZE] --mode pipeline --bw-only

# client 端
ub_bench write_bw -d <DEV_NAME> -s [SIZE] -S <SERVER_IP> --mode pipeline --bw-only
```

### 带宽 + 时延同时采集（sequential 模式）

```bash
# server 端
ub_bench write_bw -d <DEV_NAME> -s [SIZE]

# client 端
ub_bench write_bw -d <DEV_NAME> -s [SIZE] -S <SERVER_IP>
```

输出示例：
```
---- ub_bench Report ----
 actual_QPS  target_QPS  BW_avg[MiB/s]  MsgRate[Mpps]  t_min[us]  t_median[us]  t_avg[us]  P99[us]  P99.9[us]  P99.99[us]  P99.999[us]  Pmax[us]
 769160      0           48073          0.769160       3.25       12.56          12.74      20.02    22.76      25.44       34.32        297.03
```

### 多线程带宽测试

```bash
# 10 线程，CTP 模式
numactl --cpunodebind=0 --membind=0 ub_bench write_bw -d <DEV_NAME> --ctp -S <SERVER_IP> -s 1048576 --threads 10
```

### QPS 限速测试

```bash
# 每线程 100K QPS，10 线程，总 1M QPS
ub_bench write_bw -d <DEV_NAME> --ctp -S <SERVER_IP> -s 65536 --threads 10 --qps 100000
```

### QPS 扫描（找带宽时延拐点）

```bash
# 从 100K 到 1.5M QPS，步长 100K，每步 10 秒
ub_bench write_bw -d <DEV_NAME> --ctp -S <SERVER_IP> -s 65536 --threads 10 \
    --sweep 100000:1500000:100000 --sweep-duration 10
```

### CTP 传输层

```bash
# 加 --ctp 使用 CTP 传输层（bonding 设备）
ub_bench write_bw -d bonding_dev_0 --ctp -S <SERVER_IP> -s 65536
```

## 4. 新增参数说明

| 参数 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| `--mode <mode>` | string | BW 测试模式：sequential（默认）或 pipeline | sequential |
| `--threads <N>` | uint32 | 工作线程数，每线程独立 jetty + CQ | 1 |
| `--qps <Q>` | uint64 | 每线程 QPS 限速（0=不限速），仅 BW sequential 模式 | 0 |
| `--sweep <start:end:step>` | string | 线性 QPS 扫描，每步增加 step，仅 sequential 模式 | - |
| `--sweep-duration <sec>` | uint32 | 每个 sweep 步骤持续时间 | 10 |
| `--bw-only` | bool | 跳过时延采集，纯带宽测试 | false |

### 参数约束

- `--mode pipeline` 仅用于 BW 测试
- `--qps` 仅用于 BW sequential 模式
- `--sweep` 仅用于 BW sequential 模式，不支持 bidirection 和 send_bw
- `--mode sequential` 不支持 `--infinite`、`--bidirection`、`--all`
- `--threads` 不支持 `--infinite`
- `--bidirection` 不支持 sequential 模式、多线程、pipeline + 时延采集

## 5. 两种模式对比

| | 顺序模式（sequential） | 流水线模式（pipeline） |
|---|---|---|
| **发包方式** | post 1 个 → poll 1 个 → post 下一个 | post 满队列(128) → 批量 poll |
| **jfs_depth** | 1 | 128 |
| **cq_mod** | 1（每 WR 生成 CQE） | 100（100 WR 生成 1 个 CQE） |
| **时延采集** | 逐包精确（tcompleted - tposted） | user_ctx 携带时间戳，CQE 带回 |
| **--qps** | 支持 | 不支持 |
| **--sweep** | 支持 | 不支持 |
| **--bw-only** | 可关闭时延采集 | 可关闭时延采集 |
| **带宽** | 大包接近 link，小包需多线程 | 接近 link |
| **时延含义** | 真实单包完成时间 | 含排队等待（系统时延） |

## 6. 编译

### 依赖

- UMDK 用户态库（urmacore 等）
- cmake >= 3.10
- gcc / g++

### 编译步骤

```bash
cd UBBench
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物：`build/ub_bench`

## 7. 原有参数

ub_bench 保留了 urma_perftest 的全部参数，包括但不限于：

`-d/--dev` `-S/--server` `-s/--size` `-n/--iters` `-D/--duration` `-J/--jettys` `-T/--jfs_depth` `-Q/--cq_mod` `-a/--all` `-B/--bidirection` `-b/--simplex_mode` `--ctp` `--ctp` `--rate_limit` `--burst_size` `--enable_credit` `--enable_imm` `--enable_notify` `--use_jfce` `--use_flat_api` `--lock_free` `--trans_mode` `--inline_size` `--jfr_depth` `--jfs_post_list` `--sge_num` `--pair_num` `--bond_mode` `--bond_level` 等。

执行 `ub_bench -h` 查看完整参数列表。
