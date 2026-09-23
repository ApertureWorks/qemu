# Aperture QEMU Hypervisor

> **Aperture Fork Notice:**  
> This repository is a customized fork of upstream [`qemu-project/qemu`](https://gitlab.com/qemu-project/qemu.git) (originating from `ApertureWorks/qemu`). It contains custom hypervisor extensions for Apple Silicon macOS hosts to support zero-copy shared memory, paravirtualized graphics, and virtio transports for Aperture.

---

## Technical Overview

Aperture's QEMU fork powers the virtualization of LineageOS (ARM64) on macOS with Apple's `Hypervisor.framework` (HVF). It extends upstream QEMU with low-overhead graphics and IPC mechanisms:

1. **macOS IOSurface Host Memory Backend (`virtio-gpu-hostmem-macos.c`):**
   - Implements native Apple Silicon `IOSurfaceRef` memory allocation for `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`.
   - Maps host `IOSurface` base addresses directly into Stage-2 HVF GPA (`res->remapped`), enabling guest Vulkan/Venus drivers to render directly into host memory with **0 CPU copies and 0 GPU blits**.
2. **Dynamic Multi-Head Linux DRM KMS Scanouts:**
   - Configures virtual display heads (`card0-Virtual-1..16`). Default boot initializes heads `Virtual-1` and `Virtual-2` with dynamic hotplug support for secondary heads.
   - Forwards scanout page-flip flushes to the macOS host via packet `0x05` (`GPUScanoutFlushed`), delivering hardware-composited multi-window display presentation with 0 video encoding round-trips.
3. **GPU Control Server IPC (`gpu.sock`):**
   - Sends real-time resource lifecycle notifications to the host:
     - `0x01` (`GPUResourceCreated`): notifies host of new resource ID and associated `IOSurfaceID`.
     - `0x02` (`GPUResourceDestroyed`): triggers host Metal texture cache eviction.
     - `0x03` (`CMD_TRANSFER_RESOURCE`): accepts host commands to execute `iov_to_buf`.
     - `0x04` (`CMD_TRANSFER_DONE`): notifies host when memory synchronization finishes.
     - `0x05` (`GPUScanoutFlushed`): notifies host of hardware scanout updates.
4. **virtio-vsock & Serial Logging:**
   - Provides native `virtio-vsock-pci` transport for low-latency guest-host RPCs.
   - Redirects serial console output to `qemu-serial.log` to prevent terminal log spam and virtconsole stalls.

---

## Consumer & Developer Benefits

- **Performance:** Fluid 60 FPS multi-window Android UI rendering on macOS Apple Silicon without video encoder artifacts.
- **Battery Life:** Zero CPU encoding loops reduce battery draw and thermals during long sessions.
- **Maintainability:** Minimal, isolated patches against upstream QEMU's `hw/display/` and `backends/` directories.

---

## Building

### Prerequisites (macOS)
```bash
brew install meson ninja pkg-config glib pixman libslirp
```

### Build Commands
```bash
mkdir build && cd build
../configure --target-list=aarch64-softmmu --enable-hvf --enable-slirp --enable-virglrenderer
make -j$(sysctl -n hw.ncpu)
```

The resulting binary `qemu-system-aarch64` is packaged or symlinked for `QEMUHelper.xpc`.
