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
#include "qemu/main-loop.h"
#include "qemu/iov.h"
#include "hw/virtio/virtio-gpu.h"
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
 * Persistent bidirectional Unix Domain Socket channel (gpu.sock)
 *
 * Wire protocol:
 *   [0x01][4B resource_id][4B iosurface_id] - GPUResourceCreated  (QEMU -> Host)
 *   [0x02][4B resource_id]                  - GPUResourceDestroyed (QEMU -> Host)
 *   [0x03][4B resource_id]                  - CMD_TRANSFER_RESOURCE (Host -> QEMU)
 *   [0x04][4B resource_id]                  - CMD_TRANSFER_DONE     (QEMU -> Host)
 */
static const char *aperture_gpu_sock_path(void)
{
    const char *p = getenv("APERTURE_GPU_SOCK");
    if (!p || *p == '\0') {
        return "/tmp/aperture_gpu.sock";
    }
    return p;
}

#include <stdatomic.h>

static int s_gpu_sock_fd = -1;
static VirtIOGPU *s_gpu_device = NULL;
static QEMUTimer *s_gpu_sock_timer = NULL;

/*
 * Pending-notify ring: VKR threads write (resource_id, iosurface_id) pairs here;
 * the QEMU I/O thread's BH drains them and sends the 0x01 notify over gpu.sock.
 * Uses lock-free atomic read/write indices with a power-of-2 ring size.
 */
#define NOTIFY_RING_SIZE 128  /* must be power of 2 */
typedef struct {
    uint32_t resource_id;
    uint32_t iosurface_id;
} PendingNotify;

static PendingNotify  s_notify_ring[NOTIFY_RING_SIZE];
static _Atomic uint32_t s_notify_head = 0;  /* written by VKR threads */
static _Atomic uint32_t s_notify_tail = 0;  /* read/drained by I/O thread */
static QEMUBH *s_notify_bh = NULL;           /* scheduled by VKR threads, runs on I/O thread */

static int macos_hostmem_create_resource(VirtIOGPU *g, struct virtio_gpu_simple_resource *res);
static int gpu_sock_ensure_connected(void);

static void gpu_sock_timer_cb(void *opaque)
{
    /* Runs on I/O thread — safe to call gpu_sock_ensure_connected. */
    if (s_gpu_sock_fd < 0) {
        if (gpu_sock_ensure_connected() < 0) {
            if (s_gpu_sock_timer) {
                timer_mod(s_gpu_sock_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 250);
            }
        }
        /* If connection succeeded, drain any pending notifies. */
    }
    /* After connecting, drain the pending notify ring. */
    if (s_gpu_sock_fd >= 0) {
        uint32_t tail = atomic_load_explicit(&s_notify_tail, memory_order_acquire);
        uint32_t head = atomic_load_explicit(&s_notify_head, memory_order_acquire);
        while (tail != head) {
            PendingNotify *pn = &s_notify_ring[tail & (NOTIFY_RING_SIZE - 1)];
            uint8_t buf[9];
            buf[0] = 0x01;
            buf[1] = (pn->resource_id  >> 24) & 0xFF;
            buf[2] = (pn->resource_id  >> 16) & 0xFF;
            buf[3] = (pn->resource_id  >>  8) & 0xFF;
            buf[4] = (pn->resource_id       ) & 0xFF;
            buf[5] = (pn->iosurface_id >> 24) & 0xFF;
            buf[6] = (pn->iosurface_id >> 16) & 0xFF;
            buf[7] = (pn->iosurface_id >>  8) & 0xFF;
            buf[8] = (pn->iosurface_id       ) & 0xFF;
            ssize_t n = send(s_gpu_sock_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) { break; } /* will retry next timer fire or BH */
            tail++;
            atomic_store_explicit(&s_notify_tail, tail, memory_order_release);
        }
    }
}

