/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: sequential mode and sweep for ub_bench
 * Create: 2026-09-16
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdbool.h>

#include "ub_bench_run.h"
#include "ub_bench_run_test.h"
#include "urma_api.h"

static const char *g_seq_sync_before[] = {"seq_before_read_bw", "seq_before_write_bw",
                                          "seq_before_send_bw", "seq_before_atomic_bw"};
static const char *g_seq_sync_after[] = {"seq_after_read_bw", "seq_after_write_bw",
                                         "seq_after_send_bw", "seq_after_atomic_bw"};

static void *sequential_worker(void *arg)
{
    bench_thread_arg_t *t = (bench_thread_arg_t *)arg;
    perftest_context_t *ctx = t->ctx;
    perftest_config_t *cfg = t->cfg;
    run_test_ctx_t *run_ctx = &ctx->run_ctx;
    uint32_t id = t->jetty_start;

    urma_status_t status;
    urma_cr_t cr;
    uint64_t scnt = 0;

    uint64_t interval = (cfg->qps > 0) ? (uint64_t)(t->cpu_mhz * PERFTEST_M / cfg->qps) : 0;
    uint64_t next_deadline = 0;

    urma_jfs_wr_t *wr = &run_ctx->jfs_wr[id];
    bool collecting = (cfg->time_type.bs.iterations == 1);
    uint64_t sweep_start = 0;
    bool sweep_active = (t->time_limit_cycles > 0);

    if (sweep_active) {
        sweep_start = get_cycles();
        collecting = true;
    }
    t->start_time = get_cycles();

    while (1) {
        if (sweep_active) {
            uint64_t elapsed = get_cycles() - sweep_start;
            if (elapsed >= t->time_limit_cycles) {
                break;
            }
        }

        if (cfg->time_type.bs.duration == 1) {
            if (run_ctx->state == END_STATE) {
                break;
            }
            if (run_ctx->state == START_STATE) {
                if (!collecting) {
                    collecting = true;
                    hist_reset(&t->hist);
                    t->start_time = get_cycles();
                }
            } else {
                collecting = false;
            }
        }

        if (interval > 0 && next_deadline > 0) {
            while (get_cycles() < next_deadline) {
            }
        }

        uint64_t tposted = get_cycles();
        next_deadline = tposted + interval;

        wr->flag.bs.complete_enable = 1;

        urma_jfs_wr_t *bad_wr = NULL;
        if (cfg->jetty_mode == PERFTEST_JETTY_SIMPLEX) {
            status = urma_post_jfs_wr(ctx->jfs[id], wr, &bad_wr);
        } else {
            status = urma_post_jetty_send_wr(ctx->jetty[id], wr, &bad_wr);
        }
        if (status != URMA_SUCCESS) {
            LOG_ERROR("Failed to post WR, thread %u, scnt %lu.\n", id, scnt);
            t->end_time = get_cycles();
            return (void *)(intptr_t)(-1);
        }

        int cqe_cnt;
        do {
            cqe_cnt = urma_poll_jfc(ctx->jfc_s[id], 1, &cr);
        } while (cqe_cnt == 0 &&
                 (cfg->time_type.bs.iterations == 1 || run_ctx->state != END_STATE));

        if (cqe_cnt < 0) {
            LOG_ERROR("Failed to poll jfc, thread %u.\n", id);
            t->end_time = get_cycles();
            return (void *)(intptr_t)(-1);
        }
        if (cqe_cnt == 0) {
            break;
        }
        if (cr.status != URMA_CR_SUCCESS) {
            LOG_ERROR("Bad CR status, thread %u, status %d.\n", id, (int)cr.status);
            t->end_time = get_cycles();
            return (void *)(intptr_t)(-1);
        }

        uint64_t tcompleted = get_cycles();
        uint64_t delta = tcompleted - tposted;
        /* BW mode: delta = post + one-way data transfer + small ACK return + poll
           This is NOT a full data round-trip (only LAT ping-pong is).
           Do NOT divide by 2 in BW mode. */

        if (collecting) {
            hist_add(&t->hist, delta);
        }

        if (cfg->size <= (ctx->page_size / PERFTEST_BUF_NUM)) {
            uint32_t local_sge_idx = id * PERFTEST_SGE_NUM_PRE_WR * cfg->sge_num + cfg->sge_num;
            urma_sge_t *local_sge = &run_ctx->jfs_sge[local_sge_idx];
            increase_loc_addr(local_sge, cfg->size, scnt,
                              (uint64_t)ctx->local_buf[id] + ctx->buf_size,
                              cfg->cache_line_size, ctx->page_size);

            if (cfg->api_type != PERFTEST_SEND) {
                uint32_t remote_sge_idx = id * PERFTEST_SGE_NUM_PRE_WR * cfg->sge_num;
                urma_sge_t *remote_sge = &run_ctx->jfs_sge[remote_sge_idx];
                increase_loc_addr(remote_sge, cfg->size, scnt,
                                  get_remote_seg_va(ctx, cfg, id),
                                  cfg->cache_line_size, ctx->page_size);
            }
        }

        scnt++;

        if (cfg->time_type.bs.iterations == 1 && scnt >= cfg->iters) {
            break;
        }
    }

    t->end_time = get_cycles();
    return NULL;
}

