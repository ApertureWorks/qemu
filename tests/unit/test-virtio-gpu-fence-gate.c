/*
 * Tests for the virtio-gpu ring-fence gate bookkeeping.
 *
 * Copyright (c) 2026 Aperture. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "../../hw/display/virtio-gpu-fence-gate-core.h"

#define RING 34

typedef struct Log {
    void *retired[64];
    int n;
} Log;

static void retire(void *opaque, void *cmd)
{
    Log *l = opaque;
    g_assert_cmpint(l->n, <, 64);
    l->retired[l->n++] = cmd;
}

static void *id(uintptr_t n)
{
    return (void *)n;
}

static void setup(FenceGate *g, Log *l)
{
    memset(l, 0, sizeof(*l));
    fence_gate_init(g, retire, l);
}

static void test_disabled_claims_nothing(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    g_assert_false(fence_gate_claims(&g, RING));
    g_assert_false(fence_gate_hold(&g, RING, id(1), 0));
    g_assert_cmpint(l.n, ==, 0);
    fence_gate_configure(&g, 32, 16, 100);
    g_assert_true(fence_gate_claims(&g, 32));
    g_assert_true(fence_gate_claims(&g, 47));
    g_assert_false(fence_gate_claims(&g, 31));
    g_assert_false(fence_gate_claims(&g, 48));
    fence_gate_configure(&g, 32, 0, 100);
    g_assert_false(fence_gate_claims(&g, 32));
}

static void test_cumulative_release(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 1);
    for (uintptr_t i = 1; i <= 4; i++) {
        g_assert_true(fence_gate_hold(&g, RING, id(i), 0));
    }
    g_assert_cmpint(l.n, ==, 0);
    g_assert_cmpuint(fence_gate_held_count(&g, RING), ==, 4);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 1, 2), ==, 2);
    g_assert_true(l.retired[0] == id(1) && l.retired[1] == id(2));
    g_assert_cmpuint(fence_gate_held_count(&g, RING), ==, 2);
    /* a smaller credit is a no-op */
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 1, 1), ==, 0);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 1, 9), ==, 2);
    g_assert_cmpint(l.n, ==, 4);
}

/* The host's credit can arrive before the guest's fence: it is kept and retires the fence on arrival. */
static void test_credit_before_fence(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 1);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 1, 3), ==, 0);
    g_assert_true(fence_gate_hold(&g, RING, id(1), 0));
    g_assert_true(fence_gate_hold(&g, RING, id(2), 0));
    g_assert_true(fence_gate_hold(&g, RING, id(3), 0));
    g_assert_cmpint(l.n, ==, 3);
    g_assert_true(fence_gate_hold(&g, RING, id(4), 0));
    g_assert_cmpint(l.n, ==, 3); /* 4th is beyond the credit */
    g_assert_cmpuint(fence_gate_held_count(&g, RING), ==, 1);
}

static void test_stale_epoch_is_ignored(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 5);
    fence_gate_hold(&g, RING, id(1), 0);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 4, 9), ==, 0);
    g_assert_cmpint(l.n, ==, 0);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 5, 1), ==, 1);
}

static void test_reset_retires_all_and_restarts_the_count(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 1);
    fence_gate_hold(&g, RING, id(1), 0);
    fence_gate_hold(&g, RING, id(2), 0);
    fence_gate_release_up_to(&g, RING, 1, 1);
    g_assert_cmpuint(fence_gate_reset(&g, RING, 2), ==, 1);
    g_assert_cmpint(l.n, ==, 2);
    /* ordinals restart: the first fence of epoch 2 is 1, and old credit does not retire it */
    g_assert_true(fence_gate_hold(&g, RING, id(3), 0));
    g_assert_cmpint(l.n, ==, 2);
    g_assert_cmpuint(fence_gate_release_up_to(&g, RING, 2, 1), ==, 1);
}

static void test_timeout_retires_and_reports_the_next_deadline(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 1);
    fence_gate_reset(&g, RING + 1, 1);
    fence_gate_hold(&g, RING, id(1), 1000);
    fence_gate_hold(&g, RING + 1, id(2), 1040);
    g_assert_cmpint(fence_gate_expire(&g, 1099), ==, 1100);
    g_assert_cmpint(l.n, ==, 0);
    g_assert_cmpint(fence_gate_expire(&g, 1100), ==, 1140);
    g_assert_cmpint(l.n, ==, 1);
    g_assert_true(l.retired[0] == id(1));
    g_assert_cmpint(fence_gate_expire(&g, 1140), ==, -1);
    g_assert_cmpint(l.n, ==, 2);
    g_assert_cmpuint(g.stats.timed_out, ==, 2);
}

/* No held fence outlives the timeout, whatever the host does (randomised). */
static void test_no_fence_outlives_the_timeout(void)
{
    FenceGate g;
    Log l;
    uint64_t x = 0x9E3779B97F4A7C15ull;
    int64_t now = 0;
    int64_t arrival[64] = { 0 };

    for (int round = 0; round < 200; round++) {
        uintptr_t next = 1;
        setup(&g, &l);
        fence_gate_configure(&g, 32, 16, 100);
        fence_gate_reset(&g, RING, 1);
        now = 0;
        for (int step = 0; step < 60; step++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            now += x % 40;
            if (x % 3 == 0 && next < 60) {
                arrival[next] = now;
                fence_gate_hold(&g, RING, id(next), now);
                next++;
            } else if (round % 2 && x % 7 == 0) { /* half of the rounds: a silent host */
                fence_gate_release_up_to(&g, RING, 1, (uint32_t)(x % 20));
            }
            fence_gate_expire(&g, now);
            for (uintptr_t i = 1; i < next; i++) {
                bool retired = false;
                for (int k = 0; k < l.n; k++) {
                    retired |= l.retired[k] == id(i);
                }
                if (!retired) {
                    g_assert_cmpint(now - arrival[i], <, 100);
                }
            }
        }
    }
}

static void test_drop_all_does_not_call_retire(void)
{
    FenceGate g;
    Log l;

    setup(&g, &l);
    fence_gate_configure(&g, 32, 16, 100);
    fence_gate_reset(&g, RING, 1);
    fence_gate_hold(&g, RING, id(1), 0);
    fence_gate_drop_all(&g);
    g_assert_cmpint(l.n, ==, 0);
    g_assert_cmpuint(fence_gate_held_count(&g, RING), ==, 0);
    g_assert_cmpuint(g.stats.dropped, ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/virtio-gpu/fence-gate/disabled", test_disabled_claims_nothing);
    g_test_add_func("/virtio-gpu/fence-gate/cumulative", test_cumulative_release);
    g_test_add_func("/virtio-gpu/fence-gate/credit-first", test_credit_before_fence);
    g_test_add_func("/virtio-gpu/fence-gate/stale-epoch", test_stale_epoch_is_ignored);
    g_test_add_func("/virtio-gpu/fence-gate/reset", test_reset_retires_all_and_restarts_the_count);
    g_test_add_func("/virtio-gpu/fence-gate/timeout", test_timeout_retires_and_reports_the_next_deadline);
    g_test_add_func("/virtio-gpu/fence-gate/bounded-hold", test_no_fence_outlives_the_timeout);
    g_test_add_func("/virtio-gpu/fence-gate/drop-all", test_drop_all_does_not_call_retire);
    return g_test_run();
}