/* Called from VKR threads — only schedules work; no I/O ops. */
static void gpu_sock_schedule_reconnect(void)
{
    if (!s_gpu_sock_timer) {
        /* Timer creation is safe only on the I/O thread.
         * At first call we are on the I/O thread (device_realize),
         * subsequent calls (from VKR threads) skip creation if already set. */
        s_gpu_sock_timer = timer_new_ms(QEMU_CLOCK_REALTIME, gpu_sock_timer_cb, NULL);
    }
    if (s_gpu_sock_fd < 0 && s_gpu_sock_timer) {
        timer_mod(s_gpu_sock_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
    }
}

/* BH handler — runs on I/O thread; drains pending notifies and retries connection. */
static void gpu_notify_bh_handler(void *opaque)
{
    gpu_sock_timer_cb(NULL); /* reuse the same drain logic */
}

static void gpu_sock_disconnect(const char *reason)
{
    fprintf(stderr, "[QEMU-HOSTMEM-SOCK] 🔌 gpu_sock_disconnect called (fd=%d, reason='%s')\n", s_gpu_sock_fd, reason ? reason : "unknown");
    fflush(stderr);
    if (s_gpu_sock_fd >= 0) {
        qemu_set_fd_handler(s_gpu_sock_fd, NULL, NULL, NULL);
        close(s_gpu_sock_fd);
        s_gpu_sock_fd = -1;
    }
    gpu_sock_schedule_reconnect();
}

/*
 * Called once from virtio_gpu_virgl_init() which runs on the QEMU I/O thread.
 * Creates the BH and timer, and kicks the first connection attempt so QEMU
 * connects to gpu.sock before the VKR threads begin issuing notify_created calls.
 */
__attribute__((visibility("default")))
void virtio_gpu_hostmem_init_iothread(VirtIOGPU *g)
{
    fprintf(stderr, "[QEMU-HOSTMEM] virtio_gpu_hostmem_init_iothread called (g=%p)\n", (void*)g);
    fflush(stderr);
    if (g) {
        s_gpu_device = g;
    }
    if (!s_notify_bh) {
        s_notify_bh = qemu_bh_new(gpu_notify_bh_handler, NULL);
        fprintf(stderr, "[QEMU-HOSTMEM] BH created (s_notify_bh=%p)\n", (void*)s_notify_bh);
        fflush(stderr);
    }
    if (!s_gpu_sock_timer) {
        s_gpu_sock_timer = timer_new_ms(QEMU_CLOCK_REALTIME, gpu_sock_timer_cb, NULL);
        fprintf(stderr, "[QEMU-HOSTMEM] Timer created (s_gpu_sock_timer=%p)\n", (void*)s_gpu_sock_timer);
        fflush(stderr);
    }
    /* Attempt immediate connection — succeeds if gpu.sock is already listening. */
    if (s_gpu_sock_fd < 0) {
        fprintf(stderr, "[QEMU-HOSTMEM] Attempting initial connection...\n");
        fflush(stderr);
        if (gpu_sock_ensure_connected() >= 0) {
            fprintf(stderr, "[QEMU-HOSTMEM] Connected to gpu.sock at init (fd=%d)\n", s_gpu_sock_fd);
            fflush(stderr);
        } else {
            /* Schedule first retry in 50ms */
            timer_mod(s_gpu_sock_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 50);
            fprintf(stderr, "[QEMU-HOSTMEM] gpu.sock not ready at init, retry in 50ms\n");
            fflush(stderr);
        }
    }
    fprintf(stderr, "[QEMU-HOSTMEM] init_iothread complete\n");
    fflush(stderr);
}

static void gpu_sock_read_cb(void *opaque)
{
    VirtIOGPU *g = (VirtIOGPU *)opaque;
    if (s_gpu_sock_fd < 0) {
        return;
    }

    uint8_t header;
    ssize_t n = recv(s_gpu_sock_fd, &header, 1, MSG_DONTWAIT);
    if (n <= 0) {
        if (n == 0) {
            gpu_sock_disconnect("EOF on recv");
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            char reason_buf[128];
            snprintf(reason_buf, sizeof(reason_buf), "recv error: %s (errno=%d)", strerror(errno), errno);
            gpu_sock_disconnect(reason_buf);
        }
        return;
    }

    if (header == 0x03) {
        /* CMD_TRANSFER_RESOURCE: 4 bytes big-endian resource_id */
        uint8_t res_buf[4];
        size_t recvd = 0;
        while (recvd < 4) {
            ssize_t r = recv(s_gpu_sock_fd, res_buf + recvd, 4 - recvd, 0);
            if (r <= 0) {
                gpu_sock_disconnect("recv error reading resource_id payload");
                return;
            }
            recvd += (size_t)r;
        }

        uint32_t resource_id = ((uint32_t)res_buf[0] << 24) |
                               ((uint32_t)res_buf[1] << 16) |
                               ((uint32_t)res_buf[2] <<  8) |
                               ((uint32_t)res_buf[3]);

        /* Execute Zero-Encode Host-Initiated Direct Frame Transfer */
        fprintf(stderr, "[QEMU-HOSTMEM] 0x03 cmd received for res_id=%u (g=%p, s_gpu_device=%p)\n", resource_id, g, s_gpu_device);
        if (g || s_gpu_device) {
            VirtIOGPU *dev = g ? g : s_gpu_device;
            struct virtio_gpu_simple_resource *res = virtio_gpu_find_resource(dev, resource_id);
            if (res) {
                fprintf(stderr, "[QEMU-HOSTMEM] res %u found: blob_size=%" PRIu64 ", w=%u, h=%u, remapped=%p, iov=%p, iov_cnt=%u\n",
                        resource_id, res->blob_size, res->width, res->height, res->remapped, res->iov, res->iov_cnt);
                if (!res->remapped && !res->hostmem_priv) {
                    if (res->blob_size == 0) {
                        res->blob_size = (size_t)res->width * res->height * 4;
                    }
                    if (res->blob_size > 0) {
                        macos_hostmem_create_resource(dev, res);
                    }
                }
                
                size_t transfer_size = res->blob_size ? (size_t)res->blob_size
                                                       : ((size_t)res->width * res->height * 4);
                const uint8_t *pixel_src = NULL;
                bool free_pixel_src = false;

                if (res->iov && res->iov_cnt > 0 && transfer_size > 0) {
                    /* Standard 2D resource path: pixel data lives in guest-RAM iovecs. */
                    uint8_t *temp_buf = g_malloc0(transfer_size);
                    if (res->remapped) {
                        iov_to_buf(res->iov, res->iov_cnt, 0, res->remapped, transfer_size);
                    }
                    iov_to_buf(res->iov, res->iov_cnt, 0, temp_buf, transfer_size);
                    pixel_src = temp_buf;
                    free_pixel_src = true;
                    fprintf(stderr, "[QEMU-HOSTMEM] res %u: reading via iov_to_buf (2D path)\n", resource_id);
                } else if (res->remapped && transfer_size > 0) {
                    /* hostmem_priv IOSurface path: remapped is locked IOSurface base address. */
                    fprintf(stderr, "[QEMU-HOSTMEM] res %u: reading from res->remapped (hostmem path) %p\n",
                            resource_id, res->remapped);
                    uint8_t *temp_buf = g_malloc0(transfer_size);
                    memcpy(temp_buf, res->remapped, transfer_size);
                    pixel_src = temp_buf;
                    free_pixel_src = true;
                } else {
                    /* Blob resource path: pixel data lives in virgl's map_fixed (vm_remap'd IOSurface). */
                    size_t blob_sz = 0;
                    void *blob_map = virtio_gpu_hostmem_get_blob_map(dev, resource_id, &blob_sz);
                    if (blob_map && blob_sz > 0) {
                        if (transfer_size == 0) {
                            transfer_size = blob_sz;
                        }
                        fprintf(stderr, "[QEMU-HOSTMEM] res %u: reading from blob map_fixed %p size=%zu (blob path)\n",
                                resource_id, blob_map, transfer_size);
                        uint8_t *temp_buf = g_malloc0(transfer_size);
                        memcpy(temp_buf, blob_map, transfer_size);
                        pixel_src = temp_buf;
                        free_pixel_src = true;
                    } else {
                        fprintf(stderr, "[QEMU-HOSTMEM] res %u: no pixel data — remapped=%p, iov=%p, blob_map=%p\n",
                                resource_id, res->remapped, res->iov, blob_map);
                    }
                }

                if (pixel_src && transfer_size > 0) {
                    uint32_t *pixels = (uint32_t *)pixel_src;
                    size_t pixel_count = transfer_size / 4;
                    size_t non_zero = 0;
                    uint32_t first_pixel = 0;
                    for (size_t i = 0; i < pixel_count; i++) {
                        if (pixels[i] != 0) {
                            non_zero++;
                            if (first_pixel == 0) first_pixel = pixels[i];
                        }
                    }
                    fprintf(stderr, "[QEMU-HOSTMEM-AUDIT] res_id=%u: non_zero=%zu/%zu (first=0x%08X)\n",
                            resource_id, non_zero, pixel_count, first_pixel);

                    FILE *rf = fopen("/tmp/aperture_display_2.raw", "wb");
                    if (rf) {
                        fwrite(pixel_src, 1, transfer_size, rf);
                        fclose(rf);
                    }
                    FILE *prf = fopen("/private/tmp/aperture_display_2.raw", "wb");
                    if (prf) {
                        fwrite(pixel_src, 1, transfer_size, prf);
                        fclose(prf);
                    }

                    FILE *af = fopen("/tmp/aperture_2d_audit.txt", "w");
                    if (af) {
                        fprintf(af, "res_id=%u non_zero=%zu total=%zu first=0x%08X\n",
                                resource_id, non_zero, pixel_count, first_pixel);
                        fclose(af);
                    }
                    FILE *paf = fopen("/private/tmp/aperture_2d_audit.txt", "w");
                    if (paf) {
                        fprintf(paf, "res_id=%u non_zero=%zu total=%zu first=0x%08X\n",
                                resource_id, non_zero, pixel_count, first_pixel);
                        fclose(paf);
                    }
                    FILE *wf = fopen("/Users/skanda/Documents/Coding Projects/Aperture/virglrender/build/2d_audit.txt", "w");
                    if (wf) {
                        fprintf(wf, "res_id=%u non_zero=%zu total=%zu first=0x%08X\n",
                                resource_id, non_zero, pixel_count, first_pixel);
                        fclose(wf);
                    }
                    if (free_pixel_src) { g_free((void *)pixel_src); }
                }
            } else {
                fprintf(stderr, "[QEMU-HOSTMEM] res %u NOT FOUND in dev %p!\n", resource_id, dev);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "[QEMU-HostMem] Transfer requested for missing res_id=%u (ignoring safely)\n",
                              resource_id);
            }
        }

        /* Send CMD_TRANSFER_DONE ack: [0x04][4B resource_id] */
        uint8_t ack[5];
        ack[0] = 0x04;
        ack[1] = res_buf[0];
        ack[2] = res_buf[1];
        ack[3] = res_buf[2];
        ack[4] = res_buf[3];

        size_t sent = 0;
        while (sent < sizeof(ack) && s_gpu_sock_fd >= 0) {
            ssize_t s = send(s_gpu_sock_fd, ack + sent, sizeof(ack) - sent, MSG_DONTWAIT);
            if (s <= 0) {
                if (s < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                    gpu_sock_disconnect("send error on ACK (EPIPE/ECONNRESET)");
                }
                break;
            }
            sent += (size_t)s;
        }
    }
}

