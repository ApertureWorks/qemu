/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-gpu-hostmem.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#if defined(__aarch64__)
#include <Hypervisor/Hypervisor.h>
#endif
#endif

#include "ui/egl-helpers.h"

#include <virglrenderer.h>

/*
 * VIRGL_CHECK_VERSION available since libvirglrenderer 1.0.1 and was fixed
 * in 1.1.0. Undefine bugged version of the macro and provide our own.
 */
#if defined(VIRGL_CHECK_VERSION) && \
    VIRGL_VERSION_MAJOR == 1 && VIRGL_VERSION_MINOR < 1
#undef VIRGL_CHECK_VERSION
#endif

#ifndef VIRGL_CHECK_VERSION
#define VIRGL_CHECK_VERSION(major, minor, micro) \
    (VIRGL_VERSION_MAJOR > (major) || \
     VIRGL_VERSION_MAJOR == (major) && VIRGL_VERSION_MINOR > (minor) || \
     VIRGL_VERSION_MAJOR == (major) && VIRGL_VERSION_MINOR == (minor) && \
     VIRGL_VERSION_MICRO >= (micro))
#endif

#define VIRGL_HAS_MAP_FIXED \
    (VIRGL_CHECK_VERSION(1, 3, 0) && !IS_ENABLED(CONFIG_WIN32))

struct virtio_gpu_virgl_resource {
    struct virtio_gpu_simple_resource base;
    MemoryRegion *mr;
    void *map_fixed;
};

static struct virtio_gpu_virgl_resource *
virtio_gpu_virgl_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return NULL;
    }

    return container_of(res, struct virtio_gpu_virgl_resource, base);
}

/* Public helper — used by virtio-gpu-hostmem-macos.c to read blob pixel data.
 * Returns the vm_remap'd host VA of the blob resource, or NULL if not a virgl
 * blob resource or if the blob has not yet been mapped by the guest.
 * blob_size_out (if non-NULL) is filled with the allocated blob size. */
void *virtio_gpu_hostmem_get_blob_map(VirtIOGPU *g, uint32_t resource_id, size_t *blob_size_out)
{
    struct virtio_gpu_virgl_resource *vres = virtio_gpu_virgl_find_resource(g, resource_id);
    if (!vres) {
        return NULL;
    }
    if (blob_size_out) {
        *blob_size_out = (size_t)vres->base.blob_size;
    }
    return vres->map_fixed;
}

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
static void *
virgl_get_egl_display(G_GNUC_UNUSED void *cookie)
{
    return qemu_egl_display;
}
#endif

#if VIRGL_VERSION_MAJOR >= 1
enum virtio_gpu_virgl_hostmem_region_mapping_state {
    VIRTIO_GPU_MR_MAPPED,
    VIRTIO_GPU_MR_UNMAP_STARTED,
    VIRTIO_GPU_MR_UNMAP_COMPLETED,
};

struct virtio_gpu_virgl_hostmem_region {
    Object parent_obj;
    MemoryRegion mr;
    struct VirtIOGPU *g;
    enum virtio_gpu_virgl_hostmem_region_mapping_state mapping_state;
};

#define TYPE_VIRTIO_GPU_VIRGL_HOSTMEM_REGION "virtio-gpu-virgl-hostmem-region"

OBJECT_DECLARE_SIMPLE_TYPE(virtio_gpu_virgl_hostmem_region,
                           VIRTIO_GPU_VIRGL_HOSTMEM_REGION)

static struct virtio_gpu_virgl_hostmem_region *
to_hostmem_region(MemoryRegion *mr)
{
    return container_of(mr, struct virtio_gpu_virgl_hostmem_region, mr);
}

static void virtio_gpu_virgl_resume_cmdq_bh(void *opaque)
{
    VirtIOGPU *g = opaque;

    if (!virtio_gpu_virgl_update_render_state(g)) {
        return;
    }

    virtio_gpu_process_cmdq(g);
}

/*
 * MR could outlive the resource if MR's reference is held outside of
 * virtio-gpu. In order to prevent unmapping resource while MR is alive,
 * and thus, making the data pointer invalid, we will block virtio-gpu
 * command processing until MR is fully unreferenced and freed.
 */
static void virtio_gpu_virgl_hostmem_region_finalize(Object *obj)
{
    struct virtio_gpu_virgl_hostmem_region *vmr = VIRTIO_GPU_VIRGL_HOSTMEM_REGION(obj);
    VirtIOGPUBase *b;
    VirtIOGPUGL *gl;

    if (!vmr->g) {
        return;
    }

    vmr->mapping_state = VIRTIO_GPU_MR_UNMAP_COMPLETED;

    b = VIRTIO_GPU_BASE(vmr->g);
    b->renderer_blocked--;

    /*
     * memory_region_unref() is executed from RCU thread context, while
     * virglrenderer works only on the main-loop thread that's holding GL
     * context.
     */
    gl = VIRTIO_GPU_GL(vmr->g);
    qemu_bh_schedule(gl->cmdq_resume_bh);
}

static const TypeInfo virtio_gpu_virgl_hostmem_region_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_VIRTIO_GPU_VIRGL_HOSTMEM_REGION,
    .instance_size = sizeof(struct virtio_gpu_virgl_hostmem_region),
    .instance_finalize = virtio_gpu_virgl_hostmem_region_finalize
};

static void virtio_gpu_virgl_types(void)
{
    type_register_static(&virtio_gpu_virgl_hostmem_region_info);
}

type_init(virtio_gpu_virgl_types)

