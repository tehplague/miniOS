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

#ifndef _MINIOS_NET_ETHERNET_H_
#define _MINIOS_NET_ETHERNET_H_

#include <miniOS/types.h>

#define ETH_ADDR_LEN 6
#define ETH_HEADER_LEN 14
#define ETH_MIN_FRAME_LEN 60   /* minimum Ethernet frame without FCS */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

typedef struct {
    uint8_t dst_mac[ETH_ADDR_LEN];
    uint8_t src_mac[ETH_ADDR_LEN];
    uint16_t ethertype;
    const uint8_t *payload;
    uint16_t payload_len;
} ethernet_frame_info_t;

bool ethernet_frame_info_parse(const void *frame,
                               size_t frame_len,
                               ethernet_frame_info_t *info_out);

#endif /* _MINIOS_NET_ETHERNET_H_ */
