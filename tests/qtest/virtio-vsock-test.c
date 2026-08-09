/*
 * QTest testcase for Native Userspace VirtIO VSOCK PCI Device
 *
 * Copyright 2026 Aperture
 * Licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static void test_virtio_vsock_pci_init(void)
{
    QTestState *qts;

    qts = qtest_init("-machine virt -chardev socket,path=/tmp/vsock_test1.sock,server=on,wait=off,id=charvsock0 "
                     "-device virtio-vsock-pci,id=vsock_dev,chardev=charvsock0,guest-cid=3");

    /* Verify device presence & guest-cid property via QMP qom-get */
    QDict *resp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': {'path': '/machine/peripheral/vsock_dev', 'property': 'guest-cid'}}");
    g_assert(qdict_haskey(resp, "return"));
    uint64_t cid = qdict_get_int(resp, "return");
    g_assert_cmpint(cid, ==, 3);
    qobject_unref(resp);

    qtest_quit(qts);
}

static void test_virtio_vsock_pci_multiple_devices(void)
{
    QTestState *qts;

    qts = qtest_init("-machine virt "
                     "-chardev socket,path=/tmp/vsock_m1.sock,server=on,wait=off,id=charvsock1 "
                     "-device virtio-vsock-pci,id=vsock_dev1,chardev=charvsock1,guest-cid=10 "
                     "-chardev socket,path=/tmp/vsock_m2.sock,server=on,wait=off,id=charvsock2 "
                     "-device virtio-vsock-pci,id=vsock_dev2,chardev=charvsock2,guest-cid=20");

    /* Verify first device CID */
    QDict *resp1 = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': {'path': '/machine/peripheral/vsock_dev1', 'property': 'guest-cid'}}");
    g_assert(qdict_haskey(resp1, "return"));
    g_assert_cmpint(qdict_get_int(resp1, "return"), ==, 10);
    qobject_unref(resp1);

    /* Verify second device CID */
    QDict *resp2 = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': {'path': '/machine/peripheral/vsock_dev2', 'property': 'guest-cid'}}");
    g_assert(qdict_haskey(resp2, "return"));
    g_assert_cmpint(qdict_get_int(resp2, "return"), ==, 20);
    qobject_unref(resp2);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/virtio-vsock-pci/init", test_virtio_vsock_pci_init);
    qtest_add_func("/virtio-vsock-pci/multiple-devices", test_virtio_vsock_pci_multiple_devices);

    return g_test_run();
}