static int
virtio_gpu_virgl_map_resource_blob(VirtIOGPU *g,
                                   struct virtio_gpu_virgl_resource *res,
                                   uint64_t offset)
{
    g_autofree char *name = NULL;
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
#if VIRGL_HAS_MAP_FIXED
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
#endif
    MemoryRegion *mr;
    uint64_t size;
    void *data;
    int ret;

    if (!virtio_gpu_hostmem_enabled(b->conf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: hostmem disabled\n", __func__);
        return -EOPNOTSUPP;
    }

#if VIRGL_HAS_MAP_FIXED && !defined(__APPLE__)
    /*
     * virgl_renderer_resource_map_fixed() allows to create multiple
     * mappings of the same resource, while virgl_renderer_resource_map()
     * not. Don't allow mapping same resource twice.
     */
    if (res->map_fixed || res->mr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to map(fixed) virgl resource: already mapped\n",
                      __func__);
        return -EBUSY;
    }

    ret = virgl_renderer_resource_map_fixed(res->base.resource_id,
                                            gl->hostmem_mmap + offset);
    switch (ret) {
    case 0:
        res->map_fixed = gl->hostmem_mmap + offset;
        return 0;

    case -EOPNOTSUPP:
    case -EINVAL:
    case -EBADF:
        /*
         * MAP_FIXED is unsupported by this resource (e.g. fd is an IOSurfaceID on macOS).
         * Mapping falls back to a blob subregion / vm_remap method in that case.
         */
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to map(fixed) virgl resource: %s\n",
                      __func__, strerror(-ret));
        return ret;
    }
#endif

    ret = virgl_renderer_resource_map(res->base.resource_id, &data, &size);
    fprintf(stderr, "[VIRGL-MAP-BLOB-CALL] res_id=%u ret=%d data=%p size=%llu\n",
            res->base.resource_id, ret, data, (unsigned long long)size);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to map virgl resource: %s\n",
                      __func__, strerror(-ret));
        return ret;
    }

#ifdef __APPLE__
    void *surf_base = virtio_gpu_hostmem_lookup_iosurface_base(res->base.resource_id);
    if (surf_base) {
        data = surf_base;
        fprintf(stderr, "[VIRGL-MAP-IOSURFACE-DIRECT] ✅ res_id=%u mapped directly to IOSurface base=%p\n",
                res->base.resource_id, data);
    }

    if (gl->hostmem_mmap) {
        vm_address_t target = (vm_address_t)(gl->hostmem_mmap + offset);
        vm_prot_t cur_prot, max_prot;
        kern_return_t kr = vm_remap(mach_task_self(),
                                    &target,
                                    size,
                                    0,
                                    VM_FLAGS_OVERWRITE | VM_FLAGS_FIXED,
                                    mach_task_self(),
                                    (vm_address_t)data,
                                    FALSE,
                                    &cur_prot,
                                    &max_prot,
                                    VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) {
            res->map_fixed = (void *)target;
            fprintf(stderr, "[VIRGL-MAP-BLOB-VM-REMAP-OK] res_id=%u target=%p size=%llu\n",
                    res->base.resource_id, (void*)target, (unsigned long long)size);
        } else {
            fprintf(stderr, "[VIRGL-MAP-BLOB-VM-REMAP-FAIL] res_id=%u kr=%d target=%p data=%p size=%llu\n",
                    res->base.resource_id, kr, (void*)target, data, (unsigned long long)size);
        }

#if defined(__APPLE__) && defined(__aarch64__)
        hwaddr bar_base = b->hostmem.addr;
        uint64_t page_size = qemu_real_host_page_size();
        uint64_t start_gpa = QEMU_ALIGN_DOWN(bar_base + offset, page_size);
        uint64_t end_gpa = QEMU_ALIGN_UP(bar_base + offset + size, page_size);
        uint64_t aligned_size = end_gpa - start_gpa;
        void *aligned_uva = (void *)((uintptr_t)gl->hostmem_mmap + (start_gpa - bar_base));

        hv_vm_unmap(start_gpa, aligned_size);
        hv_return_t hv_ret = hv_vm_map(aligned_uva, start_gpa, aligned_size,
                                      HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        fprintf(stderr, "[VIRGL-HVF-STAGE2-MAP] res_id=%u gpa=0x%llx size=0x%llx uva=%p ret=%d\n",
                res->base.resource_id, (unsigned long long)start_gpa, (unsigned long long)aligned_size, aligned_uva, hv_ret);
#endif
    }
#endif

    return 0;
}

static int
virtio_gpu_virgl_unmap_resource_blob(VirtIOGPU *g,
                                     struct virtio_gpu_virgl_resource *res,
                                     bool *cmd_suspended)
{
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

#ifdef __APPLE__
    if (res->map_fixed && gl->hostmem_mmap) {
        mmap(res->map_fixed, res->base.blob_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

#if defined(__APPLE__) && defined(__aarch64__)
        hwaddr bar_base = b->hostmem.addr;
        uint64_t page_size = qemu_real_host_page_size();
        uint64_t offset = (uintptr_t)res->map_fixed - (uintptr_t)gl->hostmem_mmap;
        uint64_t start_gpa = QEMU_ALIGN_DOWN(bar_base + offset, page_size);
        uint64_t end_gpa = QEMU_ALIGN_UP(bar_base + offset + res->base.blob_size, page_size);
        uint64_t aligned_size = end_gpa - start_gpa;
        void *aligned_uva = (void *)((uintptr_t)gl->hostmem_mmap + (start_gpa - bar_base));

        hv_vm_unmap(start_gpa, aligned_size);
        hv_vm_map(aligned_uva, start_gpa, aligned_size,
                  HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
#endif
        res->map_fixed = NULL;
    }
    return 0;
#else
    struct virtio_gpu_virgl_hostmem_region *vmr;
    MemoryRegion *mr = res->mr;
    int ret;

    trace_virtio_gpu_cmd_res_unmap_blob(res->base.resource_id, mr,
                                        vmr->mapping_state);

    switch (vmr->mapping_state) {
    case VIRTIO_GPU_MR_UNMAP_COMPLETED:
        res->mr = NULL;
        g_free(vmr);

        ret = virgl_renderer_resource_unmap(res->base.resource_id);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: failed to unmap virgl resource: %s\n",
                          __func__, strerror(-ret));
            return ret;
        }
        break;

    case VIRTIO_GPU_MR_MAPPED:
        b->renderer_blocked++;
        vmr->mapping_state = VIRTIO_GPU_MR_UNMAP_STARTED;
        memory_region_del_subregion(&b->hostmem, mr);
        object_unparent(OBJECT(vmr));
        /* Fallthrough */
    case VIRTIO_GPU_MR_UNMAP_STARTED:
        *cmd_suspended = true;
        break;
    }

    return 0;
#endif
}
#endif

