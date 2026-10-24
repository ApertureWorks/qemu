/*
 * Virtio-GPU ring-fence gate, QEMU side. See virtio-gpu-fence-gate.h.
 *
 * Everything here runs on the main loop thread with the BQL held (fence commands are processed there, the gpu.sock read
 * handler and the QEMU_CLOCK_REALTIME timer run there too), so the gate needs no lock of its own.
 *
 * Copyright (c) 2026 Aperture. SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "virtio-gpu-fence-gate.h"
#include "virtio-gpu-fence-gate-core.h"

static FenceGate s_gate;
static bool s_inited;
static QEMUTimer *s_timer;
static uint64_t s_last_log_timeouts;

static void gate_retire(void *opaque, void *c)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd = c;

    virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
    QTAILQ_REMOVE(&g->fenceq, cmd, next);
    g_free(cmd);
    g->inflight--;
}

static void gate_rearm(int64_t next)
{
    if (s_timer && next >= 0) {
        timer_mod(s_timer, next);
    }
}

static void gate_timer_cb(void *opaque)
{
    int64_t next = fence_gate_expire(&s_gate, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));

    if (s_gate.stats.timed_out != s_last_log_timeouts) {
        /* rate limited by construction: the timer fires at most once per timeout period */
        fprintf(stderr, "[QEMU-FENCE-GATE] %" PRIu64 " held fences timed out (total), held=%" PRIu64 " released=%" PRIu64 "\n",
                s_gate.stats.timed_out, s_gate.stats.held, s_gate.stats.released);
        s_last_log_timeouts = s_gate.stats.timed_out;
    }
    gate_rearm(next);
}

void virtio_gpu_fence_gate_attach(VirtIOGPU *g)
{
    if (!s_inited) {
        fence_gate_init(&s_gate, gate_retire, g);
        s_timer = timer_new_ms(QEMU_CLOCK_REALTIME, gate_timer_cb, NULL);
        s_inited = true;
    }
    s_gate.opaque = g;
}

bool virtio_gpu_fence_gate_hold(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    if (!s_inited || !s_gate.enabled) {
        return false;
    }
    assert(bql_locked());
    s_gate.opaque = g;
    if (!fence_gate_hold(&s_gate, cmd->cmd_hdr.ring_idx, cmd, qemu_clock_get_ms(QEMU_CLOCK_REALTIME))) {
        return false;
    }
    gate_rearm(fence_gate_expire(&s_gate, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)));
    return true;
}

void virtio_gpu_fence_gate_device_reset(void)
{
    if (s_inited) {
        fence_gate_drop_all(&s_gate);
    }
}

void virtio_gpu_fence_gate_host_gone(void)
{
    if (!s_inited || !s_gate.enabled) {
        return;
    }
    for (uint32_t i = 0; i < s_gate.ring_count; i++) {
        fence_gate_reset(&s_gate, s_gate.base_ring + i, 0);
    }
    s_gate.enabled = false;
    if (s_timer) {
        timer_del(s_timer);
    }
    fprintf(stderr, "[QEMU-FENCE-GATE] host gone: gate released and disabled\n");
}

int virtio_gpu_fence_gate_payload_len(uint8_t tag)
{
    switch (tag) {
    case APERTURE_GATE_PKT_RELEASE_UP_TO: return 8; /* [ring][reserved][epoch u16][seq u32] */
    case APERTURE_GATE_PKT_CONFIG:        return 4; /* [base][count][timeout_ms u16] */
    case APERTURE_GATE_PKT_RESET:         return 3; /* [ring][epoch u16] */
    default:                              return -1;
    }
}

size_t virtio_gpu_fence_gate_handle(uint8_t tag, const uint8_t *p, uint8_t reply[4])
{
    if (!s_inited) {
        return 0; /* no virgl device: nothing to gate; the packet is consumed and ignored */
    }
    assert(bql_locked());
    switch (tag) {
    case APERTURE_GATE_PKT_CONFIG:
        fence_gate_configure(&s_gate, p[0], p[1], (uint16_t)((p[2] << 8) | p[3]));
        fprintf(stderr, "[QEMU-FENCE-GATE] %s base=%u count=%u timeout=%" PRId64 "ms\n",
                s_gate.enabled ? "enabled" : "disabled", s_gate.base_ring, s_gate.ring_count, s_gate.timeout_ms);
        return 0;
    case APERTURE_GATE_PKT_RELEASE_UP_TO: {
        uint16_t epoch = (uint16_t)((p[2] << 8) | p[3]);
        uint32_t seq = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];

        fence_gate_release_up_to(&s_gate, p[0], epoch, seq);
        return 0;
    }
    case APERTURE_GATE_PKT_RESET: {
        uint16_t epoch = (uint16_t)((p[1] << 8) | p[2]);

        fence_gate_reset(&s_gate, p[0], epoch);
        reply[0] = APERTURE_GATE_PKT_RESET_ACK;
        reply[1] = p[0];
        reply[2] = p[1];
        reply[3] = p[2];
        return 4;
    }
    default:
        return 0;
    }
}
