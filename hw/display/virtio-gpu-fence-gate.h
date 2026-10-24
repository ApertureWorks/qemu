/*
 * Virtio-GPU ring-fence gate, QEMU side: glue between the gate bookkeeping, the fence queue and gpu.sock.
 * Off until the host sends GateConfig (0x0E); with it off every function here is a no-op and the default fence path is unchanged.
 *
 * Copyright (c) 2026 Aperture. SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VIRTIO_GPU_FENCE_GATE_H
#define VIRTIO_GPU_FENCE_GATE_H

#include "hw/virtio/virtio-gpu.h"

#define APERTURE_GATE_PKT_RELEASE_UP_TO 0x0D
#define APERTURE_GATE_PKT_CONFIG        0x0E
#define APERTURE_GATE_PKT_RESET         0x0F
#define APERTURE_GATE_PKT_RESET_ACK     0x10

void virtio_gpu_fence_gate_attach(VirtIOGPU *g);
/* True if the command's ring is gated and the command is now owned by the gate (the caller must not pass it to virglrenderer). */
bool virtio_gpu_fence_gate_hold(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd);
/* Device reset: forget held commands (the caller frees the fence queue). */
void virtio_gpu_fence_gate_device_reset(void);
/* gpu.sock peer went away: retire everything held and disable the gate. */
void virtio_gpu_fence_gate_host_gone(void);
/* Payload length (bytes after the tag) of a gate packet from the host, or -1 if `tag` is not one. */
int virtio_gpu_fence_gate_payload_len(uint8_t tag);
/* Applies a host gate packet. Returns the number of bytes to send back in `reply` (0 or 4). Runs on the main loop. */
size_t virtio_gpu_fence_gate_handle(uint8_t tag, const uint8_t *payload, uint8_t reply[4]);

#endif