static void virgl_cmd_create_resource_2d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_2d c2d;
    struct virgl_renderer_resource_create_args args;
    struct virtio_gpu_virgl_resource *res;

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    if (c2d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c2d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c2d.width;
    res->base.height = c2d.height;
    res->base.format = c2d.format;
    res->base.resource_id = c2d.resource_id;
    res->base.dmabuf_fd = -1;
    res->base.blob_size = (uint64_t)c2d.width * c2d.height * 4;
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);

    args.handle = c2d.resource_id;
    args.target = 2;
    args.format = c2d.format;
    args.bind = (1 << 1);
    args.width = c2d.width;
    args.height = c2d.height;
    args.depth = 1;
    args.array_size = 1;
    args.last_level = 0;
    args.nr_samples = 0;
    args.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;
    virgl_renderer_resource_create(&args, NULL, 0);
}

static void virgl_cmd_create_resource_3d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_3d c3d;
    struct virgl_renderer_resource_create_args args;
    struct virtio_gpu_virgl_resource *res;

    VIRTIO_GPU_FILL_CMD(c3d);
    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    if (c3d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c3d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c3d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c3d.width;
    res->base.height = c3d.height;
    res->base.format = c3d.format;
    res->base.resource_id = c3d.resource_id;
    res->base.dmabuf_fd = -1;
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);

    args.handle = c3d.resource_id;
    args.target = c3d.target;
    args.format = c3d.format;
    args.bind = c3d.bind;
    args.width = c3d.width;
    args.height = c3d.height;
    args.depth = c3d.depth;
    args.array_size = c3d.array_size;
    args.last_level = c3d.last_level;
    args.nr_samples = c3d.nr_samples;
    args.flags = c3d.flags;
    virgl_renderer_resource_create(&args, NULL, 0);
}

static int
virtio_gpu_virgl_resource_unref(VirtIOGPU *g,
                                struct virtio_gpu_virgl_resource *res,
                                bool *suspended)
{
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;
#if VIRGL_VERSION_MAJOR >= 1
    int ret;

    ret = virtio_gpu_virgl_unmap_resource_blob(g, res, suspended);
    if (ret) {
        return ret;
    }
    if (*suspended) {
        return 0;
    }
#endif

    res->base.iov = NULL;
    res->base.iov_cnt = 0;
    virgl_renderer_resource_detach_iov(res->base.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }
    virgl_renderer_resource_unref(res->base.resource_id);

#if defined(CONFIG_DARWIN) || defined(__APPLE__)
    if (res->base.hostmem_priv) {
        virtio_gpu_hostmem_destroy_resource(g, &res->base);
    } else {
        virtio_gpu_hostmem_notify_destroyed(res->base.resource_id);
    }
#endif

    QTAILQ_REMOVE(&g->reslist, &res->base, next);

    g_free(res);

    return 0;
}

static void virgl_cmd_resource_unref(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd,
                                     bool *cmd_suspended)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_virgl_resource *res;

    VIRTIO_GPU_FILL_CMD(unref);
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_virgl_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virtio_gpu_virgl_resource_unref(g, res, cmd_suspended);
}

void virtio_gpu_virgl_resource_destroy(VirtIOGPU *g,
                                       struct virtio_gpu_simple_resource *base,
                                       Error **errp)
{
    struct virtio_gpu_virgl_resource *res;
    bool suspended = false;

    res = container_of(base, struct virtio_gpu_virgl_resource, base);

    if (virtio_gpu_virgl_resource_unref(g, res, &suspended)) {
        error_setg(errp, "failed to destroy virgl resource");
    }
}

static void virgl_cmd_context_create(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;

    VIRTIO_GPU_FILL_CMD(cc);
    fprintf(stderr, "[VIRGL-CTX-CREATE] ctx_id=%u context_init=0x%x nlen=%u name=%s\n",
            cc.hdr.ctx_id, cc.context_init, cc.nlen, cc.debug_name);

    if (cc.context_init) {
        if (!virtio_gpu_context_init_enabled(g->parent_obj.conf)) {
            fprintf(stderr, "[VIRGL-CTX-CREATE-ERR] context_init disabled in conf\n");
            qemu_log_mask(LOG_GUEST_ERROR, "%s: context_init disabled",
                          __func__);
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }

#if VIRGL_VERSION_MAJOR >= 1
        virgl_renderer_context_create_with_flags(cc.hdr.ctx_id,
                                                 cc.context_init,
                                                 cc.nlen,
                                                 cc.debug_name);
        return;
#endif
    }

    virgl_renderer_context_create(cc.hdr.ctx_id, cc.nlen, cc.debug_name);
}

static void virgl_cmd_context_destroy(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    virgl_renderer_context_destroy(cd.hdr.ctx_id);
}

static void virtio_gpu_rect_update(VirtIOGPU *g, int idx, int x, int y,
                                int width, int height)
{
    if (!g->parent_obj.scanout[idx].con) {
        return;
    }

    if (qemu_console_has_gl(g->parent_obj.scanout[idx].con)) {
        qemu_console_gl_update(g->parent_obj.scanout[idx].con, x, y, width, height);
    }
}

static void virgl_cmd_resource_flush(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_flush rf;
    int i;

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        if (g->parent_obj.scanout[i].resource_id != rf.resource_id) {
            continue;
        }
        virtio_gpu_rect_update(g, i, rf.r.x, rf.r.y, rf.r.width, rf.r.height);

        uint32_t iosurf_id = 0;
        struct virtio_gpu_simple_resource *res = virtio_gpu_find_resource(g, rf.resource_id);
        if (res) {
            iosurf_id = virtio_gpu_hostmem_get_iosurface_id(res);
        }
        if (iosurf_id == 0) {
            iosurf_id = virtio_gpu_hostmem_lookup_iosurface_id(rf.resource_id);
        }
        if (iosurf_id > 0) {
            virtio_gpu_hostmem_sync_scanout(rf.resource_id, res);

            uint32_t flush_w = rf.r.width ? rf.r.width : g->parent_obj.scanout[i].width;
            uint32_t flush_h = rf.r.height ? rf.r.height : g->parent_obj.scanout[i].height;
            if (flush_w == 0 && res) flush_w = res->width;
            if (flush_h == 0 && res) flush_h = res->height;

            fprintf(stderr, "[VIRGL-SCANOUT-FLUSH] scanout=%d res_id=%u iosurf_id=%u (%ux%u)\n",
                    i, rf.resource_id, iosurf_id, flush_w, flush_h);
            virtio_gpu_hostmem_notify_scanout_flush(i, iosurf_id, flush_w, flush_h);
        }
    }
}

