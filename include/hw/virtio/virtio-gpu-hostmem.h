/*
 * VirtIO GPU Host Memory Abstraction Layer
 *
 * Copyright (C) 2026 Aperture Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef VIRTIO_GPU_HOSTMEM_H
#define VIRTIO_GPU_HOSTMEM_H

#include "hw/virtio/virtio-gpu.h"

struct VirtIOGPUHostMemOps {
    bool (*available)(void);
    int (*create_resource)(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
    void (*destroy_resource)(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
    int (*map_resource)(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
    void (*unmap_resource)(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
};

typedef struct VirtIOGPUHostMemOps VirtIOGPUHostMemOps;

/* Public Wrapper APIs */
bool virtio_gpu_hostmem_available(void);
int virtio_gpu_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
void virtio_gpu_hostmem_destroy_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
int virtio_gpu_hostmem_map_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
void virtio_gpu_hostmem_unmap_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);

void *virtio_gpu_hostmem_get_iosurface(struct virtio_gpu_simple_resource *res);
uint32_t virtio_gpu_hostmem_get_iosurface_id(struct virtio_gpu_simple_resource *res);

/* Backend Vtables */

extern const VirtIOGPUHostMemOps virtio_gpu_hostmem_linux_ops;
extern const VirtIOGPUHostMemOps virtio_gpu_hostmem_macos_ops;

#endif /* VIRTIO_GPU_HOSTMEM_H */
