# Aperture QEMU Hypervisor

> **Upstream base:** tag `v11.1.3` (merged 2026-11-03). Full list of changes and how to merge upstream: [docs/forks/qemu.md](https://github.com/ApertureWorks/Aperture/blob/main/docs/forks/qemu.md).
>
> **Aperture Fork Notice:**  
> This repository is a customized fork of upstream [`qemu-project/qemu`](https://gitlab.com/qemu-project/qemu.git) (originating from `ApertureWorks/qemu`). It contains custom hypervisor extensions for Apple Silicon macOS hosts to support zero-copy shared memory, paravirtualized graphics, and virtio transports for Aperture.

---

## Technical Overview

Aperture's QEMU fork powers the virtualization of LineageOS (ARM64) on macOS with Apple's `Hypervisor.framework` (HVF). It extends upstream QEMU with low-overhead graphics and IPC mechanisms:

1. **macOS IOSurface Host Memory Backend (`virtio-gpu-hostmem-macos.c`):**
   - Implements native Apple Silicon `IOSurfaceRef` memory allocation for `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`.
   - Maps host `IOSurface` base addresses directly into Stage-2 HVF GPA (`res->remapped`), so guest Vulkan/Venus rendering lands in host memory without an extra CPU copy of the blob.
2. **Dynamic Multi-Head Linux DRM KMS Scanouts:**
   - Configures virtual display heads (`card0-Virtual-1..16`). Default boot initializes heads `Virtual-1` and `Virtual-2` with dynamic hotplug support for secondary heads.
   - Forwards scanout page-flip flushes to the macOS host via packet `0x05` (`GPUScanoutFlushed`), delivering hardware-composited multi-window display presentation with 0 video encoding round-trips.
3. **GPU Control Server IPC (`gpu.sock`):**
   - Unix socket to the Aperture host carrying resource lifecycle, blob registry, scanout flush and ring-fence gate packets. Layouts: [`docs/wire_protocol.md`](https://github.com/ApertureWorks/Aperture/blob/main/docs/wire_protocol.md).
4. **virtio-vsock:**
   - Provides native `virtio-vsock-pci` transport for low-latency guest-host RPCs.

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
