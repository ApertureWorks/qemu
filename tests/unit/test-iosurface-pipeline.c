/*
 * Standalone End-to-End Pipeline Validation Test for VirtIO GPU IOSurface Host Memory
 *
 * Validates the full QEMU VirtIO GPU Resource Table & Operations Pipeline:
 * 1. Guest Command Simulation: VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB
 * 2. HostMemoryBackend -> macOS IOSurface allocation
 * 3. QEMU Resource Table Registration (reslist lookup by resource_id)
 * 4. Transfer to Host 2D / GPU operation into res->blob (IOSurface base address)
 * 5. Scanout / Display access via res->hostmem_priv & IOSurface ID
 * 6. VIRTIO_GPU_CMD_RESOURCE_UNREF -> Resource destruction & CFRelease
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <sys/queue.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#define QTAILQ_HEAD TAILQ_HEAD
#define QTAILQ_ENTRY TAILQ_ENTRY
#define QTAILQ_INIT TAILQ_INIT
#define QTAILQ_INSERT_HEAD TAILQ_INSERT_HEAD
#define QTAILQ_REMOVE TAILQ_REMOVE
#define QTAILQ_FOREACH TAILQ_FOREACH

struct virtio_gpu_simple_resource {
    uint32_t resource_id;
    uint64_t blob_size;
    void *blob;
    int dmabuf_fd;
    uint8_t *remapped;

    void *hostmem_priv;

    QTAILQ_ENTRY(virtio_gpu_simple_resource) next;
};

struct VirtIOGPUMacOSHostMem {
    IOSurfaceRef surface;
    uint32_t iosurface_id;
};

static void *virtio_gpu_hostmem_get_iosurface(struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) return NULL;
    return (void *)((struct VirtIOGPUMacOSHostMem *)res->hostmem_priv)->surface;
}

static uint32_t virtio_gpu_hostmem_get_iosurface_id(struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) return 0;
    return ((struct VirtIOGPUMacOSHostMem *)res->hostmem_priv)->iosurface_id;
}

/* Mock QEMU VirtIOGPU State containing Resource Table (reslist) */
typedef struct VirtIOGPU {
    QTAILQ_HEAD(, virtio_gpu_simple_resource) reslist;
} VirtIOGPU;

static struct virtio_gpu_simple_resource *
virtio_gpu_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->resource_id == resource_id) {
            return res;
        }
    }
    return NULL;
}

static int macos_hostmem_map_resource(struct virtio_gpu_simple_resource *res)
{
    if (!res || !res->hostmem_priv) {
        return -1;
    }

    struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
    IOSurfaceRef surface = hm->surface;
    IOSurfaceLock(surface, 0, NULL);
    res->remapped = (uint8_t *)IOSurfaceGetBaseAddress(surface);
    res->blob = res->remapped;

    return res->remapped ? 0 : -1;
}

static void macos_hostmem_unmap_resource(struct virtio_gpu_simple_resource *res)
{
    if (res && res->hostmem_priv) {
        struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
        IOSurfaceRef surface = hm->surface;
        IOSurfaceUnlock(surface, 0, NULL);
        res->remapped = NULL;
        res->blob = NULL;
    }
}

static int macos_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (!res || res->blob_size == 0) {
        return -1;
    }

    CFMutableDictionaryRef props = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    size_t size = res->blob_size;
    CFNumberRef bytes_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &size);
    CFDictionarySetValue(props, kIOSurfaceAllocSize, bytes_num);
    CFRelease(bytes_num);

    IOSurfaceRef surface = IOSurfaceCreate(props);
    CFRelease(props);

    if (!surface) {
        return -1;
    }

    struct VirtIOGPUMacOSHostMem *hm = calloc(1, sizeof(*hm));
    hm->surface = surface;
    hm->iosurface_id = IOSurfaceGetID(surface);
    res->hostmem_priv = hm;

    return macos_hostmem_map_resource(res);
}

static void macos_hostmem_destroy_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res)
{
    if (res && res->hostmem_priv) {
        struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
        res->hostmem_priv = NULL;
        if (res->remapped) {
            macos_hostmem_unmap_resource(res);
        }
        CFRelease(hm->surface);
        free(hm);
    }
}

