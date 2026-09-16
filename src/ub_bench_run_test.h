/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: run test header file for ub_bench
 * Create: 2026-09-16
 * Note:
 * History: 2026-09-16   create file
 */

#ifndef UB_BENCH_RUN_TEST_H
#define UB_BENCH_RUN_TEST_H

#include <signal.h>

#include "ub_bench_parameters.h"
#include "ub_bench_resources.h"

extern volatile sig_atomic_t g_exit_flag;

int run_read_lat(perftest_context_t *ctx, perftest_config_t *cfg);
int run_write_lat(perftest_context_t *ctx, perftest_config_t *cfg);
int run_send_lat(perftest_context_t *ctx, perftest_config_t *cfg);
int run_atomic_lat(perftest_context_t *ctx, perftest_config_t *cfg);
int run_read_bw(perftest_context_t *ctx, perftest_config_t *cfg);
int run_write_bw(perftest_context_t *ctx, perftest_config_t *cfg);
int run_send_bw(perftest_context_t *ctx, perftest_config_t *cfg);
int run_atomic_bw(perftest_context_t *ctx, perftest_config_t *cfg);
int prepare_jfs_wr(perftest_context_t *ctx, perftest_config_t *cfg);
void update_duration_state(perftest_context_t *ctx, perftest_config_t *cfg);
void print_bi_bw_report(const bw_report_data_t *local_bw_report,
                        const bw_report_data_t *remote_bw_report, const perftest_config_t *cfg);
void print_bw_header(const perftest_config_t *cfg);
int run_once_bw_recv(perftest_context_t *ctx, perftest_config_t *cfg);
int prepare_jfr_wr(perftest_context_t *ctx, perftest_config_t *cfg);
uint64_t get_remote_seg_va(const perftest_context_t *ctx, const perftest_config_t *cfg, uint32_t i);

#endif
