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

/**
 * @file lwipopts.h
 * @brief lwIP configuration for miniOS.
 *
 * Configures lwIP in NO_SYS=1 (poll-driven) mode with ARP, IPv4, ICMP,
 * TCP, and UDP enabled. LWIP_SOCKET and LWIP_NETCONN are disabled —
 * socket API wiring is handled in the kernel socket layer (net_socket.c).
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* NO_SYS: poll-driven, no RTOS threading */
#define NO_SYS                  1

/* Prevent lwIP arch.h from typedef-ing ssize_t (int) — we get it from sys/types.h (long) */
#define LWIP_NO_UNISTD_H        1

/* Use lwIP's own range-based char checks — avoids newlib _ctype_ table in freestanding kernel */
#define LWIP_NO_CTYPE_H         1

/* Protocol modules (per D-03) */
#define LWIP_ARP                1
#define LWIP_IPV4               1
#define LWIP_ICMP               1
#define LWIP_UDP                1
#define LWIP_TCP                1
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0
#define LWIP_RAW                1

/* Ethernet */
#define LWIP_ETHERNET           1
#define LWIP_BROADCAST_PING     1

/* Memory — sized for 16 MB kernel RAM budget */
#define MEM_ALIGNMENT           4
#define MEM_SIZE                (32 * 1024)
#define MEMP_NUM_PBUF           16
#define MEMP_NUM_TCP_PCB        4
#define MEMP_NUM_UDP_PCB        4
#define MEMP_NUM_RAW_PCB        4
#define PBUF_POOL_SIZE          16
#define PBUF_POOL_BUFSIZE       1536

/* Software checksums — RTL8139 does not offload */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_TCP      1

/* errno provided by our port */
#define LWIP_PROVIDE_ERRNO      1

unsigned int miniOS_lwip_rand(void);
#define LWIP_RAND()             miniOS_lwip_rand()

/* Disable features not needed yet */
#define LWIP_DNS                1
#define LWIP_IGMP               0
#define LWIP_DHCP               0
#define LWIP_AUTOIP             0

#endif /* LWIPOPTS_H */
