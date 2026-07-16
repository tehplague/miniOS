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

#ifndef _MINIOS_DRIVERS_VIRTIO_NET_H_
#define _MINIOS_DRIVERS_VIRTIO_NET_H_

/**
 * virtio_net_probe_and_init() - Probe PCI bus for a virtio-net device and
 * initialise it.
 *
 * Scans pci_devices[] for vendor=0x1AF4, device=0x1000.  Negotiates legacy
 * virtio features, allocates virtqueues from physically contiguous PMM frames,
 * and registers a netdev_t via netdev_register().
 *
 * @return: 0 on success, -1 if no device found or initialisation failed.
 */
int virtio_net_probe_and_init(void);

#endif /* _MINIOS_DRIVERS_VIRTIO_NET_H_ */
