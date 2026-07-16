// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "unity.h"
#include <string.h>
#include <miniOS/fs/sysfs.h>
#include <miniOS/net/netdev.h>

int rtl8139_probe_and_init(void)   { return -1; }
int virtio_net_probe_and_init(void) { return -1; }

static uint8_t g_heap[8192];
static size_t g_heap_used;

void *kmalloc(size_t size)
{
    size = (size + 7U) & ~7U;
    if ((g_heap_used + size) > sizeof(g_heap))
        return NULL;

    void *ptr = &g_heap[g_heap_used];
    g_heap_used += size;
    return ptr;
}

void kfree(void *ptr) { (void)ptr; }

typedef int (*vfs_mount_fn_t)(const char *source, const char *target, const void *data);

static vfs_mount_fn_t g_mount_fn;

int register_filesystem(const char *name, vfs_mount_fn_t mount_fn)
{
    (void)name;
    g_mount_fn = mount_fn;
    return 0;
}

int vfs_register_mount(const char *target, vfs_ops_t *ops, uint32_t root_inode)
{
    (void)target;
    (void)ops;
    (void)root_inode;
    return 0;
}

#include "../../src/kernel/fs/sysfs.c"
#include "../../src/kernel/net/netdev.c"
#include "../../src/kernel/net/lwip_port.c"
#include "../../src/kernel/net/net_config.c"
#include "../../src/kernel/net/net_observe.c"

static int test_open(net_device_t *dev)
{
    (void)dev;
    return 0;
}

static int test_poll(net_device_t *dev)
{
    (void)dev;
    return 0;
}

static int g_tx_result;
static int g_tx_count;
static const void *g_last_tx_frame;
static size_t g_last_tx_len;
static bool g_link_up;
static uint8_t g_test_mac[6] = { 0x52, 0x54, 0x00, 0xab, 0xcd, 0xef };

static int test_xmit(net_device_t *dev, const void *frame, size_t frame_len)
{
    (void)dev;
    g_tx_count++;
    g_last_tx_frame = frame;
    g_last_tx_len = frame_len;
    return g_tx_result;
}

static int test_get_mac(net_device_t *dev, uint8_t mac_out[6])
{
    (void)dev;
    memcpy(mac_out, g_test_mac, sizeof(g_test_mac));
    return 0;
}

static bool test_get_link(net_device_t *dev)
{
    (void)dev;
    return g_link_up;
}

static netdev_ops_t g_test_ops = {
    .open = test_open,
    .poll = test_poll,
    .xmit = test_xmit,
    .get_mac = test_get_mac,
    .get_link = test_get_link,
};

static net_device_t g_test_dev = {
    .mtu = 1500,
    .ops = &g_test_ops,
};

static sysfs_node_t *find_child(sysfs_node_t *parent, const char *name)
{
    sysfs_node_t *child = parent ? parent->first_child : NULL;

    while (child) {
        if (child->name && strcmp(child->name, name) == 0)
            return child;
        child = child->next_sibling;
    }
    return NULL;
}

static int read_sysfs_file(sysfs_node_t *node, char *buf, size_t bufsiz)
{
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_UINT8(SYSFS_NODE_FILE, node->type);
    TEST_ASSERT_NOT_NULL(node->callback);
    return node->callback(buf, (uint32_t)bufsiz);
}

void setUp(void)
{
    memset(g_heap, 0, sizeof(g_heap));
    g_heap_used = 0;
    g_mount_fn = NULL;
    g_tx_result = 0;
    g_tx_count = 0;
    g_last_tx_frame = NULL;
    g_last_tx_len = 0;
    g_link_up = true;

    memset(&g_test_dev, 0, sizeof(g_test_dev));
    g_test_dev.mtu = 1500;
    g_test_dev.ops = &g_test_ops;

    sysfs_init();
    net_init();
    TEST_ASSERT_EQUAL_INT(0, netdev_register(&g_test_dev));
    net_observe_init();
    /* Reset stats: net_observe_init sends a TX probe that increments tx_packets */
    memset(&g_test_dev.stats, 0, sizeof(g_test_dev.stats));
}

void tearDown(void) {}

void test_sysfs_dynamic_files_reflect_live_counter_changes(void)
{
    sysfs_node_t *class_dir = find_child(g_sysfs_root, "class");
    sysfs_node_t *net_dir = find_child(class_dir, "net");
    sysfs_node_t *eth0_dir = find_child(net_dir, "eth0");
    sysfs_node_t *tx_packets = find_child(eth0_dir, "tx_packets");
    sysfs_node_t *rx_dropped = find_child(eth0_dir, "rx_dropped");
    char buf[32];
    uint8_t short_frame[59] = { 0 };
    uint8_t bad_frame[12] = { 0 };
    uint8_t good_frame[60] = { 0 };
    int len;

    len = read_sysfs_file(tx_packets, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_STRING_LEN("0\n", buf, (size_t)len);

    TEST_ASSERT_EQUAL_INT(-1, net_observe_tx(NULL, short_frame, sizeof(short_frame)));

    len = read_sysfs_file(tx_packets, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("0\n", buf, (size_t)len);

    TEST_ASSERT_EQUAL_INT(0, net_observe_rx(NULL, bad_frame, sizeof(bad_frame)));
    len = read_sysfs_file(rx_dropped, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("1\n", buf, (size_t)len);

    TEST_ASSERT_EQUAL_INT(0, net_observe_tx(NULL, good_frame, sizeof(good_frame)));

    len = read_sysfs_file(tx_packets, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("1\n", buf, (size_t)len);
}

void test_link_state_and_address_files_use_expected_format(void)
{
    sysfs_node_t *class_dir = find_child(g_sysfs_root, "class");
    sysfs_node_t *net_dir = find_child(class_dir, "net");
    sysfs_node_t *eth0_dir = find_child(net_dir, "eth0");
    sysfs_node_t *address = find_child(eth0_dir, "address");
    sysfs_node_t *link_state = find_child(eth0_dir, "link_state");
    char buf[32];
    int len;

    len = read_sysfs_file(address, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("52:54:00:ab:cd:ef\n", buf, (size_t)len);

    len = read_sysfs_file(link_state, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("up\n", buf, (size_t)len);

    g_link_up = false;
    g_test_dev.link_up = false;

    len = read_sysfs_file(link_state, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("down\n", buf, (size_t)len);
}

void test_supported_rx_classification_updates_counters(void)
{
    sysfs_node_t *class_dir = find_child(g_sysfs_root, "class");
    sysfs_node_t *net_dir = find_child(class_dir, "net");
    sysfs_node_t *eth0_dir = find_child(net_dir, "eth0");
    sysfs_node_t *rx_packets = find_child(eth0_dir, "rx_packets");
    char buf[32];
    uint8_t arp_frame[ETH_HEADER_LEN + 28] = { 0 };
    int len;

    arp_frame[12] = 0x08;
    arp_frame[13] = 0x06;
    TEST_ASSERT_EQUAL_INT(0, net_observe_rx(NULL, arp_frame, sizeof(arp_frame)));

    len = read_sysfs_file(rx_packets, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING_LEN("1\n", buf, (size_t)len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sysfs_dynamic_files_reflect_live_counter_changes);
    RUN_TEST(test_link_state_and_address_files_use_expected_format);
    RUN_TEST(test_supported_rx_classification_updates_counters);
    return UNITY_END();
}
