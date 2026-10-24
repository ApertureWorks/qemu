/*
 * Virtio-GPU ring-fence gate bookkeeping. See virtio-gpu-fence-gate-core.h.
 *
 * Copyright (c) 2026 Aperture. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "virtio-gpu-fence-gate-core.h"

typedef struct FenceGateHeld {
    void *cmd;
    int64_t arrival_ms;
    uint32_t ordinal;
} FenceGateHeld;

void fence_gate_init(FenceGate *g, FenceGateRetireFn retire, void *opaque)
{
    memset(g, 0, sizeof(*g));
    g->retire = retire;
    g->opaque = opaque;
    for (int i = 0; i < FENCE_GATE_MAX_RINGS; i++) {
        g_queue_init(&g->rings[i].held);
    }
}

void fence_gate_configure(FenceGate *g, uint8_t base, uint8_t count, uint16_t timeout_ms)
{
    if (count > FENCE_GATE_MAX_RINGS) {
        count = FENCE_GATE_MAX_RINGS;
    }
    g->base_ring = base;
    g->ring_count = count;
    g->timeout_ms = timeout_ms ? timeout_ms : 100;
    g->enabled = count != 0;
}

static FenceGateRing *ring_of(FenceGate *g, uint32_t ring)
{
    if (!g->enabled || ring < g->base_ring || ring >= (uint32_t)g->base_ring + g->ring_count) {
        return NULL;
    }
    return &g->rings[ring - g->base_ring];
}

bool fence_gate_claims(const FenceGate *g, uint32_t ring)
{
    return ring_of((FenceGate *)g, ring) != NULL;
}

static void retire_one(FenceGate *g, FenceGateHeld *h)
{
    g->retire(g->opaque, h->cmd);
    g_free(h);
}

static unsigned retire_upto(FenceGate *g, FenceGateRing *r, uint32_t seq, uint64_t *counter)
{
    unsigned n = 0;
    FenceGateHeld *h;

    while ((h = g_queue_peek_head(&r->held)) && h->ordinal <= seq) {
        g_queue_pop_head(&r->held);
        retire_one(g, h);
        n++;
    }
    *counter += n;
    return n;
}

bool fence_gate_hold(FenceGate *g, uint32_t ring, void *cmd, int64_t now_ms)
{
    FenceGateRing *r = ring_of(g, ring);
    FenceGateHeld *h;

    if (!r) {
        return false;
    }
    h = g_new0(FenceGateHeld, 1);
    h->cmd = cmd;
    h->arrival_ms = now_ms;
    h->ordinal = ++r->counter;
    g->stats.held++;
    if (r->credit >= h->ordinal) {
        g->stats.released++;
        retire_one(g, h);
    } else {
        g_queue_push_tail(&r->held, h);
    }
    return true;
}

unsigned fence_gate_release_up_to(FenceGate *g, uint32_t ring, uint16_t epoch, uint32_t seq)
{
    FenceGateRing *r = ring_of(g, ring);

    if (!r || r->epoch != epoch) {
        return 0;
    }
    if (seq > r->credit) {
        r->credit = seq;
    }
    return retire_upto(g, r, r->credit, &g->stats.released);
}

unsigned fence_gate_reset(FenceGate *g, uint32_t ring, uint16_t epoch)
{
    FenceGateRing *r = ring_of(g, ring);
    unsigned n;

    if (!r) {
        return 0;
    }
    n = retire_upto(g, r, UINT32_MAX, &g->stats.reset);
    r->counter = 0;
    r->credit = 0;
    r->epoch = epoch;
    return n;
}

int64_t fence_gate_expire(FenceGate *g, int64_t now_ms)
{
    int64_t next = -1;

    if (!g->enabled) {
        return -1;
    }
    for (int i = 0; i < g->ring_count; i++) {
        FenceGateRing *r = &g->rings[i];
        FenceGateHeld *h;

        while ((h = g_queue_peek_head(&r->held)) && now_ms - h->arrival_ms >= g->timeout_ms) {
            g_queue_pop_head(&r->held);
            /* a timed-out fence is as good as credited: later ordinals still wait for their own credit or timeout */
            if (h->ordinal > r->credit) {
                r->credit = h->ordinal;
            }
            g->stats.timed_out++;
            retire_one(g, h);
        }
        if (h) {
            int64_t due = h->arrival_ms + g->timeout_ms;
            if (next < 0 || due < next) {
                next = due;
            }
        }
    }
    return next;
}

void fence_gate_drop_all(FenceGate *g)
{
    for (int i = 0; i < FENCE_GATE_MAX_RINGS; i++) {
        FenceGateHeld *h;

        while ((h = g_queue_pop_head(&g->rings[i].held))) {
            g->stats.dropped++;
            g_free(h);
        }
        g->rings[i].counter = 0;
        g->rings[i].credit = 0;
    }
}

unsigned fence_gate_held_count(const FenceGate *g, uint32_t ring)
{
    FenceGateRing *r = ring_of((FenceGate *)g, ring);

    return r ? g_queue_get_length(&r->held) : 0;
}
