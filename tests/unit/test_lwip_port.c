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
#include <miniOS/net/ethernet.h>
#include <miniOS/net/lwip_port.h>

int rtl8139_probe_and_init(void)   { return -1; }
int virtio_net_probe_and_init(void) { return -1; }

#include "../../src/kernel/net/netdev.c"
#include "../../src/kernel/net/lwip_port.c"

static int g_poll_count;
static int g_tx_count;
static const void *g_last_rx_frame;
static size_t g_last_rx_frame_len;
static uint16_t g_last_rx_payload_len;
static lwip_port_frame_kind_t g_last_rx_kind;
static uint32_t g_last_timeout_elapsed_ms;
static void *g_last_timeout_ctx;
static void *g_last_rx_ctx;

static int test_open(net_device_t *dev)
{
    (void)dev;
    return 0;
}

static int test_poll(net_device_t *dev)
{
    (void)dev;
    g_poll_count++;
    return 0;
}

static int test_xmit(net_device_t *dev, const void *frame, size_t frame_len)
{
    (void)dev;
    (void)frame;
    g_tx_count++;
    g_last_rx_frame = frame;
    g_last_rx_frame_len = frame_len;
    return 0;
}

static int test_get_mac(net_device_t *dev, uint8_t mac_out[6])
{
    (void)dev;
    const uint8_t expected_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

    memcpy(mac_out, expected_mac, sizeof(expected_mac));
    return 0;
}

