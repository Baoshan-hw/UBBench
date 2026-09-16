/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: management header file for ub_bench
 * Create: 2026-09-16
 * Note:
 * History: 2026-09-16   create file
 */

#ifndef UB_BENCH_MGMT_H
#define UB_BENCH_MGMT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct perftest_config perftest_config_t;

typedef enum perftest_mgmt_type {
    PERFTEST_MGMT_TCP = 0,
    PERFTEST_MGMT_UB,
    PERFTEST_MGMT_TYPE_NUM
} perftest_mgmt_type_t;

int establish_connection(const perftest_config_t *cfg);
void close_connection(perftest_config_t *cfg);

int sync_data(const perftest_config_t *cfg, uint32_t index, int size, char *local_data, char *remote_data);
int sync_time(const perftest_config_t *cfg, uint32_t index, const char *a);
ssize_t comm_send(const perftest_config_t *cfg, uint32_t index, const void *buf, size_t size);
ssize_t comm_recv(const perftest_config_t *cfg, uint32_t index, void *buf, size_t size);
int comm_poll(const perftest_config_t *cfg, uint32_t index, int timeout_ms);

#endif
