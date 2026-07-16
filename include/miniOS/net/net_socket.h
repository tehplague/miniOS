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

#ifndef _MINIOS_NET_NET_SOCKET_H_
#define _MINIOS_NET_NET_SOCKET_H_

#include <stddef.h>
#include <stdint.h>

#define SOCK_PROTO_UDP  0   /**< UDP protocol selector for net_sock_alloc() */
#define SOCK_PROTO_TCP  1   /**< TCP protocol selector for net_sock_alloc() */
#define SOCK_PROTO_RAW  2   /**< Raw IP protocol selector for net_sock_alloc() */

#define NET_SOCK_RX_BUF_SIZE  1500
#define NET_SOCK_POOL_SIZE    4    /**< Max concurrent sockets (matches MEMP_NUM_*_PCB) */
#define NET_SOCK_RX_RING_SLOTS 2   /**< Slots in the per-socket RAW rx ring */

/**
 * @brief One slot in the socket rx ring.
 *
 * Used for RAW sockets (2 slots) and by UDP/TCP (single-slot at index 0).
 */
typedef struct net_sock_rx_entry {
    uint8_t  buf[NET_SOCK_RX_BUF_SIZE]; /**< Received frame payload (IP+ICMP for RAW, UDP payload for UDP, TCP bytes for TCP) */
    size_t   len;                       /**< Bytes in buf; 0 = slot empty */
    uint32_t addr_be;                   /**< Peer IPv4 in network byte order */
    uint16_t port_be;                   /**< Peer port in network byte order (0 for RAW) */
    uint16_t _pad;                      /**< Alignment */
} net_sock_rx_entry_t;

/**
 * @brief Kernel socket state.
 *
 * One net_sock_t per open socket fd. Owns the lwIP PCB pointer and a
 * 2-slot receive ring for RAW sockets (UDP/TCP use slot 0 only).
 * No dynamic allocation beyond the static pool.
 */
typedef struct net_sock {
    int      proto;                          /**< SOCK_PROTO_UDP, SOCK_PROTO_TCP, or SOCK_PROTO_RAW */
    void    *pcb;                            /**< lwIP PCB: udp_pcb* or tcp_pcb* */
    net_sock_rx_entry_t rx_ring[NET_SOCK_RX_RING_SLOTS]; /**< 2-slot RAW burst buffer; UDP/TCP use slot 0 only */
    uint8_t  rx_head;        /**< Consumer index (monotonic uint8 wrap; mask with (NET_SOCK_RX_RING_SLOTS-1)) */
    uint8_t  rx_tail;        /**< Producer index (monotonic uint8 wrap) */
    uint8_t  _pad2[2];
    uint32_t rcvtimeo_ticks; /**< SO_RCVTIMEO as LAPIC ticks (10ms each); 0 = use 10s default */
    int      connected;                      /**< Non-zero after connect/accept */
    int      in_use;                         /**< Pool slot occupied flag */
    uint32_t poll_waiter_tid;               /**< tid of thread sleeping in poll/select for this socket; 0 = none */
} net_sock_t;

/**
 * @brief Returns non-zero when the socket rx ring has at least one entry ready.
 */
static inline int net_sock_has_data(const net_sock_t *s) {
    return s && ((uint8_t)(s->rx_tail - s->rx_head) > 0);
}

/**
 * @brief Returns non-zero when the socket rx ring is full (all slots occupied).
 */
static inline int net_sock_ring_full(const net_sock_t *s) {
    return s && ((uint8_t)(s->rx_tail - s->rx_head) >= NET_SOCK_RX_RING_SLOTS);
}

/**
 * @brief Allocate a socket from the static pool.
 * @param proto  SOCK_PROTO_UDP, SOCK_PROTO_TCP, or SOCK_PROTO_RAW
 * @return Pointer to net_sock_t, or NULL if pool exhausted.
 */
net_sock_t *net_sock_alloc(int proto);

/**
 * @brief Free a socket slot back to the pool.
 *        Calls raw_remove/udp_remove/tcp_abort based on sock->proto before freeing the slot;
 *        sets sock->pcb=NULL before clearing in_use (UAF safety — prevents recv callbacks from
 *        firing on a freed slot).
 */
void net_sock_free(net_sock_t *sock);

/**
 * @brief Receive data from socket into buf.
 * @return Bytes received, -1 if no data ready or PCB not set.
 */
int net_sock_recv(net_sock_t *sock, void *buf, size_t len, int flags);

/**
 * @brief Send data through socket.
 * @return Bytes sent, -1 on error (PCB not set, or lwIP error).
 */
int net_sock_send(net_sock_t *sock, const void *buf, size_t len, int flags);

/**
 * @brief Close socket: remove lwIP PCB and free pool slot.
 * @return 0 on success.
 */
int net_sock_close(net_sock_t *sock);

#endif /* _MINIOS_NET_NET_SOCKET_H_ */