typedef struct {
    uint64_t actual_total_qps;
    uint64_t target_total_qps;
    uint64_t per_thread_qps;
    double bw_avg;
    double msg_rate;
    double t_min;
    double t_median;
    double t_avg;
    uint64_t p99;
    uint64_t p999;
    uint64_t p9999;
    uint64_t p99999;
    uint64_t pmax;
    uint64_t total_count;
} sweep_result_t;

static void print_sequential_report(perftest_config_t *cfg, uint64_t total_count,
                                    uint64_t global_start, uint64_t global_end,
                                    const latency_hist_t *merged_hist)
{
    double cpu_mhz = get_cpu_mhz(false);
    if (cpu_mhz <= 0.0) {
        LOG_ERROR("Failed to get cpu mhz for report.\n");
        return;
    }

    double cycles_to_units = cpu_mhz * PERFTEST_M;
    double unit_ratio = (double)PERFTEST_BW_MB;
    const char *unit_str = "MiB";
    if (cfg->bw_unit == PERFTEST_KB) {
        unit_ratio = (double)PERFTEST_BW_KB;
        unit_str = "KiB";
    } else if (cfg->bw_unit == PERFTEST_GB) {
        unit_ratio = (double)PERFTEST_BW_GB;
        unit_str = "GiB";
    }

    uint64_t cycles_sum = global_end - global_start;
    double bw_avg = ((double)cfg->size * total_count * cycles_to_units) /
                    ((double)cycles_sum * unit_ratio);
    double actual_qps = (double)total_count * cycles_to_units / (double)cycles_sum;
    uint64_t target_qps = (cfg->qps > 0) ? (uint64_t)cfg->threads * cfg->qps : 0;
    double msg_rate = actual_qps / (double)PERFTEST_M;

    LOG_QUIET("\n");
    LOG_QUIET("---- ub_bench Report ----\n");
    if (cfg->bw_only == false) {
        double t_avg_us = hist_avg(merged_hist) / cpu_mhz;
        double t_min_us = (merged_hist->min_delta < (uint64_t)-1) ?
                          (double)merged_hist->min_delta / cpu_mhz : 0.0;
        uint64_t p50 = hist_percentile(merged_hist, 0.5);
        uint64_t p99 = hist_percentile(merged_hist, 0.99);
        uint64_t p999 = hist_percentile(merged_hist, 0.999);
        uint64_t p9999 = hist_percentile(merged_hist, 0.9999);
        uint64_t p99999 = hist_percentile(merged_hist, 0.99999);
        uint64_t pmax = merged_hist->max_delta;

        LOG_QUIET(" actual_QPS  target_QPS  BW_avg[%s/s]  MsgRate[Mpps]  "
                  "t_min[us]  t_median[us]  t_avg[us]  P99[us]  P99.9[us]  P99.99[us]  P99.999[us]  Pmax[us]\n",
                  unit_str);
        LOG_QUIET(" %-11lu %-11lu %-14.2f %-14.6f %-10.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f %-7.2f\n",
                  (uint64_t)actual_qps, target_qps, bw_avg, msg_rate,
                  t_min_us, (double)p50 / cpu_mhz, t_avg_us,
                  (double)p99 / cpu_mhz, (double)p999 / cpu_mhz,
                  (double)p9999 / cpu_mhz, (double)p99999 / cpu_mhz,
                  (double)pmax / cpu_mhz);
    } else {
        LOG_QUIET(" actual_QPS  target_QPS  BW_avg[%s/s]  MsgRate[Mpps]\n", unit_str);
        LOG_QUIET(" %-11lu %-11lu %-13.2f %-13.6f\n",
                  (uint64_t)actual_qps, target_qps, bw_avg, msg_rate);
    }
}