static void virgl_cmd_set_scanout(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_set_scanout ss;
    int ret;

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }
    g->parent_obj.enable = 1;

    if (ss.resource_id && ss.r.width && ss.r.height) {
        struct virgl_renderer_resource_info info;
        void *d3d_tex2d = NULL;

#if VIRGL_VERSION_MAJOR >= 1
        struct virgl_renderer_resource_info_ext ext;
        memset(&ext, 0, sizeof(ext));
        ret = virgl_renderer_resource_get_info_ext(ss.resource_id, &ext);
        info = ext.base;
        d3d_tex2d = ext.d3d_tex2d;
#else
        memset(&info, 0, sizeof(info));
        ret = virgl_renderer_resource_get_info(ss.resource_id, &info);
#endif
        if (ret) {
            fprintf(stderr, "[VIRGL-SET-SCANOUT-REJECTED] scanout_id=%u res_id=%u ret=%d\n",
                    ss.scanout_id, ss.resource_id, ret);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: illegal resource specified %d\n",
                          __func__, ss.resource_id);
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
            return;
        }

        uint32_t iosurf_id = virtio_gpu_hostmem_lookup_iosurface_id(ss.resource_id);
        if (iosurf_id == 0) {
            iosurf_id = virtio_gpu_hostmem_create_scanout_iosurface(g, ss.resource_id, ss.r.width, ss.r.height);
        }

        fprintf(stderr, "[VIRGL-SET-SCANOUT-ACCEPTED] scanout_id=%u res_id=%u iosurf_id=%u rect=%ux%u+%u+%u tex_id=%u\n",
                ss.scanout_id, ss.resource_id, iosurf_id, ss.r.width, ss.r.height, ss.r.x, ss.r.y, info.tex_id);
        qemu_console_resize(g->parent_obj.scanout[ss.scanout_id].con,
                            ss.r.width, ss.r.height);
        if (info.tex_id > 0) {
            virgl_renderer_force_ctx_0();
            qemu_console_gl_scanout_texture(
                g->parent_obj.scanout[ss.scanout_id].con, info.tex_id,
                info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
                info.width, info.height,
                ss.r.x, ss.r.y, ss.r.width, ss.r.height,
                d3d_tex2d);
        }
    } else {
        fprintf(stderr, "[VIRGL-SET-SCANOUT-DISABLED] scanout_id=%u\n", ss.scanout_id);
        qemu_console_set_surface(g->parent_obj.scanout[ss.scanout_id].con, NULL);
        qemu_console_gl_scanout_disable(g->parent_obj.scanout[ss.scanout_id].con);
    }
    g->parent_obj.scanout[ss.scanout_id].resource_id = ss.resource_id;
}

static void virgl_cmd_submit_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_submit cs;
    void *buf;
    size_t s;

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);

    if (cs.size > VIRTIO_GPU_MAX_CMD_SUBMIT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command buffer too large (%u)\n",
                      __func__, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    buf = g_malloc(cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    if (s != cs.size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: size mismatch (%zd/%d)",
                      __func__, s, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        goto out;
    }

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        g->stats.req_3d++;
        g->stats.bytes_3d += cs.size;
    }

    int ret = virgl_renderer_submit_cmd(buf, cs.hdr.ctx_id, cs.size / 4);
    if (ret) {
        fprintf(stderr, "[VIRGL-SUBMIT-FAIL] ctx_id=%u size=%u ret=%d\n", cs.hdr.ctx_id, cs.size, ret);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    } else {
        fprintf(stderr, "[VIRGL-SUBMIT-OK] ctx_id=%u size=%u\n", cs.hdr.ctx_id, cs.size);
    }

out:
    g_free(buf);
}

static void virgl_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_to_host_2d t2d;
    struct virtio_gpu_box box;

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    struct virtio_gpu_virgl_resource *res = virtio_gpu_virgl_find_resource(g, t2d.resource_id);
    if (res && res->base.remapped && res->base.iov && res->base.iov_cnt > 0) {
        iov_to_buf(res->base.iov, res->base.iov_cnt, t2d.offset,
                   res->base.remapped + t2d.offset,
                   res->base.blob_size > t2d.offset ? res->base.blob_size - t2d.offset : 0);

        size_t pixel_count = res->base.blob_size / 4;
        uint32_t *pixels = (uint32_t *)res->base.remapped;
        size_t non_zero = 0;
        uint32_t first_pixel = 0;
        for (size_t i = 0; i < pixel_count; i++) {
            if (pixels[i] != 0) {
                non_zero++;
                if (first_pixel == 0) first_pixel = pixels[i];
            }
        }
        if (non_zero > 0) {
            fprintf(stderr, "[QEMU-2D-TRANSFER] res_id=%u: non_zero=%zu/%zu (first=0x%08X)\n",
                    t2d.resource_id, non_zero, pixel_count, first_pixel);
            FILE *af = fopen("/tmp/aperture_2d_audit.txt", "w");
            if (af) {
                fprintf(af, "res_id=%u non_zero=%zu total=%zu first=0x%08X\n",
                        t2d.resource_id, non_zero, pixel_count, first_pixel);
                fclose(af);
            }
            FILE *wf = fopen("/Users/skanda/Documents/Coding Projects/Aperture/virglrender/build/2d_audit.txt", "w");
            if (wf) {
                fprintf(wf, "res_id=%u non_zero=%zu total=%zu first=0x%08X\n",
                        t2d.resource_id, non_zero, pixel_count, first_pixel);
                fclose(wf);
            }
            FILE *diff_f = fopen("/Users/skanda/Documents/Coding Projects/Aperture/Desktop/Research/ZeroCopy/TRACK_B_DIFFERENTIAL_AUDIT.txt", "a");
            if (diff_f) {
                fprintf(diff_f, "TIMESTAMP=%ld | GUEST_2D_XFER(res=%u): non_zero=%zu/%zu (first=0x%08X)\n",
                        time(NULL), t2d.resource_id, non_zero, pixel_count, first_pixel);
                fclose(diff_f);
            }
        }
    }

    box.x = t2d.r.x;
    box.y = t2d.r.y;
    box.z = 0;
    box.w = t2d.r.width;
    box.h = t2d.r.height;
    box.d = 1;

    virgl_renderer_transfer_write_iov(t2d.resource_id,
                                      0,
                                      0,
                                      0,
                                      0,
                                      (struct virgl_box *)&box,
                                      t2d.offset, NULL, 0);
}