static int gpu_sock_ensure_connected(void)
{
    if (s_gpu_sock_fd >= 0) {
        return s_gpu_sock_fd;
    }

    const char *sock_path = aperture_gpu_sock_path();
    if (!sock_path || *sock_path == '\0') {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_len = sizeof(addr);
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err1 = errno;
        close(fd);
        /* Try /private/tmp variant if sock_path starts with /tmp */
        if (strncmp(sock_path, "/tmp/", 5) == 0) {
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                return -1;
            }
            char priv_path[256];
            snprintf(priv_path, sizeof(priv_path), "/private%s", sock_path);
            strncpy(addr.sun_path, priv_path, sizeof(addr.sun_path) - 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                fprintf(stderr, "[QEMU-HOSTMEM-SOCK] connect failed for %s (%s) and %s (%s)\n",
                        sock_path, strerror(err1), priv_path, strerror(errno));
                close(fd);
                return -1;
            }
        } else {
            fprintf(stderr, "[QEMU-HOSTMEM-SOCK] connect failed for %s: errno=%d (%s)\n",
                    sock_path, err1, strerror(err1));
            return -1;
        }
    }

    fprintf(stderr, "[QEMU-HOSTMEM-SOCK] Successfully connected to %s (fd=%d)\n", addr.sun_path, fd);
    s_gpu_sock_fd = fd;
    qemu_set_fd_handler(s_gpu_sock_fd, gpu_sock_read_cb, NULL, s_gpu_device);
    return s_gpu_sock_fd;
}

