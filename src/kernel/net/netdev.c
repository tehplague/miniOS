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

#include <miniOS/net/netdev.h>
#include <miniOS/net/ethernet.h>
#include <miniOS/drivers/rtl8139.h>
#include <miniOS/drivers/virtio_net.h>
#include <miniOS/io.h>
#include <string.h>

static netdev_t *g_net_devices[NET_MAX_DEVICES];
static uint32_t g_net_device_count;

bool ethernet_frame_info_parse(const void *frame,
                               size_t frame_len,
                               ethernet_frame_info_t *info_out)
{
    const uint8_t *bytes = (const uint8_t *)frame;

    if (!frame || !info_out)
        return false;
    if (frame_len < ETH_HEADER_LEN || frame_len > NET_MAX_FRAME_SIZE)
        return false;

    memcpy(info_out->dst_mac, bytes, ETH_ADDR_LEN);
    memcpy(info_out->src_mac, bytes + ETH_ADDR_LEN, ETH_ADDR_LEN);
    info_out->ethertype = (uint16_t)(((uint16_t)bytes[12] << 8) | bytes[13]);
    info_out->payload = bytes + ETH_HEADER_LEN;
    info_out->payload_len = (uint16_t)(frame_len - ETH_HEADER_LEN);
    return true;
}

void net_init(void)
{
    memset(g_net_devices, 0, sizeof(g_net_devices));
    g_net_device_count = 0;

    int found = virtio_net_probe_and_init();
    if (found != 0)
        found = rtl8139_probe_and_init();

    if (found == 0) {
        netdev_t *dev = netdev_first();
        if (dev) {
            printk("Net: device %s registered MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
                   dev->name,
                   dev->mac[0], dev->mac[1], dev->mac[2],
                   dev->mac[3], dev->mac[4], dev->mac[5],
                   dev->link_up ? "up" : "down");
        }
    } else {
        printk("Net: no supported ethernet device found\n");
    }
}

int netdev_register(netdev_t *dev)
{
    if (!dev || !dev->ops)
        return -1;
    if (g_net_device_count >= NET_MAX_DEVICES)
        return -1;

    memset(dev->name, 0, sizeof(dev->name));
    dev->name[0] = 'e';
    dev->name[1] = 't';
    dev->name[2] = 'h';
    dev->name[3] = (char)('0' + (int)g_net_device_count);

    if (dev->ops->get_mac)
        dev->ops->get_mac(dev, dev->mac);
    if (dev->ops->get_link)
        dev->link_up = dev->ops->get_link(dev);

    g_net_devices[g_net_device_count++] = dev;

    if (dev->ops->open)
        return dev->ops->open(dev);
    return 0;
}

netdev_t *netdev_first(void)
{
    if (g_net_device_count == 0)
        return NULL;
    return g_net_devices[0];
}

void netdev_poll_all(void)
{
    for (uint32_t i = 0; i < g_net_device_count; i++) {
        netdev_t *dev = g_net_devices[i];
        if (dev && dev->ops && dev->ops->poll)
            dev->ops->poll(dev);
    }
}
