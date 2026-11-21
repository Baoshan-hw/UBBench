/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: sequential mode and sweep for ub_bench
 * Create: 2026-09-16
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include "ub_bench_run.h"
#include "ub_bench_run_test.h"
#include "ub_bench_mgmt.h"
#include "urma_api.h"

#define SEQ_PEER_EXIT_CMD ('Q')

static const char *g_seq_sync_before[] = {"seq_before_read_bw", "seq_before_write_bw",
                                          "seq_before_send_bw", "seq_before_atomic_bw"};
static const char *g_seq_sync_after[] = {"seq_after_read_bw", "seq_after_write_bw",
                                         "seq_after_send_bw", "seq_after_atomic_bw"};

static void seq_catch_sigint(int sig)
{
    (void)sig;
    g_exit_flag = 1;
}

/* Interval distribution = cumulative histogram difference (bucket-wise).
 * All members are monotonic counters, so subtraction is well defined.
 * min/max are running extremes and NOT differentiable; callers must not
 * use them from the diff result. */
static void hist_subtract(latency_hist_t *dst, const latency_hist_t *a, const latency_hist_t *b)
{
    for (int i = 0; i < HIST_BUCKET_NUM; i++) {
        dst->buckets[i] = (a->buckets[i] > b->buckets[i]) ? (a->buckets[i] - b->buckets[i]) : 0;
    }
    dst->total_count = a->total_count - b->total_count;
    dst->sum_delta = a->sum_delta - b->sum_delta;
    dst->min_delta = 0;
    dst->max_delta = 0;
}

static void print_sequential_header(const perftest_config_t *cfg);

typedef struct {
    perftest_context_t *ctx;
    perftest_config_t *cfg;
    bench_thread_arg_t *args;
    uint32_t threads;
    uint32_t period_ms;
    double cpu_mhz;
    uint32_t avg_size;
    volatile int *stop;
} seq_inf_print_ctx_t;

/*
 * Infinite (-y) print thread for sequential mode. Mirrors the pipeline
 * infinite_print_thread control flow (comm_poll slices for peer-exit 'Q'
 * detection + periodic reporting). Each periodic row uses the exact same
 * header and column layout as the final sequential report, with interval
 * (per-period) statistics computed from worker histogram snapshots.
 */
static void *seq_infinite_print_thread(void *arg)
{
    seq_inf_print_ctx_t *pc = (seq_inf_print_ctx_t *)arg;
    perftest_config_t *cfg = pc->cfg;
    int poll_slice_ms = ((int)pc->period_ms < 100) ? (int)pc->period_ms : 100;
    uint32_t elapsed_ms = 0;
    uint64_t last_cycles = get_cycles();
    double cycles_to_sec = pc->cpu_mhz * PERFTEST_M;
    const perftest_bw_unit_desc_t *unit = get_bw_unit_desc(cfg->bw_unit);
    double unit_ratio = (double)unit->bytes_per_unit;

    latency_hist_t *prev = calloc(1, sizeof(latency_hist_t));
    latency_hist_t *now = calloc(1, sizeof(latency_hist_t));
    latency_hist_t *diff = calloc(1, sizeof(latency_hist_t));
    bool *responded = calloc(pc->threads, sizeof(bool));
    /* prev starts as an all-zero baseline snapshot taken at thread start
     * (the print thread is created before the workers), so the FIRST
     * periodic line covers [thread start, first snapshot] with a correctly
     * scoped interval denominator. */
    bool have_prev = true;

    print_sequential_header(cfg);

    while (!*pc->stop && !g_exit_flag) {
        int pr = comm_poll(cfg, 0, poll_slice_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr > 0) {
            char buf[1];
            ssize_t n = comm_recv(cfg, 0, buf, sizeof(buf));
            if (n <= 0 || buf[0] == SEQ_PEER_EXIT_CMD) {
                g_exit_flag = 1;
                break;
            }
            /* Keep normal synchronization data for the main thread. */
            (void)usleep((uint32_t)poll_slice_ms * 1000);
        }
        elapsed_ms += (uint32_t)poll_slice_ms;
        if (elapsed_ms < pc->period_ms) {
            continue;
        }
        elapsed_ms = 0;

        /* Request a consistent snapshot from every worker. */
        for (uint32_t i = 0; i < pc->threads; i++) {
            __atomic_store_n(&pc->args[i].snap_request, 1, __ATOMIC_RELEASE);
        }
        /* Workers respond within one packet gap; a thread isolated by
         * error_suspend never responds and its histogram is frozen. Only
         * responsive threads contribute to this interval's exact min/max. */
        for (uint32_t i = 0; i < pc->threads; i++) {
            responded[i] = false;
        }
        for (uint32_t w = 0; w < 100; w++) {
            bool all = true;
            for (uint32_t i = 0; i < pc->threads; i++) {
                if (__atomic_load_n(&pc->args[i].snap_ready, __ATOMIC_ACQUIRE)) {
                    responded[i] = true;
                } else {
                    all = false;
                }
            }
            if (all || *pc->stop || g_exit_flag) {
                break;
            }
            (void)usleep(1000);
        }
        hist_init(now);
        uint64_t interval_min = (uint64_t)-1;
        uint64_t interval_max = 0;
        for (uint32_t i = 0; i < pc->threads; i++) {
            if (responded[i]) {
                if (pc->args[i].snap.min_delta < interval_min) {
                    interval_min = pc->args[i].snap.min_delta;
                }
                if (pc->args[i].snap.max_delta > interval_max) {
                    interval_max = pc->args[i].snap.max_delta;
                }
            }
            hist_merge(now, &pc->args[i].snap);
            __atomic_store_n(&pc->args[i].snap_ready, 0, __ATOMIC_RELAXED);
        }

        uint64_t now_cycles = get_cycles();
        if (have_prev && now->total_count >= prev->total_count) {
            hist_subtract(diff, now, prev);
            double interval_s = (double)(now_cycles - last_cycles) / cycles_to_sec;
            if (interval_s > 0.0 && diff->total_count > 0) {
                double bw_avg = ((double)pc->avg_size * (double)diff->total_count) /
                                (interval_s * unit_ratio);
                double actual_qps = (double)diff->total_count / interval_s;
                double msg_rate = actual_qps / (double)PERFTEST_M;
                double t_min_us = (interval_min != (uint64_t)-1) ?
                                  (double)interval_min / pc->cpu_mhz : 0.0;
                double t_max_us = (double)interval_max / pc->cpu_mhz;
                double t_median = (double)hist_percentile(diff, 0.5) / pc->cpu_mhz;
                double t_avg = hist_avg(diff) / pc->cpu_mhz;
                double p99 = (double)hist_percentile(diff, 0.99) / pc->cpu_mhz;
                double p999 = (double)hist_percentile(diff, 0.999) / pc->cpu_mhz;
                double p9999 = (double)hist_percentile(diff, 0.9999) / pc->cpu_mhz;
                double p99999 = (double)hist_percentile(diff, 0.99999) / pc->cpu_mhz;
                if (cfg->bw_only == false) {
                    LOG_QUIET(" %-11u %-11lu %-13lu %-14.2f %-14.6f %-10.2f %-9.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f\n",
                              pc->avg_size, diff->total_count, (uint64_t)actual_qps, bw_avg, msg_rate,
                              t_min_us, t_max_us, t_median, t_avg,
                              p99, p999, p9999, p99999);
                } else {
                    LOG_QUIET(" %-11u %-11lu %-13lu %-13.2f %-13.6f\n",
                              pc->avg_size, diff->total_count, (uint64_t)actual_qps, bw_avg, msg_rate);
                }
            }
        }
        /* Track the prev snapshot's timestamp unconditionally so the next
         * interval denominator is scoped to [prev snapshot, now] even when
         * this round printed nothing (first round / hist reset guard). */
        last_cycles = now_cycles;
        *prev = *now;
        have_prev = true;
    }

    free(prev);
    free(now);
    free(diff);
    free(responded);
    return NULL;
}