static int run_sequential_client(perftest_context_t *ctx, perftest_config_t *cfg)
{
    uint32_t i;
    int ret;

    /* Exchange sweep_count (0 for non-sweep) to match server's sync_data call */
    uint32_t sweep_count = 0;
    for (i = 0; i < cfg->pair_num; i++) {
        if (sync_data(cfg, i, sizeof(uint32_t), (char *)&sweep_count,
                      (char *)&sweep_count) != 0) {
            return -1;
        }
    }

    ret = prepare_jfs_wr(ctx, cfg);
    if (ret != 0) {
        return -1;
    }

    if (cfg->warm_up && perform_warm_up(ctx, cfg) != 0) {
        return -1;
    }

    for (i = 0; i < cfg->pair_num; i++) {
        if (sync_time(cfg, i, g_seq_sync_before[cfg->api_type]) != 0) {
            return -1;
        }
    }

    if (cfg->time_type.bs.duration == 1) {
        update_duration_state(ctx, cfg);
    }

    bench_thread_arg_t *args = calloc(cfg->threads, sizeof(bench_thread_arg_t));
    if (args == NULL) {
        return -1;
    }

    double cpu_mhz = get_cpu_mhz(false);
    for (i = 0; i < cfg->threads; i++) {
        args[i].cfg = cfg;
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].jetty_start = i;
        args[i].jetty_count = 1;
        args[i].cpu_mhz = cpu_mhz;
        hist_init(&args[i].hist);
    }

    if (cfg->time_type.bs.iterations == 1) {
        ctx->run_ctx.tposted[0] = get_cycles();
    }

    for (i = 0; i < cfg->threads; i++) {
        if (pthread_create(&args[i].tid, NULL, sequential_worker, &args[i]) != 0) {
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(args[j].tid, NULL);
            }
            free(args);
            return -1;
        }
    }

    void *thread_ret;
    int worker_err = 0;
    for (i = 0; i < cfg->threads; i++) {
        pthread_join(args[i].tid, &thread_ret);
        if (thread_ret != NULL) {
            worker_err = -1;
        }
    }

    if (cfg->time_type.bs.iterations == 1) {
        ctx->run_ctx.tcompleted[0] = get_cycles();
    }

    if (worker_err != 0) {
        free(args);
        return -1;
    }

    latency_hist_t global_hist;
    hist_init(&global_hist);
    uint64_t total_count = 0;
    uint64_t global_start = args[0].start_time;
    uint64_t global_end = args[0].end_time;

    for (i = 0; i < cfg->threads; i++) {
        hist_merge(&global_hist, &args[i].hist);
        total_count += args[i].hist.total_count;
        if (args[i].start_time < global_start) {
            global_start = args[i].start_time;
        }
        if (args[i].end_time > global_end) {
            global_end = args[i].end_time;
        }
    }

    if (cfg->time_type.bs.duration == 1) {
        global_start = ctx->run_ctx.tposted[0];
        global_end = ctx->run_ctx.tcompleted[0];
    }

    print_sequential_report(cfg, total_count, global_start, global_end, &global_hist);

    for (i = 0; i < cfg->pair_num; i++) {
        sync_time(cfg, i, g_seq_sync_after[cfg->api_type]);
    }

    bw_report_data_t local = {0};
    bw_report_data_t remote = {0};
    local.size = cfg->size;
    local.iters = total_count;
    for (i = 0; i < cfg->pair_num; i++) {
        sync_data(cfg, i, sizeof(bw_report_data_t), (char *)&local, (char *)&remote);
    }

    free(args);
    return 0;
}