static void virgl_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d t3d;

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    virgl_renderer_transfer_write_iov(t3d.resource_id,
                                      t3d.hdr.ctx_id,
                                      t3d.level,
                                      t3d.stride,
                                      t3d.layer_stride,
                                      (struct virgl_box *)&t3d.box,
                                      t3d.offset, NULL, 0);
}

static void
virgl_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d tf3d;

    VIRTIO_GPU_FILL_CMD(tf3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(tf3d.resource_id);

    virgl_renderer_transfer_read_iov(tf3d.resource_id,
                                     tf3d.hdr.ctx_id,
                                     tf3d.level,
                                     tf3d.stride,
                                     tf3d.layer_stride,
                                     (struct virgl_box *)&tf3d.box,
                                     tf3d.offset, NULL, 0);
}


static void virgl_resource_attach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_attach_backing att_rb;
    struct iovec *res_iovs;
    uint32_t res_niov;
    int ret;

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    ret = virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb),
                                        cmd, NULL, &res_iovs, &res_niov);
    if (ret < 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    ret = virgl_renderer_resource_attach_iov(att_rb.resource_id,
                                             res_iovs, res_niov);

    if (ret != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, res_niov);
        return;
    }

    struct virtio_gpu_virgl_resource *res = virtio_gpu_virgl_find_resource(g, att_rb.resource_id);
    if (res) {
        res->base.iov = res_iovs;
        res->base.iov_cnt = res_niov;
    }
    fprintf(stderr, "[VIRGL-ATTACH-BACKING] res_id=%u niov=%u\n", att_rb.resource_id, res_niov);
}

static void virgl_resource_detach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_detach_backing detach_rb;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

    struct virtio_gpu_virgl_resource *res = virtio_gpu_virgl_find_resource(g, detach_rb.resource_id);
    if (res) {
        res->base.iov = NULL;
        res->base.iov_cnt = 0;
    }

    virgl_renderer_resource_detach_iov(detach_rb.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs == NULL || num_iovs == 0) {
        return;
    }
    virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
}


static void virgl_cmd_ctx_attach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource att_res;

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    virgl_renderer_ctx_attach_resource(att_res.hdr.ctx_id, att_res.resource_id);
}

static void virgl_cmd_ctx_detach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource det_res;

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    virgl_renderer_ctx_detach_resource(det_res.hdr.ctx_id, det_res.resource_id);
}

static void virgl_cmd_get_capset_info(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;

    VIRTIO_GPU_FILL_CMD(info);

    memset(&resp, 0, sizeof(resp));

    if (info.capset_index < g->capset_ids->len) {
        resp.capset_id = g_array_index(g->capset_ids, uint32_t,
                                       info.capset_index);
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    }
    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_get_capset(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t max_ver, max_size;
    VIRTIO_GPU_FILL_CMD(gc);

    virgl_renderer_get_cap_set(gc.capset_id, &max_ver,
                               &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(gc.capset_id,
                             gc.capset_version,
                             (void *)resp->capset_data);
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_cmd_resource_create_blob(VirtIOGPU *g,
                                           struct virtio_gpu_ctrl_command *cmd)
{
    struct virgl_renderer_resource_create_blob_args virgl_args = { 0 };
    g_autofree struct virtio_gpu_virgl_resource *res = NULL;
    struct virtio_gpu_resource_create_blob cblob;
    struct virgl_renderer_resource_info info;
    int ret;

    if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);
    fprintf(stderr, "[QEMU-BLOB-CREATE] res=%u blob_mem=0x%x blob_flags=0x%x size=%" PRIu64 " blob_id=0x%" PRIx64 "\n",
            cblob.resource_id, cblob.blob_mem, cblob.blob_flags, (uint64_t)cblob.size, (uint64_t)cblob.blob_id);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    if (cblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (virtio_gpu_virgl_find_resource(g, cblob.resource_id)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.resource_id = cblob.resource_id;
    res->base.blob_size = cblob.size;
    res->base.dmabuf_fd = -1;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                            cmd, &res->base.addrs,
                                            &res->base.iov, &res->base.iov_cnt);
        if (ret < 0) {
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
    }

    virgl_args.res_handle = cblob.resource_id;
    virgl_args.ctx_id = cblob.hdr.ctx_id;
    virgl_args.blob_mem = cblob.blob_mem;
    virgl_args.blob_id = cblob.blob_id;
    virgl_args.blob_flags = cblob.blob_flags;
    virgl_args.size = cblob.size;
    virgl_args.iovecs = res->base.iov;
    virgl_args.num_iovs = res->base.iov_cnt;

    ret = virgl_renderer_resource_create_blob(&virgl_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        virtio_gpu_cleanup_mapping(g, &res->base);
        return;
    }

    ret = virgl_renderer_resource_get_info(cblob.resource_id, &info);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: resource does not have info %d: %s\n",
                      __func__, cblob.resource_id, strerror(-ret));
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        virtio_gpu_cleanup_mapping(g, &res->base);
        virgl_renderer_resource_unref(cblob.resource_id);
        return;
    }

    res->base.dmabuf_fd = info.fd;

#if defined(CONFIG_DARWIN) || defined(__APPLE__)
    /* For zero-copy on macOS, the fd returned by virglrenderer is actually the IOSurface ID */
    if (info.fd > 0) {
        virtio_gpu_hostmem_notify_created(cblob.resource_id, (uint32_t)info.fd);
    } else if (res->base.blob_size > 0) {
        virtio_gpu_hostmem_create_resource(g, &res->base);
    }
#endif

    /* Now live, cleaned up in virtio_gpu_virgl_resource_unref */
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);
    g_steal_pointer(&res);
}