/* Simulated QEMU VirtIO GPU Command: VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB */
static int virtio_gpu_resource_create_blob(VirtIOGPU *g, uint32_t resource_id, uint64_t size)
{
    if (resource_id == 0 || virtio_gpu_find_resource(g, resource_id) != NULL) {
        return -1;
    }

    struct virtio_gpu_simple_resource *res = calloc(1, sizeof(*res));
    res->resource_id = resource_id;
    res->blob_size = size;

    int ret = macos_hostmem_create_resource(g, res);
    if (ret != 0) {
        free(res);
        return ret;
    }

    QTAILQ_INSERT_HEAD(&g->reslist, res, next);

    return 0;
}

/* Simulated QEMU VirtIO GPU Command: VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D */
static int virtio_gpu_transfer_to_host_2d(VirtIOGPU *g, uint32_t resource_id, const void *src_data, size_t data_size)
{
    struct virtio_gpu_simple_resource *res = virtio_gpu_find_resource(g, resource_id);
    if (!res || !res->blob || data_size > res->blob_size) {
        return -1;
    }

    memcpy(res->blob, src_data, data_size);
    return 0;
}

/* Simulated QEMU VirtIO GPU Command: VIRTIO_GPU_CMD_RESOURCE_UNREF */
static int virtio_gpu_resource_unref(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return -1;
    }

    QTAILQ_REMOVE(&g->reslist, res, next);
    macos_hostmem_destroy_resource(g, res);
    free(res);
    return 0;
}

int main(int argc, char **argv)
{
    printf("===================================================================\n");
    printf("  VirtIO GPU -> HostMemoryBackend -> IOSurface -> Resource Table  \n");
    printf("===================================================================\n\n");

    VirtIOGPU g;
    QTAILQ_INIT(&g.reslist);

    /* Step 1: Guest issues RESOURCE_CREATE_BLOB (res_id=42, size=1MB) */
    printf("[Step 1] Guest issues VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB (ID: 42, Size: 1MB)...\n");
    int ret = virtio_gpu_resource_create_blob(&g, 42, 1024 * 1024);
    assert(ret == 0);

    /* Step 2: Lookup in QEMU Resource Table */
    printf("[Step 2] QEMU Resource Table Lookup (reslist)...");
    struct virtio_gpu_simple_resource *res = virtio_gpu_find_resource(&g, 42);
    assert(res != NULL);
    assert(virtio_gpu_hostmem_get_iosurface(res) != NULL);
    assert(virtio_gpu_hostmem_get_iosurface_id(res) > 0);
    assert(res->blob != NULL);
    printf(" [FOUND: IOSurface ID %u, Mapped Addr %p]\n", virtio_gpu_hostmem_get_iosurface_id(res), res->blob);

    /* Step 3: Transfer to Host (VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D) */
    printf("[Step 3] Guest issues VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D...\n");
    uint32_t pattern[4] = { 0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x87654321 };
    ret = virtio_gpu_transfer_to_host_2d(&g, 42, pattern, sizeof(pattern));
    assert(ret == 0);

    /* Verify data in IOSurface UMA shared memory */
    uint32_t *host_ptr = (uint32_t *)res->blob;
    assert(host_ptr[0] == 0xDEADBEEF);
    assert(host_ptr[1] == 0xCAFEBABE);
    assert(host_ptr[2] == 0x12345678);
    assert(host_ptr[3] == 0x87654321);
    printf("  -> Verified IOSurface RAM payload: 0x%X 0x%X 0x%X 0x%X [MATCH]\n",
           host_ptr[0], host_ptr[1], host_ptr[2], host_ptr[3]);

    /* Step 4: Subsequent GPU / Display Operation Access */
    printf("[Step 4] Subsequent Display/GPU operation retrieving IOSurfaceRef & ID...\n");
    IOSurfaceRef surf = (IOSurfaceRef)virtio_gpu_hostmem_get_iosurface(res);
    assert(surf != NULL);
    assert(IOSurfaceGetID(surf) == virtio_gpu_hostmem_get_iosurface_id(res));
    printf("  -> Metal/Display engine acquired IOSurfaceRef (ID: %u, AllocSize: %zu) [PASSED]\n",
           virtio_gpu_hostmem_get_iosurface_id(res), IOSurfaceGetAllocSize(surf));

    /* Step 5: Guest issues RESOURCE_UNREF */
    printf("[Step 5] Guest issues VIRTIO_GPU_CMD_RESOURCE_UNREF...\n");
    ret = virtio_gpu_resource_unref(&g, 42);
    assert(ret == 0);
    assert(virtio_gpu_find_resource(&g, 42) == NULL);
    printf("  -> Resource 42 removed from reslist & IOSurface CFRelease complete [PASSED]\n");

    printf("\n>>> FULL END-TO-END PIPELINE VALIDATION PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