static bool test_get_link(net_device_t *dev)
{
    (void)dev;
    return true;
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

void setUp(void) {}
void tearDown(void) {}

static void test_rx_callback(net_device_t *dev,
                             lwip_port_frame_kind_t kind,
                             const void *frame,
                             size_t frame_len,
                             const ethernet_frame_info_t *info,
                             void *ctx)
{
    g_last_rx_frame = frame;
    g_last_rx_frame_len = frame_len;
    g_last_rx_kind = kind;
    g_last_rx_payload_len = info ? info->payload_len : 0;
    g_last_rx_ctx = ctx;
    TEST_ASSERT_NOT_NULL(dev);
}

static void test_timeout_callback(uint32_t elapsed_ms, void *ctx)
{
    g_last_timeout_elapsed_ms = elapsed_ms;
    g_last_timeout_ctx = ctx;
}

static void reset_test_state(void)
{
    g_poll_count = 0;
    g_tx_count = 0;
    g_last_rx_frame = NULL;
    g_last_rx_frame_len = 0;
    g_last_rx_payload_len = 0;
    g_last_rx_kind = LWIP_PORT_FRAME_UNSUPPORTED;
    g_last_timeout_elapsed_ms = 0;
    g_last_timeout_ctx = NULL;
    g_last_rx_ctx = NULL;

    net_init();
    TEST_ASSERT_EQUAL_INT(0, netdev_register(&g_test_dev));
}

void test_attaching_callback_returns_active_eth0_device(void)
{
    reset_test_state();

    net_device_t *dev = lwip_port_attach(test_rx_callback,
                                         (void *)0x1234,
                                         test_timeout_callback,
                                         (void *)0x5678);

    TEST_ASSERT_NOT_NULL(dev);
    TEST_ASSERT_EQUAL_STRING("eth0", dev->name);
    TEST_ASSERT_EQUAL_PTR(dev, lwip_port_attached_device());
}

void test_ethertype_classification_only_maps_arp_and_ipv4(void)
{
    ethernet_frame_info_t info = { 0 };

    info.ethertype = ETHERTYPE_ARP;
    TEST_ASSERT_EQUAL_INT(LWIP_PORT_FRAME_ARP, lwip_port_classify(&info));

    info.ethertype = ETHERTYPE_IPV4;
    TEST_ASSERT_EQUAL_INT(LWIP_PORT_FRAME_IPV4, lwip_port_classify(&info));

    info.ethertype = 0x86DD;
    TEST_ASSERT_EQUAL_INT(LWIP_PORT_FRAME_UNSUPPORTED, lwip_port_classify(&info));
}

void test_tx_returns_error_when_no_registered_device_exists(void)
{
    uint8_t frame[60] = { 0 };

    net_init();
    TEST_ASSERT_EQUAL_INT(-1, lwip_port_tx(frame, sizeof(frame)));
}

void test_supported_arp_and_ipv4_frames_reach_attached_callback(void)
{
    uint8_t arp_frame[ETH_HEADER_LEN + 28] = { 0 };
    uint8_t ipv4_frame[ETH_HEADER_LEN + 20] = { 0 };

    reset_test_state();
    TEST_ASSERT_NOT_NULL(lwip_port_attach(test_rx_callback,
                                          (void *)0x1234,
                                          test_timeout_callback,
                                          (void *)0x5678));

    arp_frame[12] = 0x08;
    arp_frame[13] = 0x06;
    g_test_dev.rx_handler(&g_test_dev, arp_frame, sizeof(arp_frame), g_test_dev.rx_handler_ctx);
    TEST_ASSERT_EQUAL_INT(LWIP_PORT_FRAME_ARP, g_last_rx_kind);
    TEST_ASSERT_EQUAL_PTR(arp_frame, g_last_rx_frame);
    TEST_ASSERT_EQUAL_UINT(sizeof(arp_frame), g_last_rx_frame_len);
    TEST_ASSERT_EQUAL_UINT(28, g_last_rx_payload_len);
    TEST_ASSERT_EQUAL_PTR((void *)0x1234, g_last_rx_ctx);

    ipv4_frame[12] = 0x08;
    ipv4_frame[13] = 0x00;
    g_test_dev.rx_handler(&g_test_dev, ipv4_frame, sizeof(ipv4_frame), g_test_dev.rx_handler_ctx);
    TEST_ASSERT_EQUAL_INT(LWIP_PORT_FRAME_IPV4, g_last_rx_kind);
    TEST_ASSERT_EQUAL_PTR(ipv4_frame, g_last_rx_frame);
    TEST_ASSERT_EQUAL_UINT(sizeof(ipv4_frame), g_last_rx_frame_len);
    TEST_ASSERT_EQUAL_UINT(20, g_last_rx_payload_len);
}

void test_unsupported_ethertypes_are_counted_and_dropped(void)
{
    uint8_t unsupported_frame[ETH_HEADER_LEN + 12] = { 0 };

    reset_test_state();
    TEST_ASSERT_NOT_NULL(lwip_port_attach(test_rx_callback,
                                          (void *)0x1234,
                                          test_timeout_callback,
                                          (void *)0x5678));

    unsupported_frame[12] = 0x86;
    unsupported_frame[13] = 0xDD;
    g_test_dev.rx_handler(&g_test_dev, unsupported_frame, sizeof(unsupported_frame), g_test_dev.rx_handler_ctx);

    TEST_ASSERT_EQUAL_UINT32(1, lwip_port_drop_count());
    TEST_ASSERT_NULL(g_last_rx_frame);
}

void test_timeout_polling_accumulates_elapsed_time_and_invokes_hook(void)
{
    reset_test_state();
    TEST_ASSERT_NOT_NULL(lwip_port_attach(test_rx_callback,
                                          (void *)0x1234,
                                          test_timeout_callback,
                                          (void *)0x5678));

    lwip_port_poll();
    TEST_ASSERT_EQUAL_INT(1, g_poll_count);

    lwip_port_poll_timeouts(25);
    lwip_port_poll_timeouts(75);

    TEST_ASSERT_EQUAL_UINT32(100, lwip_port_timeout_accumulator());
    TEST_ASSERT_EQUAL_UINT32(75, g_last_timeout_elapsed_ms);
    TEST_ASSERT_EQUAL_PTR((void *)0x5678, g_last_timeout_ctx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_attaching_callback_returns_active_eth0_device);
    RUN_TEST(test_ethertype_classification_only_maps_arp_and_ipv4);
    RUN_TEST(test_tx_returns_error_when_no_registered_device_exists);
    RUN_TEST(test_supported_arp_and_ipv4_frames_reach_attached_callback);
    RUN_TEST(test_unsupported_ethertypes_are_counted_and_dropped);
    RUN_TEST(test_timeout_polling_accumulates_elapsed_time_and_invokes_hook);
    return UNITY_END();
}
