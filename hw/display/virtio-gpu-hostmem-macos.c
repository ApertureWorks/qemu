/*
 * VirtIO GPU macOS Host Memory Backend (IOSurface Allocator)
 * Production Hardened Implementation
 *
 * Copyright (C) 2026 Aperture Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/virtio/virtio-gpu-hostmem.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOReturn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>

/* Maximum allowed single blob allocation size (1TB sanity limit) */
#define MAX_BLOB_SIZE (1ULL << 40)

/*
 * gpu_sock_notify_created / gpu_sock_notify_destroyed
 *
 * Notify Aperture's GPUControlServer of blob resource lifecycle events.
 * Wire format (big-endian, Option B — IDs only):
 *
 *   GPUResourceCreated  (9 bytes): [0x01][4B resource_id][4B iosurface_id]
 *   GPUResourceDestroyed (5 bytes): [0x02][4B resource_id]
 *
 * Each call opens a new AF_UNIX connection, sends the packet with MSG_DONTWAIT
 * (so the QEMU main loop is NEVER blocked by a slow host receiver), and closes.
 * connect() failing is intentionally silent — GPUControlServer may not be
 * started for H.264 sessions.
 */
static const char *aperture_gpu_sock_path(void)
{
    return getenv("APERTURE_GPU_SOCK");
}

static void gpu_sock_send(const uint8_t *buf, size_t len)
{
    const char *sock_path = aperture_gpu_sock_path();
    if (!sock_path || *sock_path == '\0') {
        return;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* GPUControlServer not yet listening or not a zero-copy session. */
        close(fd);
        return;
    }

    /* Best-effort non-blocking send. If the host is slow, we drop this event
     * rather than stalling the QEMU main loop. The next frame will re-sync. */
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        sent += (size_t)n;
    }

    close(fd);
}

static void gpu_sock_notify_created(uint32_t resource_id, uint32_t iosurface_id)
{
    uint8_t buf[9];
    buf[0] = 0x01;
    buf[1] = (resource_id  >> 24) & 0xFF;
    buf[2] = (resource_id  >> 16) & 0xFF;
    buf[3] = (resource_id  >>  8) & 0xFF;
    buf[4] = (resource_id       ) & 0xFF;
    buf[5] = (iosurface_id >> 24) & 0xFF;
    buf[6] = (iosurface_id >> 16) & 0xFF;
    buf[7] = (iosurface_id >>  8) & 0xFF;
    buf[8] = (iosurface_id      ) & 0xFF;
    gpu_sock_send(buf, sizeof(buf));
}

static void gpu_sock_notify_destroyed(uint32_t resource_id)
{
    uint8_t buf[5];
    buf[0] = 0x02;
    buf[1] = (resource_id >> 24) & 0xFF;
    buf[2] = (resource_id >> 16) & 0xFF;
    buf[3] = (resource_id >>  8) & 0xFF;
    buf[4] = (resource_id      ) & 0xFF;
    gpu_sock_send(buf, sizeof(buf));
}

struct VirtIOGPUMacOSHostMem {
    IOSurfaceRef surface;
    uint32_t iosurface_id;
};

void *virtio_gpu_hostmem_get_iosurface(struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return NULL;
    }
    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
    return (void *)hm->surface;
}

uint32_t virtio_gpu_hostmem_get_iosurface_id(struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return 0;
    }
    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
    return hm->iosurface_id;
}

static bool macos_hostmem_available(void)
{
    return true;
}

static int macos_hostmem_map_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return -EINVAL;
    }

    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
    if (!hm->surface) {
        return -EINVAL;
    }

    /* Guard against redundant locking */
    if (res->remapped) {
        return 0;
    }

    kern_return_t kr = IOSurfaceLock(hm->surface, 0, NULL);
    if (kr != kIOReturnSuccess) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: IOSurfaceLock failed for res_id=%u (kr=0x%x)\n",
                      __func__, res->resource_id, kr);
        return -ENOMEM;
    }

    void *addr = IOSurfaceGetBaseAddress(hm->surface);
    if (!addr) {
        IOSurfaceUnlock(hm->surface, 0, NULL);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: IOSurfaceGetBaseAddress returned NULL for res_id=%u\n",
                      __func__, res->resource_id);
        return -ENOMEM;
    }

    res->remapped = (uint8_t *)addr;
    res->blob = res->remapped;
    return 0;
}