typedef struct {
    perftest_config_t *cfg;
    volatile int *stop;
    /* periodic reporting (send_bw server): interval deltas of the shared
     * recv counter; avg_size and cpu_mhz for BW conversion */
    uint64_t *recv_count;
    uint32_t period_ms;
    double cpu_mhz;
    uint32_t avg_size;
} seq_inf_ctrl_ctx_t;

/* Infinite (-y) server control thread: detect peer exit ('Q') and local
 * SIGINT, propagate via g_exit_flag so recv workers stop; for send_bw also
 * print periodic 5-column receive statistics (server measures no latency,
 * so the row carries counters only — same columns as its final row). */
static void *seq_infinite_server_ctrl(void *arg)
{
    seq_inf_ctrl_ctx_t *cc = (seq_inf_ctrl_ctx_t *)arg;
    uint32_t period_ms = cc->period_ms;
    int poll_slice_ms = ((int)period_ms < 100) ? (int)period_ms : 100;
    uint32_t elapsed_ms = 0;
    uint64_t last_count = 0;
    uint64_t last_cycles = get_cycles();
    double cycles_to_sec = cc->cpu_mhz * PERFTEST_M;
    const perftest_bw_unit_desc_t *unit = get_bw_unit_desc(cc->cfg->bw_unit);
    double unit_ratio = (double)unit->bytes_per_unit;

    if (cc->recv_count != NULL) {
        /* Same header/columns as the bw_only sequential report row: the
         * server measures no latency, so its row is counters only. */
        LOG_QUIET(" bytes      iterations  QPS_actual   BW_avg[%s/s] MsgRate[Mpps]\n", unit->name);
    }

    while (!*cc->stop && !g_exit_flag) {
        int pr = comm_poll(cc->cfg, 0, poll_slice_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr > 0) {
            char buf[1];
            ssize_t n = comm_recv(cc->cfg, 0, buf, sizeof(buf));
            if (n <= 0 || buf[0] == SEQ_PEER_EXIT_CMD) {
                g_exit_flag = 1;
                break;
            }
        }
        if (cc->recv_count == NULL) {
            continue;
        }
        elapsed_ms += (uint32_t)poll_slice_ms;
        if (elapsed_ms < period_ms) {
            continue;
        }
        elapsed_ms = 0;
        uint64_t now_count = __atomic_load_n(cc->recv_count, __ATOMIC_RELAXED);
        uint64_t now_cycles = get_cycles();
        double interval_s = (double)(now_cycles - last_cycles) / cycles_to_sec;
        uint64_t packets = now_count - last_count;
        if (interval_s > 0.0 && packets > 0) {
            double bw_avg = ((double)cc->avg_size * (double)packets) / (interval_s * unit_ratio);
            double actual_qps = (double)packets / interval_s;
            double msg_rate = actual_qps / (double)PERFTEST_M;
            LOG_QUIET(" %-11u %-11lu %-13lu %-13.2f %-13.6f\n",
                      cc->avg_size, packets, (uint64_t)actual_qps, bw_avg, msg_rate);
        }
        last_count = now_count;
        last_cycles = now_cycles;
    }
    return NULL;
}

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
    /* -y: no iteration bound, collect per-packet latency for the whole run.
     * With -y -D the duration state machine below re-drives collecting. */
    bool collecting = (cfg->time_type.bs.iterations == 1 || cfg->time_type.bs.infinite == 1);
    uint64_t sweep_start = 0;
    bool sweep_active = (t->time_limit_cycles > 0);

    if (sweep_active) {
        sweep_start = get_cycles();
        collecting = true;
    }
    t->start_time = get_cycles();

    while (1) {
        if (g_exit_flag) {
            break;
        }
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
            if (cfg->time_type.bs.infinite == 1) {
                /* -y -D: collect throughout WARMUP/START/STOP like upstream's
                 * cumulative iters counter; the state machine only controls
                 * exit timing, not collection. Periodic lines carry data from
                 * the very start (matching upstream, hence the "first and
                 * last results may be invalid" startup warning). */
                collecting = true;
            } else if (run_ctx->state == START_STATE) {
                if (!collecting) {
                    collecting = true;
                    hist_reset(&t->hist);
                    t->interval_min = (uint64_t)-1;
                    t->interval_max = 0;
                    t->start_time = get_cycles();
                }
            } else {
                collecting = false;
            }
        }

        if (interval > 0 && next_deadline > 0) {
            while (get_cycles() < next_deadline) {
                if (g_exit_flag) {
                    break;
                }
            }
            if (g_exit_flag) {
                break;
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
        } while (cqe_cnt == 0 && !run_ctx->abort && !g_exit_flag &&
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
            if (cfg->error_suspend == false) {
                if (__sync_bool_compare_and_swap(&run_ctx->abort, 0, 1)) {
                    LOG_ERROR("CR error: %s (status %d), thread %u.\n",
                              cr_status_str((int)cr.status), (int)cr.status, id);
                    if (cr.status == URMA_CR_ACK_TIMEOUT_ERR) {
                        LOG_ERROR("Hint: try reducing --threads or -s.\n");
                    } else if (cr.status == URMA_CR_RNR_RETRY_CNT_EXC_ERR) {
                        LOG_ERROR("Hint: receiver buffer exhausted, try increasing -R or reducing --threads.\n");
                    }
                }
                t->end_time = get_cycles();
                return (void *)(intptr_t)(-1);
            }
            /* error_suspend: the JFS is suspended by HW; this thread cannot
             * make progress, but the other threads own independent JFS/JFC
             * and keep running. Isolate this thread instead of aborting. */
            LOG_ERROR("CR error: %s (status %d), thread %u isolated (error_suspend).\n",
                      cr_status_str((int)cr.status), (int)cr.status, id);
            t->err_count++;
            t->end_time = get_cycles();
            return NULL;
        }

        uint64_t tcompleted = get_cycles();
        uint64_t delta = tcompleted - tposted;
        /* BW mode: delta = post + one-way data transfer + small ACK return + poll
           This is NOT a full data round-trip (only LAT ping-pong is).
           Do NOT divide by 2 in BW mode. */

        if (collecting) {
            hist_add(&t->hist, delta);
            if (delta < t->interval_min) {
                t->interval_min = delta;
            }
            if (delta > t->interval_max) {
                t->interval_max = delta;
            }
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

        /* infinite: serve the print thread's snapshot request in the gap
         * between packets; this thread is the only writer of t->hist.
         * The staging carries cumulative buckets plus the EXACT interval
         * extremes (snap.min/max); the interval trackers then restart. */
        if (__atomic_load_n(&t->snap_request, __ATOMIC_ACQUIRE)) {
            t->snap = t->hist;
            t->snap.min_delta = t->interval_min;
            t->snap.max_delta = t->interval_max;
            t->interval_min = (uint64_t)-1;
            t->interval_max = 0;
            __atomic_store_n(&t->snap_ready, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&t->snap_request, 0, __ATOMIC_RELAXED);
        }

        if (cfg->time_type.bs.iterations == 1 && scnt >= cfg->iters) {
            break;
        }
    }

    t->end_time = get_cycles();
    return NULL;
}

