/*
  Virtio VSOCK Socket Device (Userspace chardev bridge)
 
  Copyright 2026 Aperture
  Licensed under GNU GPL v2 or later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char-fe.h"
#include "standard-headers/linux/virtio_vsock.h"

#define TYPE_VIRTIO_VSOCK_SOCKET "virtio-vsock-socket-device"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOVSockSocket, VIRTIO_VSOCK_SOCKET)

struct VirtIOVSockSocket {
    VirtIODevice parent_obj;
    CharFrontend chr;
    uint64_t guest_cid;
    VirtQueue *rx_vq;
    VirtQueue *tx_vq;
    VirtQueue *event_vq;
    
    uint32_t peer_port;
    uint64_t peer_cid;
    uint32_t host_port;
    bool connected;
    uint32_t rx_fwd_cnt;

    uint8_t rx_buf[65536];
    size_t rx_buf_len;
    size_t rx_buf_offset;
};

static void send_rx_pkt(VirtIOVSockSocket *vsock, uint16_t op, const uint8_t *buf, size_t len)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vsock);
    VirtQueueElement *elem;

    if (!virtio_queue_ready(vsock->rx_vq)) {
        fprintf(stderr, "[VSOCK-DEVICE] send_rx_pkt DROP: rx_vq not ready!\n");
        return;
    }

    elem = virtqueue_pop(vsock->rx_vq, sizeof(VirtQueueElement));
    if (!elem) {
        fprintf(stderr, "[VSOCK-DEVICE] send_rx_pkt DROP: rx_vq empty!\n");
        return;
    }

    struct virtio_vsock_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.src_cid = cpu_to_le64(2); // Host CID
    hdr.dst_cid = cpu_to_le64(vsock->peer_cid ? vsock->peer_cid : vsock->guest_cid);
    hdr.src_port = cpu_to_le32(vsock->host_port ? vsock->host_port : 5000);
    hdr.dst_port = cpu_to_le32(vsock->peer_port);
    hdr.len = cpu_to_le32((uint32_t)len);
    hdr.type = cpu_to_le16(VIRTIO_VSOCK_TYPE_STREAM);
    hdr.op = cpu_to_le16(op);
    hdr.flags = 0;
    hdr.buf_alloc = cpu_to_le32(262144);
    hdr.fwd_cnt = cpu_to_le32(vsock->rx_fwd_cnt);

    size_t offset = iov_from_buf(elem->in_sg, elem->in_num, 0, &hdr, sizeof(hdr));
    if (len > 0 && buf) {
        offset += iov_from_buf(elem->in_sg, elem->in_num, sizeof(hdr), buf, len);
    }

    fprintf(stderr, "[VSOCK-DEVICE] send_rx_pkt: op=%u len=%zu to peer_port=%u\n", op, len, vsock->peer_port);
    virtqueue_push(vsock->rx_vq, elem, offset);
    g_free(elem);
    virtio_notify(vdev, vsock->rx_vq);
}

static void flush_rx(VirtIOVSockSocket *vsock)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vsock);

    fprintf(stderr, "[VSOCK-DEVICE] flush_rx ENTER: connected=%d ready=%d offset=%zu len=%zu\n",
            vsock->connected, virtio_queue_ready(vsock->rx_vq),
            vsock->rx_buf_offset, vsock->rx_buf_len);

    if (!vsock->connected || !virtio_queue_ready(vsock->rx_vq)) {
        return;
    }

    while (vsock->rx_buf_offset < vsock->rx_buf_len) {
        VirtQueueElement *elem = virtqueue_pop(vsock->rx_vq, sizeof(VirtQueueElement));
        if (!elem) {
            fprintf(stderr, "[VSOCK-DEVICE] flush_rx: virtqueue_pop returned NULL! (offset=%zu/%zu)\n",
                    vsock->rx_buf_offset, vsock->rx_buf_len);
            break;
        }

        size_t total_in = iov_size(elem->in_sg, elem->in_num);
        if (total_in <= sizeof(struct virtio_vsock_hdr)) {
            fprintf(stderr, "[VSOCK-DEVICE] flush_rx: total_in=%zu <= hdr size, dropping elem!\n", total_in);
            virtqueue_push(vsock->rx_vq, elem, 0);
            g_free(elem);
            break;
        }

        size_t avail = vsock->rx_buf_len - vsock->rx_buf_offset;
        size_t max_payload = total_in - sizeof(struct virtio_vsock_hdr);
        size_t chunk_len = avail < max_payload ? avail : max_payload;

        struct virtio_vsock_hdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.src_cid = cpu_to_le64(2); // Host CID
        hdr.dst_cid = cpu_to_le64(vsock->peer_cid ? vsock->peer_cid : vsock->guest_cid);
        hdr.src_port = cpu_to_le32(vsock->host_port ? vsock->host_port : 5000);
        hdr.dst_port = cpu_to_le32(vsock->peer_port);
        hdr.len = cpu_to_le32((uint32_t)chunk_len);
        hdr.type = cpu_to_le16(VIRTIO_VSOCK_TYPE_STREAM);
        hdr.op = cpu_to_le16(VIRTIO_VSOCK_OP_RW);
        hdr.flags = 0;
        hdr.buf_alloc = cpu_to_le32(262144);
        hdr.fwd_cnt = cpu_to_le32(vsock->rx_fwd_cnt);

        size_t offset = iov_from_buf(elem->in_sg, elem->in_num, 0, &hdr, sizeof(hdr));
        offset += iov_from_buf(elem->in_sg, elem->in_num, sizeof(hdr),
                               vsock->rx_buf + vsock->rx_buf_offset, chunk_len);

        vsock->rx_buf_offset += chunk_len;
        fprintf(stderr, "[VSOCK-DEVICE] flush_rx: pushed %zu bytes (op=RW) to guest peer_port=%u (offset=%zu/%zu)\n",
                chunk_len, vsock->peer_port, vsock->rx_buf_offset, vsock->rx_buf_len);
        virtqueue_push(vsock->rx_vq, elem, offset);
        g_free(elem);
        virtio_notify(vdev, vsock->rx_vq);
    }

    if (vsock->rx_buf_offset >= vsock->rx_buf_len) {
        vsock->rx_buf_len = 0;
        vsock->rx_buf_offset = 0;
        qemu_chr_fe_accept_input(&vsock->chr);
    } else if (vsock->rx_buf_offset > 0) {
        memmove(vsock->rx_buf, vsock->rx_buf + vsock->rx_buf_offset,
                vsock->rx_buf_len - vsock->rx_buf_offset);
        vsock->rx_buf_len -= vsock->rx_buf_offset;
        vsock->rx_buf_offset = 0;
    }
}

static void handle_tx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVSockSocket *vsock = VIRTIO_VSOCK_SOCKET(vdev);
    VirtQueueElement *elem;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        struct virtio_vsock_hdr hdr;
        size_t hdr_len;

        hdr_len = iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr));
        if (hdr_len >= sizeof(hdr)) {
            uint16_t op = le16_to_cpu(hdr.op);
            uint32_t len = le32_to_cpu(hdr.len);
            uint64_t src_cid = le64_to_cpu(hdr.src_cid);
            uint32_t src_port = le32_to_cpu(hdr.src_port);
            uint32_t dst_port = le32_to_cpu(hdr.dst_port);

            if (op == VIRTIO_VSOCK_OP_REQUEST) {
                vsock->peer_cid = src_cid;
                vsock->peer_port = src_port;
                vsock->host_port = dst_port;
                vsock->connected = true;
                vsock->rx_fwd_cnt = 0;

                fprintf(stderr, "[VSOCK-DEVICE] Guest OP_REQUEST: src_port=%u dst_port=%u\n", src_port, dst_port);
                send_rx_pkt(vsock, VIRTIO_VSOCK_OP_RESPONSE, NULL, 0);
                flush_rx(vsock);
                qemu_chr_fe_accept_input(&vsock->chr);
            } else if (op == VIRTIO_VSOCK_OP_RW && len > 0) {
                uint8_t *buf = g_malloc(len);
                iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), buf, len);
                fprintf(stderr, "[VSOCK-DEVICE] Guest OP_RW: writing %u bytes to host socket\n", len);
                qemu_chr_fe_write_all(&vsock->chr, buf, len);
                g_free(buf);
                vsock->rx_fwd_cnt += len;
            } else if (op == VIRTIO_VSOCK_OP_CREDIT_REQUEST) {
                send_rx_pkt(vsock, VIRTIO_VSOCK_OP_CREDIT_UPDATE, NULL, 0);
            } else if (op == VIRTIO_VSOCK_OP_SHUTDOWN || op == VIRTIO_VSOCK_OP_RST) {
                fprintf(stderr, "[VSOCK-DEVICE] Guest SHUTDOWN/RST (op=%u)\n", op);
                vsock->connected = false;
                vsock->rx_fwd_cnt = 0;
                vsock->rx_buf_len = 0;
                vsock->rx_buf_offset = 0;
            }
        }

        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

static void handle_rx(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOVSockSocket *vsock = VIRTIO_VSOCK_SOCKET(vdev);
    fprintf(stderr, "[VSOCK-DEVICE] handle_rx CALLED: connected=%d ready=%d len=%zu\n",
            vsock->connected, virtio_queue_ready(vsock->rx_vq), vsock->rx_buf_len);
    if (vsock->connected) {
        flush_rx(vsock);
        if (vsock->rx_buf_len == 0) {
            qemu_chr_fe_accept_input(&vsock->chr);
        }
    }
}

static void handle_event(VirtIODevice *vdev, VirtQueue *vq)
{
}

static int chr_can_read(void *opaque)
{
    VirtIOVSockSocket *vsock = opaque;
    if (vsock->rx_buf_len >= sizeof(vsock->rx_buf)) {
        return 0;
    }
    return sizeof(vsock->rx_buf) - vsock->rx_buf_len;
}

static void chr_read(void *opaque, const uint8_t *buf, int size)
{
    VirtIOVSockSocket *vsock = opaque;
    if (size <= 0) {
        return;
    }

    fprintf(stderr, "[VSOCK-DEVICE] chr_read: received %d bytes from host (current buf_len=%zu, connected=%d)\n",
            size, vsock->rx_buf_len, vsock->connected);

    if (vsock->rx_buf_len + (size_t)size > sizeof(vsock->rx_buf)) {
        fprintf(stderr, "[VSOCK-DEVICE] chr_read OVERFLOW: dropping excess %d bytes\n", size);
        return;
    }

    memcpy(vsock->rx_buf + vsock->rx_buf_len, buf, (size_t)size);
    vsock->rx_buf_len += (size_t)size;
    flush_rx(vsock);
}

static void chr_event(void *opaque, QEMUChrEvent event)
{
    VirtIOVSockSocket *vsock = opaque;
    fprintf(stderr, "[VSOCK-DEVICE] chr_event: %d (connected=%d)\n", event, vsock->connected);
    switch (event) {
    case CHR_EVENT_OPENED:
        flush_rx(vsock);
        if (vsock->rx_buf_len == 0) {
            qemu_chr_fe_accept_input(&vsock->chr);
        }
        break;
    case CHR_EVENT_CLOSED:
        fprintf(stderr, "[VSOCK-DEVICE] Host closed socket\n");
        if (vsock->connected) {
            send_rx_pkt(vsock, VIRTIO_VSOCK_OP_RST, NULL, 0);
            vsock->connected = false;
        }
        vsock->rx_fwd_cnt = 0;
        vsock->rx_buf_len = 0;
        vsock->rx_buf_offset = 0;
        break;
    default:
        break;
    }
}

static void virtio_vsock_socket_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOVSockSocket *vsock = VIRTIO_VSOCK_SOCKET(vdev);
    struct virtio_vsock_config vsock_cfg;

    memset(&vsock_cfg, 0, sizeof(vsock_cfg));
    vsock_cfg.guest_cid = cpu_to_le64(vsock->guest_cid);
    memcpy(config, &vsock_cfg, sizeof(vsock_cfg));
}

static void virtio_vsock_socket_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSockSocket *vsock = VIRTIO_VSOCK_SOCKET(dev);

    virtio_init(vdev, VIRTIO_ID_VSOCK, sizeof(struct virtio_vsock_config));

    vsock->rx_vq = virtio_add_queue(vdev, 128, handle_rx);
    vsock->tx_vq = virtio_add_queue(vdev, 128, handle_tx);
    vsock->event_vq = virtio_add_queue(vdev, 128, handle_event);

    qemu_chr_fe_set_handlers(&vsock->chr, chr_can_read, chr_read, chr_event, NULL,
                             vsock, NULL, true);
}

static void virtio_vsock_socket_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOVSockSocket *vsock = VIRTIO_VSOCK_SOCKET(dev);

    qemu_chr_fe_deinit(&vsock->chr, false);
    virtio_delete_queue(vsock->rx_vq);
    virtio_delete_queue(vsock->tx_vq);
    virtio_delete_queue(vsock->event_vq);
    virtio_cleanup(vdev);
}

static const Property virtio_vsock_socket_properties[] = {
    DEFINE_PROP_CHR("chardev", VirtIOVSockSocket, chr),
    DEFINE_PROP_UINT64("guest-cid", VirtIOVSockSocket, guest_cid, 3),
};

static uint64_t virtio_vsock_socket_get_features(VirtIODevice *vdev, uint64_t features, Error **errp)
{
    return features;
}

static void virtio_vsock_socket_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_vsock_socket_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = virtio_vsock_socket_realize;
    vdc->unrealize = virtio_vsock_socket_unrealize;
    vdc->get_config = virtio_vsock_socket_get_config;
    vdc->get_features = virtio_vsock_socket_get_features;
}

static const TypeInfo virtio_vsock_socket_info = {
    .name = TYPE_VIRTIO_VSOCK_SOCKET,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOVSockSocket),
    .class_init = virtio_vsock_socket_class_init,
};

/* PCI Wrapper */
typedef struct VirtIOVSockSocketPCI VirtIOVSockSocketPCI;