static void virgl_cmd_resource_map_blob(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_map_blob mblob;
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_resp_map_info resp;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    int ret;

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    fprintf(stderr, "[VIRGL-MAP-BLOB-CMD] res_id=%u offset=0x%llx\n",
            mblob.resource_id, (unsigned long long)mblob.offset);

    res = virtio_gpu_virgl_find_resource(g, mblob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (mblob.offset + res->base.blob_size > b->conf.hostmem ||
        mblob.offset + res->base.blob_size < mblob.offset) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to map virgl resource: invalid offset\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    ret = virtio_gpu_virgl_map_resource_blob(g, res, mblob.offset);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    virgl_renderer_resource_get_map_info(mblob.resource_id, &resp.map_info);
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_resource_unmap_blob(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd,
                                          bool *cmd_suspended)
{
    struct virtio_gpu_resource_unmap_blob ublob;
    struct virtio_gpu_virgl_resource *res;
    int ret;

    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    res = virtio_gpu_virgl_find_resource(g, ublob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = virtio_gpu_virgl_unmap_resource_blob(g, res, cmd_suspended);
    if (ret) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
}

static void virgl_cmd_set_scanout_blob(VirtIOGPU *g,
                                       struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_set_scanout_blob ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_scanout_blob_bswap(&ss);
    trace_virtio_gpu_cmd_set_scanout_blob(ss.scanout_id, ss.resource_id,
                                          ss.r.width, ss.r.height, ss.r.x,
                                          ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    if (ss.width < 16 ||
        ss.height < 16 ||
        ss.r.x + ss.r.width > ss.width ||
        ss.r.y + ss.r.height > ss.height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout %d bounds for"
                      " resource %d, rect (%d,%d)+%d,%d, fb %d %d\n",
                      __func__, ss.scanout_id, ss.resource_id,
                      ss.r.x, ss.r.y, ss.r.width, ss.r.height,
                      ss.width, ss.height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

#ifndef __APPLE__
    if (res->base.dmabuf_fd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource not backed by dmabuf %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    if (!virtio_gpu_scanout_blob_to_fb(&fb, &ss, res->base.blob_size)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    g->parent_obj.enable = 1;
    if (virtio_gpu_update_dmabuf(g, ss.scanout_id, &res->base, &fb, &ss.r)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to update dmabuf\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
#else
    uint32_t iosurf_id = virtio_gpu_hostmem_lookup_iosurface_id(ss.resource_id);
    if (iosurf_id == 0) {
        iosurf_id = virtio_gpu_hostmem_get_iosurface_id(&res->base);
    }
    if (iosurf_id == 0) {
        iosurf_id = virtio_gpu_hostmem_create_scanout_iosurface(g, ss.resource_id, ss.r.width, ss.r.height);
    }

    fprintf(stderr, "[VIRGL-SET-SCANOUT-BLOB-ACCEPTED] scanout_id=%u res_id=%u iosurf_id=%u rect=%ux%u+%u+%u\n",
            ss.scanout_id, ss.resource_id, iosurf_id, ss.r.width, ss.r.height, ss.r.x, ss.r.y);
    g->parent_obj.enable = 1;
    g->parent_obj.scanout[ss.scanout_id].resource_id = ss.resource_id;
    g->parent_obj.scanout[ss.scanout_id].width = ss.r.width;
    g->parent_obj.scanout[ss.scanout_id].height = ss.r.height;
    g->parent_obj.scanout[ss.scanout_id].x = ss.r.x;
    g->parent_obj.scanout[ss.scanout_id].y = ss.r.y;

    if (iosurf_id > 0) {
        virtio_gpu_hostmem_sync_scanout(ss.resource_id, &res->base);
        virtio_gpu_hostmem_notify_scanout_flush(ss.scanout_id, iosurf_id, ss.r.width, ss.r.height);
    }
#endif
}
#endif

void virtio_gpu_virgl_process_cmd(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    bool cmd_suspended = false;
    int ret;

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);

    virgl_renderer_force_ctx_0();
    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_CTX_CREATE:
        virgl_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        virgl_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        virgl_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        virgl_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        virgl_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        virgl_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        virgl_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        virgl_resource_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        virgl_resource_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        virgl_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        virgl_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        virgl_cmd_resource_unref(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        virgl_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        virgl_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
#if VIRGL_VERSION_MAJOR >= 1
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        virgl_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        virgl_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        virgl_cmd_resource_unmap_blob(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        virgl_cmd_set_scanout_blob(g, cmd);
        break;
#endif
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (cmd_suspended || cmd->finished) {
        return;
    }
    if (cmd->error) {
        fprintf(stderr, "%s: ctrl 0x%x, error 0x%x\n", __func__,
                cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);
#if VIRGL_VERSION_MAJOR >= 1
    if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
        const uint32_t flags = VIRGL_RENDERER_FENCE_FLAG_MERGEABLE;

        ret = virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id, flags,
                                                  cmd->cmd_hdr.ring_idx,
                                                  cmd->cmd_hdr.fence_id);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: virgl_renderer_context_create_fence error: %s",
                          __func__, strerror(-ret));
        }
        return;
    }
#endif

    /*
     * Unlike other virglrenderer functions, this one returns a positive
     * error code.
     */
    ret = virgl_renderer_create_fence(cmd->cmd_hdr.fence_id, 0);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: virgl_renderer_create_fence error: %s",
                      __func__, strerror(ret));
    }
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        /*
         * the guest can end up emitting fences out of order
         * so we should check all fenced cmds not just the first one.
         */
#if VIRGL_VERSION_MAJOR >= 1
        if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
            continue;
        }
#endif
        if (cmd->cmd_hdr.fence_id > fence) {
            continue;
        }
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            trace_virtio_gpu_dec_inflight_fences(g->inflight);
        }
    }
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t ring_idx, uint64_t fence_id) {
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX &&
            cmd->cmd_hdr.ctx_id == ctx_id && cmd->cmd_hdr.ring_idx == ring_idx &&
            cmd->cmd_hdr.fence_id <= fence_id) {
            trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                trace_virtio_gpu_dec_inflight_fences(g->inflight);
            }
        }
    }
}
#endif

