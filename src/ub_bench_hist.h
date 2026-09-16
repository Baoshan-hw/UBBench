/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: log2 histogram for ub_bench latency collection
 * Create: 2026-09-16
 */

#ifndef UB_BENCH_HIST_H
#define UB_BENCH_HIST_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define HIST_SUB_BITS   9
#define HIST_MAX_MAG    28   // max 2^28 cycles (~111ms @2.4GHz), covers 8MB packets + tail spikes
#define HIST_SUB_NUM    (1 << HIST_SUB_BITS)
#define HIST_BUCKET_NUM (HIST_MAX_MAG * HIST_SUB_NUM)

typedef struct {
    uint32_t buckets[HIST_BUCKET_NUM];
    uint64_t total_count;
    uint64_t sum_delta;
    uint64_t min_delta;
    uint64_t max_delta;
} latency_hist_t;

static inline void hist_init(latency_hist_t *h)
{
    memset(h, 0, sizeof(*h));
    h->min_delta = (uint64_t)-1;
    h->max_delta = 0;
}

static inline void hist_reset(latency_hist_t *h)
{
    memset(h->buckets, 0, sizeof(h->buckets));
    h->total_count = 0;
    h->sum_delta = 0;
    h->min_delta = (uint64_t)-1;
    h->max_delta = 0;
}

static inline void hist_add(latency_hist_t *h, uint64_t delta)
{
    if (delta == 0) {
        h->buckets[0]++;
        h->total_count++;
        h->sum_delta += delta;
        h->min_delta = 0;
        return;
    }
    int mag = delta ? (63 - __builtin_clzll(delta)) : 0;
    if (mag >= HIST_MAX_MAG) {
        mag = HIST_MAX_MAG - 1;
    }
    uint32_t sub_size = (1U << mag) >> HIST_SUB_BITS;
    if (sub_size == 0) {
        sub_size = 1;
    }
    int sub = (int)((delta - (1ULL << mag)) / sub_size);
    if (sub >= HIST_SUB_NUM) {
        sub = HIST_SUB_NUM - 1;
    }
    h->buckets[mag * HIST_SUB_NUM + sub]++;
    h->total_count++;
    h->sum_delta += delta;
    if (delta < h->min_delta) {
        h->min_delta = delta;
    }
    if (delta > h->max_delta) {
        h->max_delta = delta;
    }
}

static inline uint64_t hist_bucket_to_value(int bucket_idx)
{
    int mag = bucket_idx >> HIST_SUB_BITS;
    int sub = bucket_idx & (HIST_SUB_NUM - 1);
    uint32_t sub_size = (1U << mag) >> HIST_SUB_BITS;
    if (sub_size == 0) {
        sub_size = 1;
    }
    return (1ULL << mag) + (uint64_t)sub * sub_size;
}

uint64_t hist_percentile(const latency_hist_t *h, double pct);
void hist_merge(latency_hist_t *dst, const latency_hist_t *src);
double hist_avg(const latency_hist_t *h);

#endif