static int run_sweep_client(perftest_context_t *ctx, perftest_config_t *cfg)
{
    uint32_t i;
    int ret;

    ret = prepare_jfs_wr(ctx, cfg);
    if (ret != 0) {
        return -1;
    }

    if (cfg->warm_up && perform_warm_up(ctx, cfg) != 0) {
        return -1;
    }

    uint32_t sweep_count = cfg->sweep_steps;
    for (i = 0; i < cfg->pair_num; i++) {
        if (sync_data(cfg, i, sizeof(uint32_t), (char *)&sweep_count,
                      (char *)&sweep_count) != 0) {
            return -1;
        }
    }

    sweep_result_t *results = calloc(cfg->sweep_steps, sizeof(sweep_result_t));
    if (results == NULL) {
        return -1;
    }

    double cpu_mhz = get_cpu_mhz(false);
    double unit_ratio = (cfg->bw_unit == PERFTEST_MB) ? (double)PERFTEST_BW_MB :
                        (cfg->bw_unit == PERFTEST_KB) ? (double)PERFTEST_BW_KB :
                        (double)PERFTEST_BW_GB;
    const char *unit_str = (cfg->bw_unit == PERFTEST_MB) ? "MiB" :
                           (cfg->bw_unit == PERFTEST_KB) ? "KiB" : "GiB";

    for (uint32_t s = 0; s < cfg->sweep_steps; s++) {
        uint64_t target_qps = cfg->sweep_start + (uint64_t)s * cfg->sweep_step;
        cfg->qps = target_qps;

        for (i = 0; i < cfg->pair_num; i++) {
            sync_time(cfg, i, g_seq_sync_before[cfg->api_type]);
        }

        bench_thread_arg_t *args = calloc(cfg->threads, sizeof(bench_thread_arg_t));
        if (args == NULL) {
            free(results);
            return -1;
        }

        uint64_t total_cycles = (uint64_t)(cpu_mhz * PERFTEST_M * cfg->sweep_duration);

        for (i = 0; i < cfg->threads; i++) {
            args[i].cfg = cfg;
            args[i].ctx = ctx;
            args[i].thread_id = i;
            args[i].jetty_start = i;
            args[i].jetty_count = 1;
            args[i].time_limit_cycles = total_cycles;
            args[i].cpu_mhz = cpu_mhz;
            hist_init(&args[i].hist);
        }

        for (i = 0; i < cfg->threads; i++) {
            if (pthread_create(&args[i].tid, NULL, sequential_worker, &args[i]) != 0) {
                LOG_ERROR("Failed to create thread %u in sweep step %u.\n", i, s);
                for (uint32_t j = 0; j < i; j++) {
                    pthread_join(args[j].tid, NULL);
                }
                free(args);
                free(results);
                return -1;
            }
        }

        void *thread_ret;
        int worker_err = 0;
        for (i = 0; i < cfg->threads; i++) {
            pthread_join(args[i].tid, &thread_ret);
            if (thread_ret != NULL) {
                worker_err = -1;
            }
        }

        if (worker_err != 0) {
            LOG_ERROR("Worker thread failed in sweep step %u.\n", s);
            free(args);
            free(results);
            return -1;
        }

        latency_hist_t merged;
        hist_init(&merged);
        uint64_t total_count = 0;
        uint64_t global_start = (uint64_t)(-1);
        uint64_t global_end = 0;

        for (i = 0; i < cfg->threads; i++) {
            hist_merge(&merged, &args[i].hist);
            total_count += args[i].hist.total_count;
            if (args[i].start_time > 0 && args[i].start_time < global_start) {
                global_start = args[i].start_time;
            }
            if (args[i].end_time > global_end) {
                global_end = args[i].end_time;
            }
        }

        double bw_avg = ((double)cfg->size * total_count * cpu_mhz * PERFTEST_M) /
                        ((double)(global_end - global_start) * unit_ratio);
        double t_avg = hist_avg(&merged) / cpu_mhz;
        double actual_qps = (double)total_count * cpu_mhz * PERFTEST_M /
                            (double)(global_end - global_start);
        double msg_rate = actual_qps / (double)PERFTEST_M;

        results[s].actual_total_qps = (uint64_t)actual_qps;
        results[s].target_total_qps = (uint64_t)cfg->threads * target_qps;
        results[s].per_thread_qps = target_qps;
        results[s].bw_avg = bw_avg;
        results[s].msg_rate = msg_rate;
        results[s].t_min = (merged.min_delta < (uint64_t)-1) ?
                            (double)merged.min_delta / cpu_mhz : 0.0;
        results[s].t_median = (double)hist_percentile(&merged, 0.5) / cpu_mhz;
        results[s].t_avg = t_avg;
        results[s].p99 = hist_percentile(&merged, 0.99);
        results[s].p999 = hist_percentile(&merged, 0.999);
        results[s].p9999 = hist_percentile(&merged, 0.9999);
        results[s].p99999 = hist_percentile(&merged, 0.99999);
        results[s].pmax = merged.max_delta;
        results[s].total_count = total_count;

        for (i = 0; i < cfg->pair_num; i++) {
            sync_time(cfg, i, g_seq_sync_after[cfg->api_type]);
            bw_report_data_t local = {0};
            bw_report_data_t remote = {0};
            local.size = cfg->size;
            local.iters = total_count;
            sync_data(cfg, i, sizeof(bw_report_data_t), (char *)&local, (char *)&remote);
        }

        free(args);
    }

    LOG_QUIET("\n");
    LOG_QUIET("==== ub_bench Sweep Report ====\n");
    LOG_QUIET(" threads: %u    size: %u    sweep_duration: %us    mode: sequential\n",
              cfg->threads, cfg->size, cfg->sweep_duration);
    LOG_QUIET("\n");
    LOG_QUIET(" actual_QPS  target_QPS  per_thr_QPS  BW_avg[%s/s]  MsgRate[Mpps]  "
              "t_min[us]  t_median[us]  t_avg[us]  P99[us]  P99.9[us]  P99.99[us]  P99.999[us]  Pmax[us]\n",
              unit_str);
    for (uint32_t s = 0; s < cfg->sweep_steps; s++) {
        LOG_QUIET(" %-11lu %-11lu %-12lu %-14.2f %-14.6f %-10.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f %-7.2f\n",
                  results[s].actual_total_qps,
                  results[s].target_total_qps,
                  results[s].per_thread_qps,
                  results[s].bw_avg, results[s].msg_rate,
                  results[s].t_min, results[s].t_median, results[s].t_avg,
                  (double)results[s].p99 / cpu_mhz,
                  (double)results[s].p999 / cpu_mhz,
                  (double)results[s].p9999 / cpu_mhz,
                  (double)results[s].p99999 / cpu_mhz,
                  (double)results[s].pmax / cpu_mhz);
    }

    free(results);
    return 0;
}