#define TYPE_VIRTIO_VSOCK_PCI "virtio-vsock-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIOVSockSocketPCI, VIRTIO_VSOCK_SOCKET_PCI,
                         TYPE_VIRTIO_VSOCK_PCI)

struct VirtIOVSockSocketPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOVSockSocket vdev;
};

static const Property virtio_vsock_socket_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 3),
};

static void virtio_vsock_socket_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOVSockSocketPCI *dev = VIRTIO_VSOCK_SOCKET_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);

    virtio_pci_force_virtio_1(vpci_dev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_vsock_socket_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
    k->realize = virtio_vsock_socket_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_vsock_socket_pci_properties);
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_VSOCK;
    pcidev_k->revision = 0x00;
    pcidev_k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
}

static void virtio_vsock_socket_pci_instance_init(Object *obj)
{
    VirtIOVSockSocketPCI *dev = VIRTIO_VSOCK_SOCKET_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_VSOCK_SOCKET);
}

static const VirtioPCIDeviceTypeInfo virtio_vsock_socket_pci_info = {
    .base_name             = TYPE_VIRTIO_VSOCK_PCI,
    .generic_name          = "virtio-vsock-pci",
    .instance_size = sizeof(VirtIOVSockSocketPCI),
    .instance_init = virtio_vsock_socket_pci_instance_init,
    .class_init    = virtio_vsock_socket_pci_class_init,
};

static void virtio_vsock_socket_register_types(void)
{
    type_register_static(&virtio_vsock_socket_info);
    virtio_pci_types_register(&virtio_vsock_socket_pci_info);
}

type_init(virtio_vsock_socket_register_types)
