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

#ifndef _MINIOS_NET_LWIP_PORT_H_
#define _MINIOS_NET_LWIP_PORT_H_

#include <miniOS/net/ethernet.h>
#include <miniOS/net/netdev.h>

typedef enum {
    LWIP_PORT_FRAME_UNSUPPORTED = 0,
    LWIP_PORT_FRAME_ARP,
    LWIP_PORT_FRAME_IPV4,
} lwip_port_frame_kind_t;

typedef void (*lwip_port_rx_callback_t)(netdev_t *dev,
                                        lwip_port_frame_kind_t kind,
                                        const void *frame,
                                        size_t frame_len,
                                        const ethernet_frame_info_t *info,
                                        void *ctx);

typedef void (*lwip_port_timeout_callback_t)(uint32_t elapsed_ms, void *ctx);

netdev_t *lwip_port_attach(lwip_port_rx_callback_t rx_callback,
                               void *rx_ctx,
                               lwip_port_timeout_callback_t timeout_callback,
                               void *timeout_ctx);
void lwip_port_poll(void);
void lwip_port_poll_timeouts(uint32_t elapsed_ms);
int lwip_port_tx(const void *frame, size_t frame_len);
lwip_port_frame_kind_t lwip_port_classify(const ethernet_frame_info_t *info);

#ifdef TEST_BUILD
net_device_t *lwip_port_attached_device(void);
uint32_t lwip_port_timeout_accumulator(void);
uint32_t lwip_port_drop_count(void);
#endif

#endif /* _MINIOS_NET_LWIP_PORT_H_ */
