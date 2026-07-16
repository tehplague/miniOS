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
#include <stdlib.h>

#include <miniOS/drivers/pci.h>
#include <miniOS/net/netdev.h>
#include <miniOS/net/ethernet.h>

pci_device_t pci_devices[PCI_MAX_DEVICES];
int pci_device_count = 0;

bool ethernet_frame_info_parse(const void *frame, size_t frame_len, ethernet_frame_info_t *info_out)
{
    (void)frame;
    (void)frame_len;
    (void)info_out;
    return true;
}

int netdev_register(net_device_t *dev)
{
    (void)dev;
    return 0;
}

void *kmalloc(size_t size)
{
    return malloc((size_t)size);
}

void kfree(void *ptr)
{
    free(ptr);
}

#include "../../src/arch/x86_64/drivers/rtl8139.c"

void setUp(void) {}
void tearDown(void) {}

void test_rtl8139_tx_length_guards_reject_frames_outside_ethernet_window(void)
{
    TEST_ASSERT_FALSE(rtl8139_tx_length_valid(59));
    TEST_ASSERT_TRUE(rtl8139_tx_length_valid(60));
    TEST_ASSERT_TRUE(rtl8139_tx_length_valid(NET_MAX_FRAME_SIZE));
    TEST_ASSERT_FALSE(rtl8139_tx_length_valid(NET_MAX_FRAME_SIZE + 1));
}

void test_rtl8139_receive_parser_rejects_invalid_status_and_wrapped_lengths(void)
{
    TEST_ASSERT_FALSE(rtl8139_rx_status_valid(0x0000, 64, 64));
    TEST_ASSERT_FALSE(rtl8139_rx_status_valid(0x0001, 64, 32));
    TEST_ASSERT_TRUE(rtl8139_rx_status_valid(0x0001, 64, 64));
}

void test_rtl8139_ring_bookkeeping_wraps_on_32bit_aligned_boundary(void)
{
    TEST_ASSERT_EQUAL_UINT32(128, rtl8139_ring_advance(0, 124, 8192));
    TEST_ASSERT_EQUAL_UINT32(0, rtl8139_ring_advance(8180, 8, 8192));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rtl8139_tx_length_guards_reject_frames_outside_ethernet_window);
    RUN_TEST(test_rtl8139_receive_parser_rejects_invalid_status_and_wrapped_lengths);
    RUN_TEST(test_rtl8139_ring_bookkeeping_wraps_on_32bit_aligned_boundary);
    return UNITY_END();
}
