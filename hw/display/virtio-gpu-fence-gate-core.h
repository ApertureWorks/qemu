/*
 * Virtio-GPU ring-fence gate: pure bookkeeping, no QEMU types (unit tested on its own).
 *
 * A fenced SUBMIT_3D on a gated ring is not passed to virglrenderer; the command stays on the fence queue and is retired
 * later by an explicit cumulative credit from the host (ReleaseUpTo), by a reset, or by a timeout. The timeout is the
 * safety net that makes a dead or hung host unable to stall the guest. The gate is disabled until configured, and a
 * disabled gate claims no ring, so the default path is unchanged.
 *
 * Copyright (c) 2026 Aperture. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VIRTIO_GPU_FENCE_GATE_CORE_H
#define VIRTIO_GPU_FENCE_GATE_CORE_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#define FENCE_GATE_MAX_RINGS 32

typedef struct FenceGateRing {
    uint16_t epoch;
    uint32_t counter;  /* ordinal of the newest held-or-retired fence since the last reset */
    uint32_t credit;   /* cumulative credit received; may run ahead of the counter */
    GQueue held;       /* of FenceGateHeld*, in ordinal order */
} FenceGateRing;

typedef struct FenceGateStats {
    uint64_t held, released, timed_out, reset, dropped;
} FenceGateStats;

typedef void (*FenceGateRetireFn)(void *opaque, void *cmd);

typedef struct FenceGate {
    bool enabled;
    uint8_t base_ring, ring_count;
    int64_t timeout_ms;
    FenceGateRing rings[FENCE_GATE_MAX_RINGS];
    FenceGateRetireFn retire;
    void *opaque;
    FenceGateStats stats;
} FenceGate;

void fence_gate_init(FenceGate *g, FenceGateRetireFn retire, void *opaque);
/* Enables the gate for rings [base, base+count) (count is clamped to FENCE_GATE_MAX_RINGS); a zero count disables it. */
void fence_gate_configure(FenceGate *g, uint8_t base, uint8_t count, uint16_t timeout_ms);
bool fence_gate_claims(const FenceGate *g, uint32_t ring);
/*
 * Takes over a fenced command of a claimed ring. If the credit already covers its ordinal it is retired at once (the host's
 * message and the guest's fence travel different paths and either can win). Returns false if the ring is not claimed.
 */
bool fence_gate_hold(FenceGate *g, uint32_t ring, void *cmd, int64_t now_ms);
/* Cumulative credit; ignored when `epoch` is not the ring's current epoch. Returns the number of commands retired. */
unsigned fence_gate_release_up_to(FenceGate *g, uint32_t ring, uint16_t epoch, uint32_t seq);
/* Retires everything held on the ring, zeroes counter and credit, adopts `epoch`. Returns the number retired. */
unsigned fence_gate_reset(FenceGate *g, uint32_t ring, uint16_t epoch);
/* Retires held commands older than the timeout. Returns the absolute deadline (ms) of the oldest remaining one, or -1. */
int64_t fence_gate_expire(FenceGate *g, int64_t now_ms);
/*
 * Fail open: retires everything held and credits every future fence at once (until the next reset). Used when the host
 * is gone, so guest fences on gated rings keep signalling instead of hanging forever.
 */
void fence_gate_open_all(FenceGate *g);
/* Forgets every held command without retiring it (the owner is freeing them, e.g. device reset). */
void fence_gate_drop_all(FenceGate *g);
unsigned fence_gate_held_count(const FenceGate *g, uint32_t ring);

#endif
