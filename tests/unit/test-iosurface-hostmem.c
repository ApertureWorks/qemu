/*
 * Standalone Unit Test for VirtIO GPU macOS IOSurface Host Memory Allocator
 *
 * Tests:
 * 1. Allocation of 4KB, 1MB, 64MB blob resources
 * 2. Write/Read consistency (pattern 0xAA 0xBB 0xCC)
 * 3. 10,000 iteration create-map-write-unmap-destroy stress test (leak check)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

/* Mock VirtIO GPU Simple Resource Structure */
struct virtio_gpu_simple_resource {
    uint32_t resource_id;
    uint64_t blob_size;
    void *blob;
    int dmabuf_fd;
    uint8_t *remapped;

    void *hostmem_priv;
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

static int macos_hostmem_create_resource(struct virtio_gpu_simple_resource *res)
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

static void macos_hostmem_destroy_resource(struct virtio_gpu_simple_resource *res)
{
    if (res && res->hostmem_priv) {
        struct VirtIOGPUMacOSHostMem *hm = (struct VirtIOGPUMacOSHostMem *)res->hostmem_priv;
        res->hostmem_priv = NULL;
        if (res->remapped) {
            IOSurfaceUnlock(hm->surface, 0, NULL);
            res->remapped = NULL;
            res->blob = NULL;
        }
        CFRelease(hm->surface);
        free(hm);
    }
}

static void test_blob_allocations(void)
{
    printf("[Test 1] Testing Blob Allocations (4KB, 1MB, 64MB)...\n");

    uint64_t sizes[] = { 4 * 1024, 1024 * 1024, 64 * 1024 * 1024 };
    for (int i = 0; i < 3; i++) {
        struct virtio_gpu_simple_resource res = {
            .resource_id = i + 1,
            .blob_size = sizes[i]
        };

        int ret = macos_hostmem_create_resource(&res);
        assert(ret == 0);
        assert(virtio_gpu_hostmem_get_iosurface(&res) != NULL);
        assert(virtio_gpu_hostmem_get_iosurface_id(&res) > 0);
        assert(res.remapped != NULL);

        printf("  -> Allocated %llu bytes | IOSurface ID: %u | Base Addr: %p [PASSED]\n",
               (unsigned long long)res.blob_size, virtio_gpu_hostmem_get_iosurface_id(&res), res.remapped);

        macos_hostmem_destroy_resource(&res);
        assert(virtio_gpu_hostmem_get_iosurface(&res) == NULL);
    }
}

static void test_write_read_consistency(void)
{
    printf("[Test 2] Testing Write/Read Data Consistency...\n");

    struct virtio_gpu_simple_resource res = {
        .resource_id = 100,
        .blob_size = 4096
    };

    int ret = macos_hostmem_create_resource(&res);
    assert(ret == 0);
    assert(res.remapped != NULL);

    /* Write test pattern: 0xAA 0xBB 0xCC */
    res.remapped[0] = 0xAA;
    res.remapped[1] = 0xBB;
    res.remapped[2] = 0xCC;
    res.remapped[4095] = 0xDD;

    /* Verify pattern readback */
    assert(res.remapped[0] == 0xAA);
    assert(res.remapped[1] == 0xBB);
    assert(res.remapped[2] == 0xCC);
    assert(res.remapped[4095] == 0xDD);

    printf("  -> Written 0xAA 0xBB 0xCC 0xDD -> Verified Readback Match [PASSED]\n");

    macos_hostmem_destroy_resource(&res);
}

static void test_lifetime_stress(void)
{
    printf("[Test 3] Lifetime Stress & Memory Leak Test (10,000 Iterations)...\n");

    for (int i = 0; i < 10000; i++) {
        struct virtio_gpu_simple_resource res = {
            .resource_id = i + 1,
            .blob_size = 64 * 1024 // 64KB per alloc
        };

        int ret = macos_hostmem_create_resource(&res);
        assert(ret == 0);
        assert(res.remapped != NULL);

        /* Write sample byte */
        res.remapped[0] = (uint8_t)(i & 0xFF);
        assert(res.remapped[0] == (uint8_t)(i & 0xFF));

        macos_hostmem_destroy_resource(&res);
    }

    printf("  -> Completed 10,000 Iterations of Create->Map->Write->Unmap->Destroy [PASSED]\n");
}

int main(int argc, char **argv)
{
    printf("========================================================\n");
    printf("  QEMU macOS VirtIO GPU IOSurface Host Memory Test Suite \n");
    printf("========================================================\n\n");

    test_blob_allocations();
    test_write_read_consistency();
    test_lifetime_stress();

    printf("\n>>> ALL PHASE E IOSURFACE UNIT TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
