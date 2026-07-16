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

int rtl8139_probe_and_init(void)   { return -1; }
int virtio_net_probe_and_init(void) { return -1; }

#include "../../src/kernel/net/netdev.c"

void setUp(void) {}
void tearDown(void) {}

void test_registering_one_device_exposes_first_device_with_mac_and_mtu(void)
{
    netdev_ops_t ops = {
        .get_mac = NULL,
        .get_link = NULL,
    };
    net_device_t dev = {
        .name = "",
        .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
        .mtu = 1500,
        .link_up = true,
        .ops = &ops,
    };

    net_init();
    TEST_ASSERT_EQUAL_INT(0, netdev_register(&dev));

    net_device_t *first = netdev_first();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("eth0", first->name);
    TEST_ASSERT_EQUAL_UINT16(1500, first->mtu);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(dev.mac, first->mac, 6);
}

void test_ethernet_helper_rejects_frames_outside_legal_bounds(void)
{
    ethernet_frame_info_t info;
    uint8_t too_short[ETH_HEADER_LEN - 1] = { 0 };
    uint8_t too_long[NET_MAX_FRAME_SIZE + 1] = { 0 };

    TEST_ASSERT_FALSE(ethernet_frame_info_parse(too_short, sizeof(too_short), &info));
    TEST_ASSERT_FALSE(ethernet_frame_info_parse(too_long, sizeof(too_long), &info));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_registering_one_device_exposes_first_device_with_mac_and_mtu);
    RUN_TEST(test_ethernet_helper_rejects_frames_outside_legal_bounds);
    return UNITY_END();
}