static void macos_hostmem_unmap_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return;
    }

    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
    if (res->remapped && hm->surface) {
        kern_return_t kr = IOSurfaceUnlock(hm->surface, 0, NULL);
        if (kr != kIOReturnSuccess) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: IOSurfaceUnlock failed for res_id=%u (kr=0x%x)\n",
                          __func__, res->resource_id, kr);
        }
        res->remapped = NULL;
        res->blob = NULL;
    }
}

static int macos_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (!res) {
        return -EINVAL;
    }

    size_t page_size = getpagesize();

    /* Bounds, sanity, and overflow check for allocation size */
    if (res->blob_size == 0 || res->blob_size > MAX_BLOB_SIZE || res->blob_size > (uint64_t)SIZE_MAX - page_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Invalid blob allocation size %" PRIu64 " for res_id=%u\n",
                      __func__, res->blob_size, res->resource_id);
        return -EINVAL;
    }

    /* Double allocation check on existing resource handle */
    if (res->hostmem_priv) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Resource %u already has active host memory allocation\n",
                      __func__, res->resource_id);
        return -EEXIST;
    }

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    if (!props) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Failed to create CFDictionary props\n", __func__);
        return -ENOMEM;
    }

    size_t size = res->blob_size;
    CFNumberRef bytes_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &size);
    if (!bytes_num) {
        CFRelease(props);
        return -ENOMEM;
    }

    CFDictionarySetValue(props, kIOSurfaceAllocSize, bytes_num);
    CFRelease(bytes_num);

    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);

    if (!surface) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: IOSurfaceCreate failed for size %" PRIu64 " (out of memory)\n",
                      __func__, res->blob_size);
        return -ENOMEM;
    }

    struct VirtIOGPUMacOSHostMem *hm = g_new0(struct VirtIOGPUMacOSHostMem, 1);
    hm->surface = surface;
    hm->iosurface_id = IOSurfaceGetID(surface);
    res->hostmem_priv = hm;

    qemu_log_mask(LOG_UNIMP, "[QEMU VirtIO GPU Trace] RESOURCE_CREATE_BLOB: res_id=%u, size=%" PRIu64 ", iosurface_id=%u\n",
                  res->resource_id, res->blob_size, hm->iosurface_id);

    /* Notify Aperture host that this IOSurface is now associated with resource_id. */
    gpu_sock_notify_created(res->resource_id, hm->iosurface_id);

    /* Lock CPU mapping for host RAM access */
    int ret = macos_hostmem_map_resource(g, res);
    if (ret < 0) {
        CFRelease(surface);
        g_free(hm);
        res->hostmem_priv = NULL;
        return ret;
    }

    return 0;
}

static void macos_hostmem_destroy_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return;
    }

    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;

    /* Notify Aperture before releasing the surface so host can stop sampling it. */
    gpu_sock_notify_destroyed(res->resource_id);

    res->hostmem_priv = NULL;

    if (hm->surface) {
        if (res->remapped) {
            kern_return_t kr = IOSurfaceUnlock(hm->surface, 0, NULL);
            if (kr != kIOReturnSuccess) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: IOSurfaceUnlock failed for res_id=%u (kr=0x%x)\n",
                              __func__, res->resource_id, kr);
            }
            res->remapped = NULL;
            res->blob = NULL;
        }
        CFRelease(hm->surface);
        hm->surface = NULL;
    }

    g_free(hm);
}

const VirtIOGPUHostMemOps virtio_gpu_hostmem_macos_ops = {
    .available = macos_hostmem_available,
    .create_resource = macos_hostmem_create_resource,
    .destroy_resource = macos_hostmem_destroy_resource,
    .map_resource = macos_hostmem_map_resource,
    .unmap_resource = macos_hostmem_unmap_resource,
};
