/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: log2 histogram implementation for ub_bench
 * Create: 2026-09-16
 */

#include "ub_bench_hist.h"

uint64_t hist_percentile(const latency_hist_t *h, double pct)
{
    if (h->total_count == 0) {
        return 0;
    }
    uint64_t threshold = (uint64_t)(h->total_count * (1.0 - pct));
    uint64_t count = 0;
    for (int i = HIST_BUCKET_NUM - 1; i >= 0; i--) {
        count += h->buckets[i];
        if (count > threshold) {
            return hist_bucket_to_value(i);
        }
    }
    return h->max_delta;
}

void hist_merge(latency_hist_t *dst, const latency_hist_t *src)
{
    for (int i = 0; i < HIST_BUCKET_NUM; i++) {
        dst->buckets[i] += src->buckets[i];
    }
    dst->total_count += src->total_count;
    dst->sum_delta += src->sum_delta;
    if (src->min_delta < dst->min_delta) {
        dst->min_delta = src->min_delta;
    }
    if (src->max_delta > dst->max_delta) {
        dst->max_delta = src->max_delta;
    }
}

double hist_avg(const latency_hist_t *h)
{
    if (h->total_count == 0) {
        return 0;
    }
    return (double)h->sum_delta / (double)h->total_count;
}
