/*
 * VirtIO GPU Linux Host Memory Backend (udmabuf)
 *
 * Copyright (C) 2026 Aperture Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-gpu-hostmem.h"

static bool linux_hostmem_available(void)
{
    return virtio_gpu_have_udmabuf();
}

static int linux_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    virtio_gpu_init_udmabuf(res);
    return res->blob ? 0 : -EINVAL;
}

static void linux_hostmem_destroy_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    virtio_gpu_fini_udmabuf(g, res);
}

static int linux_hostmem_map_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    return res->remapped ? 0 : -EINVAL;
}

static void linux_hostmem_unmap_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
}

const VirtIOGPUHostMemOps virtio_gpu_hostmem_linux_ops = {
    .available = linux_hostmem_available,
    .create_resource = linux_hostmem_create_resource,
    .destroy_resource = linux_hostmem_destroy_resource,
    .map_resource = linux_hostmem_map_resource,
    .unmap_resource = linux_hostmem_unmap_resource,
};