static void *recv_server_worker(void *arg)
{
    bench_thread_arg_t *t = (bench_thread_arg_t *)arg;
    perftest_context_t *ctx = t->ctx;
    perftest_config_t *cfg = t->cfg;
    run_test_ctx_t *run_ctx = &ctx->run_ctx;
    uint32_t id = t->jetty_start;

    urma_cr_t *cr = calloc(1, sizeof(urma_cr_t) * PERFTEST_POLL_BATCH);
    if (cr == NULL) {
        t->end_time = get_cycles();
        return (void *)(intptr_t)(-1);
    }

    urma_status_t status;
    uint64_t rcnt = 0;
    uint64_t collected = 0;
    urma_jfr_wr_t *wr = &run_ctx->jfr_wr[id * cfg->jfr_post_list];

    t->start_time = get_cycles();

    time_t last_recv_time = time(NULL);
    uint64_t idle_count = 0;

    while (1) {
        if (run_ctx->abort) {
            break;
        }
        if (g_exit_flag) {
            break;
        }
        if (cfg->time_type.bs.duration == 1) {
            if (run_ctx->state == END_STATE) {
                break;
            }
        }

        int cqe_cnt = urma_poll_jfc(ctx->jfc_r[id], PERFTEST_POLL_BATCH, cr);
        if (cqe_cnt < 0) {
            LOG_ERROR("Failed to poll jfc_r, thread %u.\n", id);
            free(cr);
            t->end_time = get_cycles();
            return (void *)(intptr_t)(-1);
        }

        if (cqe_cnt > 0) {
            last_recv_time = time(NULL);
            idle_count = 0;
        } else if (++idle_count % 1000000 == 0) {
            if (time(NULL) - last_recv_time > 10) {
                if (__sync_bool_compare_and_swap(&run_ctx->abort, 0, 1)) {
                    LOG_ERROR("Timeout: no data for 10s, peer may have disconnected.\n");
                }
                break;
            }
        }

        for (int i = 0; i < cqe_cnt; i++) {
            if (cr[i].status != URMA_CR_SUCCESS) {
                if (cfg->error_suspend == false) {
                    if (__sync_bool_compare_and_swap(&run_ctx->abort, 0, 1)) {
                        LOG_ERROR("CR error: %s (status %d), thread %u.\n",
                                  cr_status_str((int)cr[i].status), (int)cr[i].status, id);
                        if (cr[i].status == URMA_CR_ACK_TIMEOUT_ERR) {
                            LOG_ERROR("Hint: try reducing --threads or -s.\n");
                        } else if (cr[i].status == URMA_CR_RNR_RETRY_CNT_EXC_ERR) {
                            LOG_ERROR("Hint: receiver buffer exhausted, try increasing -R or reducing --threads.\n");
                        }
                    }
                    free(cr);
                    t->end_time = get_cycles();
                    return (void *)(intptr_t)(-1);
                }
                /* error_suspend: isolate this recv thread, others keep polling. */
                LOG_ERROR("CR error: %s (status %d), recv thread %u isolated (error_suspend).\n",
                          cr_status_str((int)cr[i].status), (int)cr[i].status, id);
                t->err_count++;
                free(cr);
                t->end_time = get_cycles();
                return NULL;
            }
            rcnt++;
            if (cfg->time_type.bs.infinite == 1) {
                __atomic_add_fetch(&run_ctx->inf_recv_count, 1, __ATOMIC_RELAXED);
            }

            if (cfg->time_type.bs.duration == 1) {
                if (run_ctx->state == START_STATE) {
                    collected++;
                }
            } else {
                collected++;
            }

            urma_jfr_wr_t *bad_wr = NULL;
            if (cfg->jetty_mode == PERFTEST_JETTY_SIMPLEX) {
                status = urma_post_jfr_wr(ctx->jfr[id], wr, &bad_wr);
            } else {
                status = urma_post_jetty_recv_wr(ctx->jetty[id], wr, &bad_wr);
            }
            if (status != URMA_SUCCESS) {
                LOG_ERROR("Failed to post recv WR, thread %u, rcnt %lu.\n", id, rcnt);
                free(cr);
                t->end_time = get_cycles();
                return (void *)(intptr_t)(-1);
            }

            if (cfg->size <= (ctx->page_size / PERFTEST_BUF_NUM)) {
                uint32_t local_sge_idx = id * PERFTEST_SGE_NUM_PRE_WR * cfg->sge_num + cfg->sge_num;
                urma_sge_t *local_sge = &run_ctx->jfr_sge[local_sge_idx];
                uint64_t lva = (cfg->seg_pre_jetty == false)
                                   ? (uint64_t)ctx->local_buf[0] + id * ctx->buf_size
                                   : (uint64_t)ctx->local_buf[id];
                increase_loc_addr(local_sge, cfg->size, (uint64_t)run_ctx->rposted + rcnt,
                                  lva, cfg->cache_line_size, ctx->page_size);
            }
        }

        if (cfg->time_type.bs.iterations == 1 && rcnt >= cfg->iters) {
            break;
        }
    }

    free(cr);
    t->end_time = get_cycles();
    t->total_count = collected;
    return NULL;
}

