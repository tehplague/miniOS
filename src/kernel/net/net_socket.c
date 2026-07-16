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
 * @file net_socket.c
 * @brief Kernel socket state layer for miniOS.
 *
 * Provides a static pool of net_sock_t structs. The lwIP PCB pointer
 * (pcb field) is set by the syscall layer when bind/connect is called.
 * Until then, send/recv return -1.
 */

#include <miniOS/net/net_socket.h>
#include <miniOS/sched/sched.h>
#include <miniOS/net/lwip_netif.h>
#include <string.h>
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"

static net_sock_t g_sock_pool[NET_SOCK_POOL_SIZE];

/**
 * @brief Allocate a socket from the static pool.
 *
 * Scans the pool for a free slot, zeroes it, and sets proto and in_use.
 * Returns NULL if all NET_SOCK_POOL_SIZE slots are occupied.
 */
net_sock_t *net_sock_alloc(int proto)
{
    for (int i = 0; i < NET_SOCK_POOL_SIZE; i++) {
        if (!g_sock_pool[i].in_use) {
            memset(&g_sock_pool[i], 0, sizeof(net_sock_t));
            g_sock_pool[i].in_use = 1;
            g_sock_pool[i].proto  = proto;
            return &g_sock_pool[i];
        }
    }
    return NULL;  /* pool exhausted */
}

/**
 * @brief Release a socket slot back to the pool.
 *
 * D-04: call lwIP teardown BEFORE nulling pcb, so the raw/udp/tcp layer
 * removes the pcb from its internal list. Then null pcb BEFORE clearing
 * in_use so any recv callback that races cannot reach a freed slot.
 */
void net_sock_free(net_sock_t *sock)
{
    if (!sock)
        return;
    /* D-04: call lwIP teardown BEFORE nulling pcb, so the raw/udp/tcp layer
     * removes the pcb from its internal list. Then null pcb BEFORE clearing
     * in_use so any recv callback that races cannot reach a freed slot. */
    if (sock->pcb) {
        if (sock->proto == SOCK_PROTO_RAW)
            raw_remove((struct raw_pcb *)sock->pcb);
        else if (sock->proto == SOCK_PROTO_UDP)
            udp_remove((struct udp_pcb *)sock->pcb);
        else if (sock->proto == SOCK_PROTO_TCP)
            tcp_abort((struct tcp_pcb *)sock->pcb);
        sock->pcb = NULL;
    }
    sock->rx_head   = 0;
    sock->rx_tail   = 0;
    sock->rx_ring[0].len = 0;
    sock->rx_ring[1].len = 0;
    sock->rcvtimeo_ticks = 0;
    sock->connected = 0;
    sock->in_use    = 0;
}

/**
 * @brief Receive data from socket ring head (non-blocking).
 *
 * Returns -1 immediately when PCB is NULL (socket not connected) or
 * when the rx ring is empty. Copies min(slot->len, len) bytes on success
 * and advances rx_head.
 */
int net_sock_recv(net_sock_t *sock, void *buf, size_t len, int flags)
{
    (void)flags;
    if (!sock || !sock->pcb)
        return -1;  /* not connected */
    if (!net_sock_has_data(sock))
        return -1;  /* no data ready */

    net_sock_rx_entry_t *slot = &sock->rx_ring[sock->rx_head & (NET_SOCK_RX_RING_SLOTS - 1)];
    size_t copy = slot->len < len ? slot->len : len;
    memcpy(buf, slot->buf, copy);
    slot->len = 0;
    sock->rx_head++;
    return (int)copy;
}

/**
 * @brief Send data through socket.
 *
 * Returns -1 when PCB is NULL (socket not connected). Actual lwIP
 * send (udp_send/tcp_write) is implemented here for each protocol type.
 */
int net_sock_send(net_sock_t *sock, const void *buf, size_t len, int flags)
{
    (void)flags;
    if (!sock || !sock->pcb)
        return -1;  /* no PCB — not connected */

    if (len > 65535)
        return -1;  /* EMSGSIZE — truncation guard */
    if (sock->proto == SOCK_PROTO_UDP) {
        /* UDP: allocate pbuf, copy payload, send via connected PCB */
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) return -1;
        memcpy(p->payload, buf, len);
        err_t e = udp_send((struct udp_pcb *)sock->pcb, p);
        pbuf_free(p);
        return (e == ERR_OK) ? (int)len : -1;
    } else if (sock->proto == SOCK_PROTO_RAW) {
        /* Raw send via write() — uses raw_send (no dest addr, requires prior sendto pattern) */
        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)len, PBUF_RAM);
        if (!p) return -1;
        memcpy(p->payload, buf, len);
        err_t e = raw_send((struct raw_pcb *)sock->pcb, p);
        pbuf_free(p);
        return (e == ERR_OK) ? (int)len : -1;
    } else {
        /* TCP: write into lwIP send buffer and flush */
        err_t e = tcp_write((struct tcp_pcb *)sock->pcb, buf, (u16_t)len,
                            TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) return -1;
        tcp_output((struct tcp_pcb *)sock->pcb);
        return (int)len;
    }
}

/**
 * @brief Close socket: free pool slot (including lwIP PCB teardown).
 *
 * @return 0 on success.
 */
int net_sock_close(net_sock_t *sock)
{
    if (!sock)
        return -1;
    net_sock_free(sock);
    return 0;
}

/**
 * net_poll_thread_fn() - Dedicated kernel thread that drives lwIP polling.
 *
 * Runs continuously, calling lwip_netif_poll() so that network receive
 * callbacks fire even when userspace threads are sleeping in poll/select.
 * Receive callbacks call sched_wake_tid() to unblock waiting threads.
 */
void net_poll_thread_fn(void *arg)
{
    (void)arg;
    for (;;) {
        lwip_netif_poll();
        sched_yield();
    }
}
