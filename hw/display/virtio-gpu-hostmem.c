/*
 * VirtIO GPU Host Memory Dispatcher
 *
 * Copyright (C) 2026 Aperture Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-gpu-hostmem.h"

static const VirtIOGPUHostMemOps *get_hostmem_ops(void)
{
#if defined(CONFIG_LINUX)
    return &virtio_gpu_hostmem_linux_ops;
#elif defined(CONFIG_DARWIN) || defined(__APPLE__)
    return &virtio_gpu_hostmem_macos_ops;
#else
    return &virtio_gpu_hostmem_macos_ops;
#endif
}

bool virtio_gpu_hostmem_available(void)
{
    const VirtIOGPUHostMemOps *ops = get_hostmem_ops();
    return ops && ops->available ? ops->available() : false;
}

int virtio_gpu_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    const VirtIOGPUHostMemOps *ops = get_hostmem_ops();
    return ops && ops->create_resource ? ops->create_resource(g, res) : -ENOSYS;
}

void virtio_gpu_hostmem_destroy_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    const VirtIOGPUHostMemOps *ops = get_hostmem_ops();
    if (ops && ops->destroy_resource) {
        ops->destroy_resource(g, res);
    }
}

int virtio_gpu_hostmem_map_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    const VirtIOGPUHostMemOps *ops = get_hostmem_ops();
    return ops && ops->map_resource ? ops->map_resource(g, res) : -ENOSYS;
}

void virtio_gpu_hostmem_unmap_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    const VirtIOGPUHostMemOps *ops = get_hostmem_ops();
    if (ops && ops->unmap_resource) {
        ops->unmap_resource(g, res);
    }
}

#if !defined(CONFIG_DARWIN) && !defined(__APPLE__)
void virtio_gpu_hostmem_notify_created(uint32_t resource_id, uint32_t iosurface_id) {}
void virtio_gpu_hostmem_notify_destroyed(uint32_t resource_id) {}

void *virtio_gpu_hostmem_get_iosurface(struct virtio_gpu_simple_resource *res)
{
    return NULL;
}

uint32_t virtio_gpu_hostmem_get_iosurface_id(struct virtio_gpu_simple_resource *res)
{
    return 0;
}

uint32_t virtio_gpu_hostmem_lookup_iosurface_id(uint32_t res_id)
{
    return 0;
}

void *virtio_gpu_hostmem_lookup_iosurface_base(uint32_t res_id)
{
    return NULL;
}
#endif