typedef struct {
    uint32_t size;
    uint64_t total_count;
    uint64_t global_start;
    uint64_t global_end;
    uint64_t target_qps;
    bool bw_only;
    double t_min;
    double t_median;
    double t_avg;
    double p99;
    double p999;
    double p9999;
    double p99999;
    double pmax;
} sequential_report_data_t;

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

/* Actual average message length: with random IO size the WR init drew each
 * length into ctx->sum_size (per WR template), so the report must convert
 * bandwidth with the drawn length, not the declared cfg->size. */
static uint32_t get_actual_avg_size(const perftest_context_t *ctx, const perftest_config_t *cfg)
{
    if (ctx->sum_size == 0) {
        return cfg->size;
    }
    return (uint32_t)(ctx->sum_size / (cfg->jfs_post_list * cfg->jettys));
}

static void print_sequential_header(const perftest_config_t *cfg)
{
    const char *unit_str = get_bw_unit_desc(cfg->bw_unit)->name;
    LOG_QUIET("\n");
    if (cfg->bw_only == false) {
        LOG_QUIET(" bytes      iterations  QPS_actual   BW_avg[%s/s]  MsgRate[Mpps]  "
                  "t_min[us]  t_max[us] t_median[us]  t_avg[us]  99%%[us]  99.9%%[us]  99.99%%[us]  99.999%%[us] \n",
                  unit_str);
    } else {
        LOG_QUIET(" bytes      iterations  QPS_actual   BW_avg[%s/s] MsgRate[Mpps]\n", unit_str);
    }
}

