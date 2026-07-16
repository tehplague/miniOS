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

#ifndef _MINIOS_NET_NETDEV_H_
#define _MINIOS_NET_NETDEV_H_

#include <miniOS/types.h>

#define NET_MAX_DEVICES 4
#define NET_MAX_FRAME_SIZE 1518

struct net_device;

typedef void (*net_rx_handler_t)(struct net_device *dev,
                                 const void *frame,
                                 size_t frame_len,
                                 void *ctx);

typedef struct {
    int  (*open)(struct net_device *dev);
    int  (*poll)(struct net_device *dev);
    int  (*xmit)(struct net_device *dev, const void *frame, size_t frame_len);
    int  (*get_mac)(struct net_device *dev, uint8_t mac_out[6]);
    bool (*get_link)(struct net_device *dev);
} netdev_ops_t;

typedef struct net_device {
    char name[8];
    uint8_t mac[6];
    uint16_t mtu;
    bool link_up;
    void *driver_data;
    net_rx_handler_t rx_handler;
    void *rx_handler_ctx;
    const netdev_ops_t *ops;
    struct
    {
        uint64_t rx_packets;
        uint64_t rx_dropped;
        uint64_t tx_packets;
        uint64_t tx_dropped;
        uint64_t rx_bytes;
        uint64_t tx_bytes;
    } stats;
} netdev_t;

typedef netdev_t net_device_t;

void net_init(void);
int netdev_register(netdev_t *dev);
netdev_t *netdev_first(void);
void netdev_poll_all(void);

#endif /* _MINIOS_NET_NETDEV_H_ */
