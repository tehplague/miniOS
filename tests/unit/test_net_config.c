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

#include <miniOS/net/net_config.h>

int rtl8139_probe_and_init(void)   { return -1; }
int virtio_net_probe_and_init(void) { return -1; }

#include "../../src/kernel/net/netdev.c"
#include "../../src/kernel/net/net_config.c"

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
    .get_mac = test_get_mac,
    .get_link = test_get_link,
};

static net_device_t g_test_dev = {
    .mtu = 1500,
    .ops = &g_test_ops,
};

void setUp(void) {}
void tearDown(void) {}

void test_qemu_defaults_match_expected_ipv4_values(void)
{
    net_ipv4_config_t config = { 0 };
    const uint8_t expected_ip[4] = { 10, 0, 2, 15 };
    const uint8_t expected_mask[4] = { 255, 255, 255, 0 };
    const uint8_t expected_gateway[4] = { 10, 0, 2, 2 };

    net_config_load_qemu_defaults(&config);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_ip, config.address, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mask, config.netmask, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_gateway, config.gateway, 4);
    TEST_ASSERT_EQUAL_STRING("eth0", config.if_name);
}

void test_apply_device_identity_copies_active_mac_from_registered_netdev(void)
{
    net_ipv4_config_t config = { 0 };
    const uint8_t expected_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

    net_init();
    TEST_ASSERT_EQUAL_INT(0, netdev_register(&g_test_dev));
    TEST_ASSERT_EQUAL_INT(0, net_config_apply_device_identity(&config));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mac, config.mac, 6);
    TEST_ASSERT_EQUAL_STRING("eth0", config.if_name);
}

void test_get_active_exposes_qemu_defaults_and_runtime_identity(void)
{
    const uint8_t expected_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    const uint8_t expected_ip[4] = { 10, 0, 2, 15 };
    const net_ipv4_config_t *config;

    net_init();
    TEST_ASSERT_EQUAL_INT(0, netdev_register(&g_test_dev));
    config = net_config_get_active();

    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_ip, config->address, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mac, config->mac, 6);
    TEST_ASSERT_EQUAL_STRING("eth0", config->if_name);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_qemu_defaults_match_expected_ipv4_values);
    RUN_TEST(test_apply_device_identity_copies_active_mac_from_registered_netdev);
    RUN_TEST(test_get_active_exposes_qemu_defaults_and_runtime_identity);
    return UNITY_END();
}