static void print_sequential_report(perftest_config_t *cfg, uint32_t avg_size, uint64_t total_count,
                                    uint64_t global_start, uint64_t global_end,
                                    const latency_hist_t *merged_hist, bool print_header)
{
    double cpu_mhz = get_cpu_mhz(false);
    if (cpu_mhz <= 0.0) {
        LOG_ERROR("Failed to get cpu mhz for report.\n");
        return;
    }

    double cycles_to_units = cpu_mhz * PERFTEST_M;
    const perftest_bw_unit_desc_t *unit_desc = get_bw_unit_desc(cfg->bw_unit);
    double unit_ratio = (double)unit_desc->bytes_per_unit;

    uint64_t cycles_sum = global_end - global_start;
    double bw_avg = ((double)avg_size * total_count * cycles_to_units) /
                    ((double)cycles_sum * unit_ratio);
    double actual_qps = (double)total_count * cycles_to_units / (double)cycles_sum;
    double msg_rate = actual_qps / (double)PERFTEST_M;

    if (print_header) {
        print_sequential_header(cfg);
    }
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

        LOG_QUIET(" %-11u %-11lu %-13lu %-14.2f %-14.6f %-10.2f %-9.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f\n",
                  avg_size, total_count, (uint64_t)actual_qps, bw_avg, msg_rate,
                  t_min_us, (double)pmax / cpu_mhz, (double)p50 / cpu_mhz, t_avg_us,
                  (double)p99 / cpu_mhz, (double)p999 / cpu_mhz,
                  (double)p9999 / cpu_mhz, (double)p99999 / cpu_mhz);
    } else {
        LOG_QUIET(" %-11u %-11lu %-13lu %-13.2f %-13.6f\n",
                  avg_size, total_count, (uint64_t)actual_qps, bw_avg, msg_rate);
    }
}

static void print_sequential_report_from_data(const sequential_report_data_t *data,
                                             const perftest_config_t *cfg, bool header)
{
    double cpu_mhz = get_cpu_mhz(false);
    if (cpu_mhz <= 0.0) {
        LOG_ERROR("Failed to get cpu mhz for report.\n");
        return;
    }

    double cycles_to_units = cpu_mhz * PERFTEST_M;
    const perftest_bw_unit_desc_t *unit_desc = get_bw_unit_desc(cfg->bw_unit);
    double unit_ratio = (double)unit_desc->bytes_per_unit;

    uint64_t cycles_sum = data->global_end - data->global_start;
    double bw_avg = ((double)data->size * data->total_count * cycles_to_units) /
                    ((double)cycles_sum * unit_ratio);
    double actual_qps = (double)data->total_count * cycles_to_units / (double)cycles_sum;
    double msg_rate = actual_qps / (double)PERFTEST_M;

    if (header) {
        print_sequential_header(cfg);
    }
    if (data->bw_only == false) {
        LOG_QUIET(" %-11u %-11lu %-13lu %-14.2f %-14.6f %-10.2f %-9.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f\n",
                  data->size, data->total_count, (uint64_t)actual_qps, bw_avg, msg_rate,
                  data->t_min, data->pmax, data->t_median, data->t_avg,
                  data->p99, data->p999, data->p9999, data->p99999);
    } else {
        LOG_QUIET(" %-11u %-11lu %-13lu %-13.2f %-13.6f\n",
                  data->size, data->total_count, (uint64_t)actual_qps, bw_avg, msg_rate);
    }
}