static void gpu_sock_send(const uint8_t *buf, size_t len)
{
    int fd = gpu_sock_ensure_connected();
    if (fd < 0) {
        return;
    }

    size_t sent = 0;
    while (sent < len && s_gpu_sock_fd >= 0) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_DONTWAIT);
        if (n <= 0) {
            if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                gpu_sock_disconnect("send error on notify (EPIPE/ECONNRESET)");
            }
            break;
        }
        sent += (size_t)n;
    }
}

__attribute__((visibility("default")))
void virtio_gpu_hostmem_notify_created(uint32_t resource_id, uint32_t iosurface_id)
{
    fprintf(stderr, "[QEMU-HOSTMEM-NOTIFY-CREATED] res_id=%u, iosurf_id=%u\n", resource_id, iosurface_id);
    FILE *f = fopen("/tmp/aperture_latest_2d_iosurface.txt", "w");
    if (f) {
        fprintf(f, "%u %u\n", resource_id, iosurface_id);
        fclose(f);
    }
    FILE *pf = fopen("/private/tmp/aperture_latest_2d_iosurface.txt", "w");
    if (pf) {
        fprintf(pf, "%u %u\n", resource_id, iosurface_id);
        fclose(pf);
    }
    FILE *wf = fopen("/Users/skanda/Documents/Coding Projects/Aperture/virglrender/build/latest_2d_iosurface.txt", "w");
    if (wf) {
        fprintf(wf, "%u %u\n", resource_id, iosurface_id);
        fclose(wf);
    }

    /*
     * VKR-thread-safe path: enqueue into the ring buffer and schedule the BH.
     * The BH runs on the QEMU I/O thread, where it is safe to call
     * gpu_sock_ensure_connected() and qemu_set_fd_handler().
     */
    uint32_t head = atomic_load_explicit(&s_notify_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s_notify_tail, memory_order_acquire);
    if (((head + 1) & (NOTIFY_RING_SIZE - 1)) != (tail & (NOTIFY_RING_SIZE - 1))) {
        /* Ring not full — write entry */
        uint32_t idx = head & (NOTIFY_RING_SIZE - 1);
        s_notify_ring[idx].resource_id  = resource_id;
        s_notify_ring[idx].iosurface_id = iosurface_id;
        atomic_store_explicit(&s_notify_head, head + 1, memory_order_release);
    } else {
        fprintf(stderr, "[QEMU-HOSTMEM-NOTIFY-CREATED] ring full, dropping notify for res_id=%u!\n", resource_id);
    }

    /* Schedule BH (safe to call from any thread) to drain the ring on the I/O thread. */
    if (s_notify_bh) {
        qemu_bh_schedule(s_notify_bh);
    } else {
        /* BH not yet created (device_realize not called yet) — fall back to timer. */
        gpu_sock_schedule_reconnect();
    }
}

