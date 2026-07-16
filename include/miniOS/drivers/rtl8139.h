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

#ifndef _MINIOS_DRIVERS_RTL8139_H_
#define _MINIOS_DRIVERS_RTL8139_H_

#include <miniOS/drivers/pci.h>
#include <miniOS/net/netdev.h>

#define RTL8139_PCI_VENDOR_ID 0x10EC
#define RTL8139_PCI_DEVICE_ID 0x8139

int rtl8139_probe_and_init(void);
int rtl8139_poll(netdev_t *dev);

#ifdef TEST_BUILD
bool rtl8139_tx_length_valid(size_t frame_len);
bool rtl8139_rx_status_valid(uint16_t status, size_t frame_len, size_t avail_len);
uint32_t rtl8139_ring_advance(uint32_t ring_offset, uint32_t consumed_len, uint32_t ring_size);
#endif

#endif /* _MINIOS_DRIVERS_RTL8139_H_ */