static int run_sequential_client(perftest_context_t *ctx, perftest_config_t *cfg)
{
    uint32_t i;
    int ret;

    ret = prepare_jfs_wr(ctx, cfg);
    if (ret != 0) {
        return -1;
    }

    if (cfg->warm_up && perform_warm_up(ctx, cfg) != 0) {
        destroy_jfs_wr(ctx);
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
        args[i].interval_min = (uint64_t)-1;
        args[i].interval_max = 0;
    }

    if (cfg->time_type.bs.iterations == 1) {
        ctx->run_ctx.tposted[0] = get_cycles();
    }

    /* infinite (-y): start the periodic print thread (peer-exit detection +
     * interval latency reporting) before the workers. */
    volatile int inf_stop = 0;
    seq_inf_print_ctx_t pc = {0};
    pthread_t print_tid;
    bool inf_print_on = false;
    if (cfg->time_type.bs.infinite == 1) {
        g_exit_flag = 0;
        (void)signal(SIGINT, seq_catch_sigint);
        pc.ctx = ctx;
        pc.cfg = cfg;
        pc.args = args;
        pc.threads = cfg->threads;
        pc.period_ms = (cfg->inf_period_ms > 0) ? cfg->inf_period_ms : (uint32_t)cfg->inf_period * 1000;
        pc.cpu_mhz = cpu_mhz;
        pc.avg_size = get_actual_avg_size(ctx, cfg);
        pc.stop = &inf_stop;
        if (pthread_create(&print_tid, NULL, seq_infinite_print_thread, &pc) != 0) {
            LOG_ERROR("Failed to create infinite print thread.\n");
            (void)signal(SIGINT, SIG_DFL);
            free(args);
            return -1;
        }
        inf_print_on = true;
    }

    for (i = 0; i < cfg->threads; i++) {
        if (pthread_create(&args[i].tid, NULL, sequential_worker, &args[i]) != 0) {
            if (inf_print_on) {
                inf_stop = 1;
                (void)pthread_join(print_tid, NULL);
                (void)signal(SIGINT, SIG_DFL);
            }
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(args[j].tid, NULL);
            }
            free(args);
            return -1;
        }
    }

    void *thread_ret;
    int worker_err = 0;
    uint32_t err_threads = 0;
    for (i = 0; i < cfg->threads; i++) {
        pthread_join(args[i].tid, &thread_ret);
        if (thread_ret != NULL) {
            worker_err = -1;
        }
        if (args[i].err_count > 0) {
            err_threads++;
        }
    }

    if (inf_print_on) {
        inf_stop = 1;
        void *print_ret;
        (void)pthread_join(print_tid, &print_ret);
        /* Ctrl-C exit: notify the peer before the tail; the tail syncs below
         * are skipped so no partial mgmt message is left in flight. */
        if (g_exit_flag) {
            notify_peer_exit();
        }
        (void)signal(SIGINT, SIG_DFL);
    }

    if (cfg->time_type.bs.iterations == 1) {
        ctx->run_ctx.tcompleted[0] = get_cycles();
    }

    if (worker_err != 0) {
        ctx->run_ctx.abort = 1;
        free(args);
        return -1;
    }
    if (err_threads > 0) {
        LOG_QUIET(" Error threads       : %u/%u isolated (error_suspend)\n", err_threads, cfg->threads);
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

    uint32_t avg_size = get_actual_avg_size(ctx, cfg);
    /* -y: no final report, matching the pipeline infinite convention -- the
     * last periodic line is the client's final output; the server still
     * prints its row from the report exchange below. */
    if (cfg->time_type.bs.infinite == 0) {
        print_sequential_report(cfg, avg_size, total_count, global_start, global_end, &global_hist, !cfg->all);
    }

    if (g_exit_flag == 0) {
        for (i = 0; i < cfg->pair_num; i++) {
            sync_time(cfg, i, g_seq_sync_after[cfg->api_type]);
        }

        {
            sequential_report_data_t sdata = {0};
            sdata.size = avg_size;
            sdata.total_count = total_count;
            sdata.global_start = global_start;
            sdata.global_end = global_end;
            sdata.target_qps = (cfg->qps > 0) ? (uint64_t)cfg->threads * cfg->qps : 0;
            sdata.bw_only = cfg->bw_only;
            if (cfg->bw_only == false && total_count > 0) {
                sdata.t_min = (global_hist.min_delta < (uint64_t)-1) ?
                              (double)global_hist.min_delta / cpu_mhz : 0.0;
                sdata.t_median = (double)hist_percentile(&global_hist, 0.5) / cpu_mhz;
                sdata.t_avg = hist_avg(&global_hist) / cpu_mhz;
                sdata.p99 = (double)hist_percentile(&global_hist, 0.99) / cpu_mhz;
                sdata.p999 = (double)hist_percentile(&global_hist, 0.999) / cpu_mhz;
                sdata.p9999 = (double)hist_percentile(&global_hist, 0.9999) / cpu_mhz;
                sdata.p99999 = (double)hist_percentile(&global_hist, 0.99999) / cpu_mhz;
                sdata.pmax = (double)global_hist.max_delta / cpu_mhz;
            }
            sequential_report_data_t remote_data = {0};
            for (i = 0; i < cfg->pair_num; i++) {
                sync_data(cfg, i, sizeof(sequential_report_data_t),
                          (char *)&sdata, (char *)&remote_data);
            }
        }
    }

    destroy_jfs_wr(ctx);
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
        destroy_jfs_wr(ctx);
        return -1;
    }

    sweep_result_t *results = calloc(cfg->sweep_steps, sizeof(sweep_result_t));
    if (results == NULL) {
        return -1;
    }

    /* The WR set is prepared once for all sweep steps, so the drawn lengths
     * (random IO size) are fixed for this whole sweep; report their mean. */
    uint32_t avg_size = get_actual_avg_size(ctx, cfg);

    double cpu_mhz = get_cpu_mhz(false);
    const perftest_bw_unit_desc_t *unit_desc = get_bw_unit_desc(cfg->bw_unit);
    double unit_ratio = (double)unit_desc->bytes_per_unit;
    const char *unit_str = unit_desc->name;

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
            ctx->run_ctx.abort = 1;
            free(args);
            free(results);
            return -1;
        }

        uint32_t err_threads = 0;
        for (i = 0; i < cfg->threads; i++) {
            if (args[i].err_count > 0) {
                err_threads++;
            }
        }
        if (err_threads > 0) {
            LOG_QUIET(" Error threads       : %u/%u isolated (error_suspend)\n", err_threads, cfg->threads);
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

        double bw_avg = ((double)avg_size * total_count * cpu_mhz * PERFTEST_M) /
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
            sequential_report_data_t sdata = {0};
            sdata.size = avg_size;
            sdata.total_count = total_count;
            sdata.global_start = global_start;
            sdata.global_end = global_end;
            sdata.target_qps = (uint64_t)cfg->threads * target_qps;
            sdata.bw_only = cfg->bw_only;
            if (cfg->bw_only == false && total_count > 0) {
                sdata.t_min = (merged.min_delta < (uint64_t)-1) ?
                              (double)merged.min_delta / cpu_mhz : 0.0;
                sdata.t_median = (double)hist_percentile(&merged, 0.5) / cpu_mhz;
                sdata.t_avg = hist_avg(&merged) / cpu_mhz;
                sdata.p99 = (double)hist_percentile(&merged, 0.99) / cpu_mhz;
                sdata.p999 = (double)hist_percentile(&merged, 0.999) / cpu_mhz;
                sdata.p9999 = (double)hist_percentile(&merged, 0.9999) / cpu_mhz;
                sdata.p99999 = (double)hist_percentile(&merged, 0.99999) / cpu_mhz;
                sdata.pmax = (double)merged.max_delta / cpu_mhz;
            }
            sequential_report_data_t remote_data = {0};
            sync_data(cfg, i, sizeof(sequential_report_data_t),
                      (char *)&sdata, (char *)&remote_data);
        }

        free(args);
    }

    LOG_QUIET("\n");
    LOG_QUIET(" bytes      iterations  QPS_actual   BW_avg[%s/s]  MsgRate[Mpps]  "
              "t_min[us]  t_max[us] t_median[us]  t_avg[us]  99%%[us]  99.9%%[us]  99.99%%[us]  99.999%%[us] \n",
              unit_str);
    for (uint32_t s = 0; s < cfg->sweep_steps; s++) {
        LOG_QUIET(" %-11u %-11lu %-13lu %-14.2f %-14.6f %-10.2f %-9.2f %-13.2f %-10.2f %-8.2f %-10.2f %-11.2f %-12.2f\n",
                  avg_size,
                  results[s].total_count,
                  results[s].actual_total_qps,
                  results[s].bw_avg, results[s].msg_rate,
                  results[s].t_min, (double)results[s].pmax / cpu_mhz,
                  results[s].t_median, results[s].t_avg,
                  (double)results[s].p99 / cpu_mhz,
                  (double)results[s].p999 / cpu_mhz,
                  (double)results[s].p9999 / cpu_mhz,
                  (double)results[s].p99999 / cpu_mhz);
    }

    destroy_jfs_wr(ctx);
    free(results);
    return 0;
}