__attribute__((visibility("default")))
void virtio_gpu_hostmem_notify_destroyed(uint32_t resource_id)
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
    if (!g || !res) {
        return -EINVAL;
    }
    s_gpu_device = g;

    /* One-time I/O-thread initialization of the BH and reconnect timer.
     * This function runs on the QEMU I/O thread so it is safe here. */
    if (!s_notify_bh) {
        s_notify_bh = qemu_bh_new(gpu_notify_bh_handler, NULL);
        fprintf(stderr, "[QEMU-HOSTMEM] BH created, starting initial connection attempt\n");
    }
    if (!s_gpu_sock_timer) {
        s_gpu_sock_timer = timer_new_ms(QEMU_CLOCK_REALTIME, gpu_sock_timer_cb, NULL);
    }
    /* Kick first connect attempt immediately via BH (I/O thread, next iteration). */
    if (s_gpu_sock_fd < 0 && s_notify_bh) {
        qemu_bh_schedule(s_notify_bh);
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

    CFDictionarySetValue(props, CFSTR("IOSurfaceIsGlobal"), kCFBooleanTrue);

    int bpe = 4;
    CFNumberRef bpe_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpe);
    CFDictionarySetValue(props, kIOSurfaceBytesPerElement, bpe_num);
    CFRelease(bpe_num);

    uint32_t pixel_format = 0x52474241; // 'RGBA'
    CFNumberRef fmt_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
    CFDictionarySetValue(props, kIOSurfacePixelFormat, fmt_num);
    CFRelease(fmt_num);

    uint64_t total_pixels = size / 4;
    int w = (int)total_pixels;
    int h = 1;

    /* Detect 2D render buffers by finding the factorization closest to standard display aspect ratios */
    if (size >= 320 * 240 * 4) {
        float best_diff = 1000.0f;
        for (int test_w = 3840; test_w >= 320; test_w -= 16) {
            if (total_pixels % test_w == 0) {
                int test_h = (int)(total_pixels / test_w);
                float ar = (float)test_w / (float)test_h;
                if (ar >= 0.5f && ar <= 2.4f) {
                    float diff_16_10 = fabsf(ar - 1.6f);
                    float diff_16_9  = fabsf(ar - 1.777f);
                    float diff_4_3   = fabsf(ar - 1.333f);
                    float diff_port  = fabsf(ar - 0.625f);
                    float min_d = fminf(fminf(diff_16_10, diff_16_9), fminf(diff_4_3, diff_port));
                    if (min_d < best_diff) {
                        best_diff = min_d;
                        w = test_w;
                        h = test_h;
                    }
                }
            }
        }
    }
    int bpr = w * 4;

    CFNumberRef w_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &w);
    CFDictionarySetValue(props, kIOSurfaceWidth, w_num);
    CFRelease(w_num);

    CFNumberRef h_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &h);
    CFDictionarySetValue(props, kIOSurfaceHeight, h_num);
    CFRelease(h_num);

    CFNumberRef bpr_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bpr);
    CFDictionarySetValue(props, kIOSurfaceBytesPerRow, bpr_num);
    CFRelease(bpr_num);

    CFDictionarySetValue(props, CFSTR("IOSurfaceIsGlobal"), kCFBooleanTrue);

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
    virtio_gpu_hostmem_notify_created(res->resource_id, hm->iosurface_id);

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
    virtio_gpu_hostmem_notify_destroyed(res->resource_id);

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