int bench_run_sequential(perftest_context_t *ctx, perftest_config_t *cfg)
{
    uint32_t i;

    if (cfg->server_ip == NULL && !cfg->bidirection) {
        print_bw_header(cfg);
        uint32_t sweep_count = 0;
        for (i = 0; i < cfg->pair_num; i++) {
            if (sync_data(cfg, i, sizeof(uint32_t), (char *)&sweep_count,
                          (char *)&sweep_count) != 0) {
                return -1;
            }
        }
        cfg->sweep_steps = sweep_count;
        cfg->sweep = (sweep_count > 0);

        if (cfg->cmd == PERFTEST_SEND_BW) {
            if (prepare_jfr_wr(ctx, cfg) != 0) {
                LOG_ERROR("Failed to prepare jfr wr for send_bw server.\n");
                return -1;
            }
        }

        uint32_t loop_count = cfg->sweep ? cfg->sweep_steps : 1;
        for (uint32_t s = 0; s < loop_count; s++) {
            for (i = 0; i < cfg->pair_num; i++) {
                if (sync_time(cfg, i, g_seq_sync_before[cfg->api_type]) != 0) {
                    return -1;
                }
                if (cfg->cmd == PERFTEST_SEND_BW) {
                    if (run_once_bw_recv(ctx, cfg) != 0) {
                        LOG_ERROR("Failed to run recv in send_bw server.\n");
                        return -1;
                    }
                }
                if (sync_time(cfg, i, g_seq_sync_after[cfg->api_type]) != 0) {
                    return -1;
                }
                bw_report_data_t local = {0};
                bw_report_data_t remote = {0};
                local.size = cfg->size;
                if (sync_data(cfg, i, sizeof(bw_report_data_t), (char *)&local,
                              (char *)&remote) != 0) {
                    return -1;
                }
                print_bi_bw_report(&local, &remote, cfg);
            }
        }
        return 0;
    }

    if (cfg->sweep) {
        return run_sweep_client(ctx, cfg);
    }
    return run_sequential_client(ctx, cfg);
}