static int run_recv_server_mt(perftest_context_t *ctx, perftest_config_t *cfg, bool print_header)
{
    uint32_t i;

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
    }

    /* infinite (-y): control thread detects peer exit ('Q') / local SIGINT and
     * stops the recv workers via g_exit_flag. */
    volatile int ctrl_stop = 0;
    seq_inf_ctrl_ctx_t cc = {0};
    pthread_t ctrl_tid;
    bool ctrl_on = false;
    if (cfg->time_type.bs.infinite == 1) {
        g_exit_flag = 0;
        ctx->run_ctx.inf_recv_count = 0;
        (void)signal(SIGINT, seq_catch_sigint);
        cc.cfg = cfg;
        cc.stop = &ctrl_stop;
        cc.recv_count = &ctx->run_ctx.inf_recv_count;
        cc.period_ms = (cfg->inf_period_ms > 0) ? cfg->inf_period_ms : (uint32_t)cfg->inf_period * 1000;
        cc.cpu_mhz = cpu_mhz;
        cc.avg_size = get_actual_avg_size(ctx, cfg);
        if (pthread_create(&ctrl_tid, NULL, seq_infinite_server_ctrl, &cc) != 0) {
            LOG_ERROR("Failed to create infinite control thread.\n");
            (void)signal(SIGINT, SIG_DFL);
            free(args);
            return -1;
        }
        ctrl_on = true;
    }

    for (i = 0; i < cfg->threads; i++) {
        if (pthread_create(&args[i].tid, NULL, recv_server_worker, &args[i]) != 0) {
            if (ctrl_on) {
                ctrl_stop = 1;
                (void)pthread_join(ctrl_tid, NULL);
                (void)signal(SIGINT, SIG_DFL);
            }
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

    if (ctrl_on) {
        ctrl_stop = 1;
        void *ctrl_ret;
        (void)pthread_join(ctrl_tid, &ctrl_ret);
        if (g_exit_flag) {
            notify_peer_exit();
        }
        (void)signal(SIGINT, SIG_DFL);
    }

    if (worker_err != 0) {
        ctx->run_ctx.abort = 1;
        free(args);
        return -1;
    }

    uint32_t err_threads = 0;
    for (i = 0; i < cfg->threads; i++) {
        if (args[i].err_count > 0) {
            err_threads++;
        }
    }
    if (err_threads > 0) {
        LOG_QUIET(" Error threads       : %u/%u isolated (error_suspend)\n", err_threads, cfg->threads);
    }

    uint64_t total_count = 0;
    uint64_t global_start = args[0].start_time;
    uint64_t global_end = args[0].end_time;

    for (i = 0; i < cfg->threads; i++) {
        total_count += args[i].total_count;
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

    if (cfg->time_type.bs.infinite == 1 && g_exit_flag) {
        /* Ctrl-C / peer-exit: the report exchange is unavailable (the client
         * is gone); print local receive stats from the worker counters. */
        double cycles_to_sec = cpu_mhz * PERFTEST_M;
        double elapsed_s = (double)(global_end - global_start) / cycles_to_sec;
        LOG_QUIET(" server received %lu packets in %.1f s (infinite exit)\n",
                  total_count, elapsed_s);
        free(args);
        return 0;
    }

    for (i = 0; i < cfg->pair_num; i++) {
        if (sync_time(cfg, i, g_seq_sync_after[cfg->api_type]) != 0) {
            free(args);
            return -1;
        }
    }

    for (i = 0; i < cfg->pair_num; i++) {
        sequential_report_data_t local = {0};
        sequential_report_data_t remote = {0};
        if (sync_data(cfg, i, sizeof(sequential_report_data_t),
                      (char *)&local, (char *)&remote) != 0) {
            free(args);
            return -1;
        }
        remote.total_count = total_count;
        remote.global_start = global_start;
        remote.global_end = global_end;
        remote.bw_only = true;
        /* -y: periodic rows only (client-side convention); no final row. */
        if (cfg->time_type.bs.infinite == 0) {
            print_sequential_report_from_data(&remote, cfg, print_header);
        }
    }

    free(args);
    return 0;
}

int bench_run_sequential(perftest_context_t *ctx, perftest_config_t *cfg)
{
    uint32_t i;

    if (cfg->server_ip == NULL && !cfg->bidirection) {
        /* Total rounds driven by the client: (sizes when --all) x (sweep steps).
         * 0 keeps the legacy meaning "non-sweep single size" -> 1 round. */
        uint32_t total_rounds = 0;
        for (i = 0; i < cfg->pair_num; i++) {
            if (sync_data(cfg, i, sizeof(uint32_t), (char *)&total_rounds,
                          (char *)&total_rounds) != 0) {
                return -1;
            }
        }
        if (total_rounds == 0) {
            total_rounds = 1;
        }
        cfg->sweep_steps = total_rounds;

        if (cfg->cmd == PERFTEST_SEND_BW) {
            uint32_t rounds = (cfg->all == true) ? cfg->order : 1;
            for (uint32_t r = 0; r < rounds; r++) {
                int ret = 0;
                if (r > 0 && recreate_jetty(ctx, cfg) != 0) {
                    LOG_ERROR("Failed to recreate jetty, round: %u.\n", r);
                    return -1;
                }
                if (prepare_jfr_wr(ctx, cfg) != 0) {
                    LOG_ERROR("Failed to prepare jfr wr for send_bw server.\n");
                    return -1;
                }
                /* infinite forces the worker-based recv path (run_once_bw_recv
                 * is count-bound and shared with pipeline). */
                if (cfg->threads > 1 || cfg->time_type.bs.infinite == 1) {
                    ret = run_recv_server_mt(ctx, cfg, (cfg->all == true) ? (r == 0) : true);
                } else {
                    for (i = 0; i < cfg->pair_num; i++) {
                        if (sync_time(cfg, i, g_seq_sync_before[cfg->api_type]) != 0) {
                            ret = -1;
                            break;
                        }
                        uint64_t server_rcnt = 0;
                        if (run_once_bw_recv(ctx, cfg, &server_rcnt) != 0) {
                            LOG_ERROR("Failed to run recv in send_bw server.\n");
                            ret = -1;
                            break;
                        }
                        if (sync_time(cfg, i, g_seq_sync_after[cfg->api_type]) != 0) {
                            ret = -1;
                            break;
                        }
                        sequential_report_data_t local = {0};
                        sequential_report_data_t remote = {0};
                        if (sync_data(cfg, i, sizeof(sequential_report_data_t),
                                      (char *)&local, (char *)&remote) != 0) {
                            ret = -1;
                            break;
                        }
                        if (ctx->run_ctx.tposted[0] > 0 && ctx->run_ctx.tcompleted[0] > 0) {
                            remote.global_start = ctx->run_ctx.tposted[0];
                            remote.global_end = ctx->run_ctx.tcompleted[0];
                        }
                        remote.total_count = server_rcnt;
                        remote.bw_only = true;
                        print_sequential_report_from_data(&remote, cfg,
                                                          (cfg->all == true) ? (r == 0) : true);
                    }
                }
                destroy_jfr_wr(ctx);
                if (ret != 0) {
                    return -1;
                }
            }
            return 0;
        }

        uint32_t loop_count = total_rounds;
        if (cfg->sweep) {
            const char *srv_unit_str = get_bw_unit_desc(cfg->bw_unit)->name;
            LOG_QUIET("\n");
            LOG_QUIET(" bytes      iterations  QPS_actual   BW_avg[%s/s]  MsgRate[Mpps]  "
                      "t_min[us]  t_max[us] t_median[us]  t_avg[us]  99%%[us]  99.9%%[us]  99.99%%[us]  99.999%%[us] \n",
                      srv_unit_str);
        }
        for (uint32_t s = 0; s < loop_count; s++) {
            for (i = 0; i < cfg->pair_num; i++) {
                if (sync_time(cfg, i, g_seq_sync_before[cfg->api_type]) != 0) {
                    return -1;
                }
                if (sync_time(cfg, i, g_seq_sync_after[cfg->api_type]) != 0) {
                    return -1;
                }
                sequential_report_data_t local = {0};
                sequential_report_data_t remote = {0};
                if (sync_data(cfg, i, sizeof(sequential_report_data_t),
                              (char *)&local, (char *)&remote) != 0) {
                    return -1;
                }
                /* -y: periodic rows only (client-side convention); the
                 * exchange still runs for protocol completion. */
                if (cfg->time_type.bs.infinite == 0) {
                    print_sequential_report_from_data(&remote, cfg, (s == 0 && !cfg->sweep));
                }
            }
        }
        return 0;
    }

    /* CLIENT: exchange total rounds with the server first, then drive rounds. */
    uint32_t total_rounds = (uint32_t)((cfg->all ? cfg->order : 1) *
                                       (cfg->sweep ? cfg->sweep_steps : 1));
    for (i = 0; i < cfg->pair_num; i++) {
        if (sync_data(cfg, i, sizeof(uint32_t), (char *)&total_rounds,
                      (char *)&total_rounds) != 0) {
            return -1;
        }
    }

    if (cfg->sweep) {
        uint32_t sizes = cfg->all ? cfg->order : 1;
        for (uint32_t s = 0; s < sizes; s++) {
            if (cfg->all) {
                cfg->size = 1U << (s + 1);
            }
            if (run_sweep_client(ctx, cfg) != 0) {
                return -1;
            }
        }
        return 0;
    }

    uint32_t sizes = cfg->all ? cfg->order : 1;
    if (cfg->all) {
        /* One header, one row per size -- same convention as the pipeline -a scan. */
        print_sequential_header(cfg);
    }
    for (uint32_t s = 0; s < sizes; s++) {
        if (cfg->all) {
            cfg->size = 1U << (s + 1);
            /* send_bw server recreates its jfr per round; the paired syncs
             * inside recreate_jetty require the client to call it too. */
            if (s > 0 && cfg->cmd == PERFTEST_SEND_BW && recreate_jetty(ctx, cfg) != 0) {
                LOG_ERROR("Failed to recreate jetty, size: %u.\n", cfg->size);
                return -1;
            }
        }
        if (run_sequential_client(ctx, cfg) != 0) {
            return -1;
        }
    }
    return 0;
}