void virtio_gpu_virgl_reset_async_fences(VirtIOGPU *g)
{
    struct virtio_gpu_virgl_context_fence *f;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    while (!QSLIST_EMPTY(&gl->async_fenceq)) {
        f = QSLIST_FIRST(&gl->async_fenceq);

        QSLIST_REMOVE_HEAD(&gl->async_fenceq, next);

        g_free(f);
    }
}

#if VIRGL_CHECK_VERSION(1, 1, 2)
static void virtio_gpu_virgl_async_fence_bh(void *opaque)
{
    QSLIST_HEAD(, virtio_gpu_virgl_context_fence) async_fenceq;
    struct virtio_gpu_ctrl_command *cmd, *tmp;
    struct virtio_gpu_virgl_context_fence *f;
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (gl->renderer_state != RS_INITED) {
        return;
    }

    QSLIST_MOVE_ATOMIC(&async_fenceq, &gl->async_fenceq);

    while (!QSLIST_EMPTY(&async_fenceq)) {
        f = QSLIST_FIRST(&async_fenceq);

        QSLIST_REMOVE_HEAD(&async_fenceq, next);

        QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
            /*
             * the guest can end up emitting fences out of order
             * so we should check all fenced cmds not just the first one.
             */
            if (cmd->cmd_hdr.fence_id > f->fence_id) {
                continue;
            }
            if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) {
                if (cmd->cmd_hdr.ring_idx != f->ring_idx) {
                    continue;
                }
                if (cmd->cmd_hdr.ctx_id != f->ctx_id) {
                    continue;
                }
            }
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
        }

        trace_virtio_gpu_fence_resp(f->fence_id);
        g_free(f);
        g->inflight--;
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            trace_virtio_gpu_dec_inflight_fences(g->inflight);
        }
    }
}

static void
virtio_gpu_virgl_push_async_fence(VirtIOGPU *g, uint32_t ctx_id,
                                  uint32_t ring_idx, uint64_t fence_id)
{
    struct virtio_gpu_virgl_context_fence *f;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    f = g_new(struct virtio_gpu_virgl_context_fence, 1);
    f->ctx_id = ctx_id;
    f->ring_idx = ring_idx;
    f->fence_id = fence_id;

    QSLIST_INSERT_HEAD_ATOMIC(&gl->async_fenceq, f, next);

    qemu_bh_schedule(gl->async_fence_bh);
}

static void virgl_write_async_fence(void *opaque, uint32_t fence)
{
    VirtIOGPU *g = opaque;

    virtio_gpu_virgl_push_async_fence(g, 0, UINT32_MAX, fence);
}

static void virgl_write_async_context_fence(void *opaque, uint32_t ctx_id,
                                            uint32_t ring_idx, uint64_t fence)
{
    VirtIOGPU *g = opaque;

    virtio_gpu_virgl_push_async_fence(g, ctx_id, ring_idx, fence);
}
#endif

static virgl_renderer_gl_context
virgl_create_context(void *opaque, int scanout_idx,
                     struct virgl_renderer_gl_ctx_param *params)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext ctx;
    QEMUGLParams qparams;

    qparams.major_ver = params->major_ver;
    qparams.minor_ver = params->minor_ver;

    ctx = qemu_console_gl_ctx_create(g->parent_obj.scanout[scanout_idx].con, &qparams);
    return (virgl_renderer_gl_context)ctx;
}

static void virgl_destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    qemu_console_gl_ctx_destroy(g->parent_obj.scanout[0].con, qctx);
}

static int virgl_make_context_current(void *opaque, int scanout_idx,
                                      virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    return qemu_console_gl_ctx_make_current(g->parent_obj.scanout[scanout_idx].con, qctx);
}

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs = {
#if VIRGL_VERSION_MAJOR >= 1
    .version             = 3,
#else
    .version             = 1,
#endif
    .write_fence         = virgl_write_fence,
    .create_gl_context   = virgl_create_context,
    .destroy_gl_context  = virgl_destroy_context,
    .make_current        = virgl_make_context_current,
#if VIRGL_VERSION_MAJOR >= 1
    .write_context_fence = virgl_write_context_fence,
#endif
};

static void virtio_gpu_print_stats(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (g->stats.requests) {
        fprintf(stderr, "stats: vq req %4d, %3d -- 3D %4d (%5d)\n",
                g->stats.requests,
                g->stats.max_inflight,
                g->stats.req_3d,
                g->stats.bytes_3d);
        g->stats.requests     = 0;
        g->stats.max_inflight = 0;
        g->stats.req_3d       = 0;
        g->stats.bytes_3d     = 0;
    } else {
        fprintf(stderr, "stats: idle\r");
    }
    timer_mod(gl->print_stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void virtio_gpu_fence_poll(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (!virtio_gpu_virgl_update_render_state(g)) {
        return;
    }

    virgl_renderer_poll();
    virtio_gpu_process_cmdq(g);
    if (!QTAILQ_EMPTY(&g->cmdq) || !QTAILQ_EMPTY(&g->fenceq)) {
        timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 10);
    }
}

void virtio_gpu_virgl_fence_poll(VirtIOGPU *g)
{
    virtio_gpu_fence_poll(g);
}

void virtio_gpu_virgl_reset_scanout(VirtIOGPU *g)
{
    int i;

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        qemu_console_set_surface(g->parent_obj.scanout[i].con, NULL);
        qemu_console_gl_scanout_disable(g->parent_obj.scanout[i].con);
    }
}

static bool virtio_gpu_virgl_reset(VirtIOGPU *g)
{
    struct virtio_gpu_simple_resource *res, *tmp;

    /*
     * Virgl blob resource unmapping can be suspended and
     * deferred on unref, ensure that destruction is completed.
     */
    QTAILQ_FOREACH_SAFE(res, &g->reslist, next, tmp) {
        virtio_gpu_virgl_resource_destroy(g, res, NULL);
    }

    if (!QTAILQ_EMPTY(&g->reslist)) {
        return false;
    }

    virgl_renderer_reset();

    virtio_gpu_virgl_reset_async_fences(g);

    return true;
}

static int virtio_gpu_virgl_init(VirtIOGPU *g)
{
    int ret;
    uint32_t flags = 0;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
    if (qemu_egl_display) {
        virtio_gpu_3d_cbs.version = 4;
        virtio_gpu_3d_cbs.get_egl_display = virgl_get_egl_display;
#if VIRGL_CHECK_VERSION(1, 1, 2)
        virtio_gpu_3d_cbs.write_fence         = virgl_write_async_fence;
        virtio_gpu_3d_cbs.write_context_fence = virgl_write_async_context_fence;
        flags |= VIRGL_RENDERER_ASYNC_FENCE_CB;
        flags |= VIRGL_RENDERER_THREAD_SYNC;
#endif
    }
#endif
#ifdef VIRGL_RENDERER_D3D11_SHARE_TEXTURE
    if (qemu_egl_angle_d3d) {
        flags |= VIRGL_RENDERER_D3D11_SHARE_TEXTURE;
    }
#endif
#if VIRGL_VERSION_MAJOR >= 1
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        flags |= VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER;
#ifdef __APPLE__
        flags |= VIRGL_RENDERER_NO_VIRGL;
#endif
    }
    if (virtio_gpu_drm_enabled(g->parent_obj.conf)) {
        flags |= VIRGL_RENDERER_DRM;

        if (!(flags & VIRGL_RENDERER_ASYNC_FENCE_CB)) {
            /*
             * Virglrenderer skips enabling DRM context support without
             * enabled async-fence feature. VirtIO-GPU will initialize
             * successfully, but DRM context won't be available in guest.
             *
             * For vrend async-fencing can be enabled only if EGL display
             * is used. Vrend can't be disabled in QEMU, hence DRM implicitly
             * requires EGL too.
             *
             * Async-fence was bugged in virglrenderer versions <= 1.1.1.
             */
            error_report("drm requires egl display and virglrenderer >= 1.2.0");
            return -EINVAL;
        }
    }
#endif

    ret = virgl_renderer_init(g, flags, &virtio_gpu_3d_cbs);
    if (ret != 0) {
        error_report("virgl could not be initialized: %d", ret);
        return ret;
    }

    gl->fence_poll = timer_new_ms(QEMU_CLOCK_REALTIME,
                                  virtio_gpu_fence_poll, g);

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        gl->print_stats = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                       virtio_gpu_print_stats, g);
        timer_mod(gl->print_stats,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }

#if VIRGL_VERSION_MAJOR >= 1
    gl->cmdq_resume_bh = virtio_bh_io_new_guarded(DEVICE(g),
                                                  virtio_gpu_virgl_resume_cmdq_bh,
                                                  g);
#if VIRGL_CHECK_VERSION(1, 1, 2)
    gl->async_fence_bh = virtio_bh_io_new_guarded(DEVICE(g),
                                                  virtio_gpu_virgl_async_fence_bh,
                                                  g);
#endif
#endif

#ifdef __APPLE__
    /* Initialize the gpu.sock BH and reconnect timer on the I/O thread.
     * This MUST be called here (I/O thread context) so that qemu_bh_new,
     * timer_new_ms, and qemu_set_fd_handler are all called from the correct thread. */
    fprintf(stderr, "[VIRGL-INIT] Calling virtio_gpu_hostmem_init_iothread (I/O thread)\n");
    fflush(stderr);
    virtio_gpu_hostmem_init_iothread(g);
    fprintf(stderr, "[VIRGL-INIT] virtio_gpu_hostmem_init_iothread returned\n");
    fflush(stderr);
#endif

    return 0;
}

bool virtio_gpu_virgl_update_render_state(VirtIOGPU *g)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    switch (gl->renderer_state) {
    case RS_RESET:
        virgl_renderer_force_ctx_0();

        if (!virtio_gpu_virgl_reset(g)) {
            return false;
        }
        /* fallthrough */
    case RS_START:
        if (virtio_gpu_virgl_init(g)) {
            gl->renderer_state = RS_INIT_FAILED;
            return false;
        }

        gl->renderer_state = RS_INITED;
        break;
    case RS_INIT_FAILED:
        return false;
    case RS_INITED:
        break;
    }

    return true;
}

static void virtio_gpu_virgl_add_capset(GArray *capset_ids, uint32_t capset_id)
{
    g_array_append_val(capset_ids, capset_id);
}

GArray *virtio_gpu_virgl_get_capsets(VirtIOGPU *g)
{
    uint32_t capset_max_ver, capset_max_size;
    GArray *capset_ids;

    capset_ids = g_array_new(false, false, sizeof(uint32_t));

    /* VIRGL is always supported. */
    virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL);

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2,
                               &capset_max_ver,
                               &capset_max_size);
    if (capset_max_ver) {
        virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL2);
    }

    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS,
                                   &capset_max_ver,
                                   &capset_max_size);
        if (capset_max_size) {
            virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VENUS);
        }
    }

    if (virtio_gpu_venus_enabled(g->parent_obj.conf) ||
        virtio_gpu_drm_enabled(g->parent_obj.conf)) {
        virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_DRM,
                                   &capset_max_ver,
                                   &capset_max_size);
        if (capset_max_size) {
            virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_DRM);
        }
    }

    return capset_ids;
}
