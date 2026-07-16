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

#include <miniOS/syscall.h>
#include <miniOS/sched/sched.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/net/net_socket.h>
#include <miniOS/net/lwip_netif.h>
#include <miniOS/net/unix_sock.h>
#include <miniOS/types.h>
#include <string.h>
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/ip4.h"
#include "lwip/def.h"
#include "syscall_internal.h"

/* Pending accept PCB deposited by sys_tcp_accept_cb, consumed by SYS_accept */
static struct tcp_pcb *g_pending_accept_pcb;

/**
 * raw_copy_ipv4_into_ring_slot() - Copy an IPv4 ICMP packet into an rx ring slot.
 *
 * Assembles the IP header (from ip4_current_header()) plus the ICMP payload
 * from the pbuf into slot->buf. Updates slot->len to the total bytes written.
 * Returns the byte count written (0 on error or if p/slot is NULL).
 */
static size_t raw_copy_ipv4_into_ring_slot(net_sock_rx_entry_t *slot, struct pbuf *p) {
    const struct ip_hdr *iph = ip4_current_header();
    size_t header_len = IP_HLEN;
    size_t payload_offset = 0;
    size_t payload_len;

    if (!slot || !p)
        return 0;

    if (iph && IPH_HL_BYTES(iph) >= IP_HLEN)
        header_len = IPH_HL_BYTES(iph);

    if (p->tot_len >= header_len) {
        uint8_t first = 0;
        pbuf_copy_partial(p, &first, 1, 0);
        if ((first >> 4) == 4)
            payload_offset = header_len;
    }

    payload_len = (p->tot_len > payload_offset) ? (p->tot_len - payload_offset) : p->tot_len;
    if (payload_len > NET_SOCK_RX_BUF_SIZE - header_len)
        payload_len = NET_SOCK_RX_BUF_SIZE - header_len;

    memset(slot->buf, 0, header_len);
    if (iph) {
        memcpy(slot->buf, iph, header_len);
        ((struct ip_hdr *)slot->buf)->_len =
            lwip_htons((u16_t)(header_len + payload_len));
    } else {
        struct ip_hdr *hdr = (struct ip_hdr *)slot->buf;
        IPH_VHL_SET(hdr, 4, (u8_t)(header_len / 4));
        IPH_LEN_SET(hdr, lwip_htons((u16_t)(header_len + payload_len)));
        IPH_TTL_SET(hdr, 64);
        IPH_PROTO_SET(hdr, IP_PROTO_ICMP);
    }

    pbuf_copy_partial(p, slot->buf + header_len, (u16_t)payload_len, (u16_t)payload_offset);
    return header_len + payload_len;
}

static err_t sys_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    if (err == ERR_OK && arg)
        ((net_sock_t *)arg)->connected = 1;
    return ERR_OK;
}

static err_t sys_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err == ERR_OK)
        g_pending_accept_pcb = newpcb;
    return ERR_OK;
}

/**
 * sys_udp_recv_cb() - lwIP UDP receive callback.
 *
 * Single-slot semantics (D-05): writes into rx_ring[0] and forces head=0, tail=1
 * so net_sock_has_data() returns true. Each callback overwrites the previous entry.
 */
static void sys_udp_recv_cb(void *arg, struct udp_pcb *pcb,
                            struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    net_sock_t *s = (net_sock_t *)arg;
    if (!s || !p) { if (p) pbuf_free(p); return; }
    net_sock_rx_entry_t *slot = &s->rx_ring[0];
    size_t copy = p->tot_len < NET_SOCK_RX_BUF_SIZE ? p->tot_len : NET_SOCK_RX_BUF_SIZE;
    pbuf_copy_partial(p, slot->buf, (u16_t)copy, 0);
    slot->len     = copy;
    slot->addr_be = addr ? ip_2_ip4(addr)->addr : 0;
    slot->port_be = lwip_htons(port);
    /* Single-slot semantics (D-05, UDP/TCP only): always overwrite rx_ring[0]
     * and force head=0, tail=1 so net_sock_has_data() returns true.
     * The most-recent datagram wins; any un-consumed prior entry is discarded.
     * NOTE: RAW sockets use 2-slot ring semantics via sys_raw_recv_cb instead. */
    s->rx_head = 0;
    s->rx_tail = 1;
    if (s->poll_waiter_tid)
        sched_wake_tid(s->poll_waiter_tid);
    pbuf_free(p);
}

/**
 * sys_raw_recv_cb() - lwIP RAW receive callback — pure ring producer (D-03).
 *
 * Delivers every ICMP packet addressed to the local interface into the 2-slot
 * ring without any ID/seq filter. Drops the packet when the ring is full (D-05).
 * Upstream BusyBox ping filters by ID/seq in userspace.
 */
static u8_t sys_raw_recv_cb(void *arg, struct raw_pcb *pcb,
                            struct pbuf *p, const ip_addr_t *addr) {
    (void)pcb;
    net_sock_t *s = (net_sock_t *)arg;
    if (!s || !p) {
        if (p) pbuf_free(p);
        return 1;
    }
    /* D-05: 2-slot ring; drop when full rather than overwrite in-flight entry */
    if (net_sock_ring_full(s)) {
        pbuf_free(p);
        return 1;
    }
    net_sock_rx_entry_t *slot = &s->rx_ring[s->rx_tail & (NET_SOCK_RX_RING_SLOTS - 1)];
    size_t n = raw_copy_ipv4_into_ring_slot(slot, p);
    if (n == 0) {
        pbuf_free(p);
        return 1;
    }
    slot->len     = n;
    slot->addr_be = addr ? ip_2_ip4(addr)->addr : 0;
    slot->port_be = 0;
    s->rx_tail++;
    pbuf_free(p);
    return 1;
}

/**
 * sys_tcp_recv_cb() - lwIP TCP receive callback.
 *
 * Single-slot semantics (D-05): writes into rx_ring[0] and forces head=0, tail=1.
 * Preserves tcp_recved() flow-control acknowledgement.
 */
static err_t sys_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)err;
    net_sock_t *s = (net_sock_t *)arg;
    if (!p)
        return ERR_OK;
    if (s) {
        net_sock_rx_entry_t *slot = &s->rx_ring[0];
        size_t copy = p->tot_len < NET_SOCK_RX_BUF_SIZE ? p->tot_len : NET_SOCK_RX_BUF_SIZE;
        pbuf_copy_partial(p, slot->buf, (u16_t)copy, 0);
        slot->len     = copy;
        slot->addr_be = 0;
        slot->port_be = 0;
        /* Single-slot semantics (D-05, TCP): always overwrite rx_ring[0] and
         * force head=0, tail=1. Most-recent segment wins; prior un-consumed
         * data is discarded. Intentional for current single-threaded applets. */
        s->rx_head = 0;
        s->rx_tail = 1;
        tcp_recved(pcb, p->tot_len);
        if (s->poll_waiter_tid)
            sched_wake_tid(s->poll_waiter_tid);
    }
    pbuf_free(p);
    return ERR_OK;
}


int64_t syscall_dispatch_net(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg6;

    switch (nr) {
    case SYS_socket: {
        int domain = (int)arg1;
        int type = (int)arg2;

        /* AF_UNIX (domain == 1) */
        if (domain == 1) {
            if (type != 1) return -93; /* EPROTONOSUPPORT: only SOCK_STREAM */
            unix_sock_t *us = unix_sock_alloc();
            if (!us) return -24;
            vfs_file_t *fdt = THREAD_FDT(sched_current());
            int fd = -1;
            for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
                if (!fdt[i].in_use) { fd = i; break; }
            }
            if (fd < 0) { unix_sock_free(us); return -24; }
            fdt[fd].in_use     = 1;
            fdt[fd].ftype      = VFS_FILE_TYPE_UNIX_SOCKET;
            fdt[fd].usock      = us;
            fdt[fd].ref_count  = 1;
            fdt[fd].flags      = 0;
            fdt[fd].ops        = NULL;
            return (int64_t)fd;
        }

        if (domain != 2)
            return -97;

        int proto;
        if (type == 2) {
            proto = SOCK_PROTO_UDP;
        } else if (type == 1) {
            proto = SOCK_PROTO_TCP;
        } else if (type == 3 && (int)arg3 == 1) {
            proto = SOCK_PROTO_RAW;
        } else if (type == 3) {
            return -93;
        } else {
            return -93;
        }

        net_sock_t *sock = net_sock_alloc(proto);
        if (!sock)
            return -24;

        struct thread *ts = sched_current();
        vfs_file_t *ts_fdt = THREAD_FDT(ts);
        int fd = -1;
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
            if (!ts_fdt[i].in_use) {
                fd = i;
                break;
            }
        }
        if (fd < 0) {
            net_sock_free(sock);
            return -24;
        }

        if (proto == SOCK_PROTO_UDP) {
            struct udp_pcb *pcb = udp_new();
            if (!pcb) {
                net_sock_free(sock);
                return -105;
            }
            sock->pcb = pcb;
            udp_recv(pcb, sys_udp_recv_cb, sock);
        } else if (proto == SOCK_PROTO_RAW) {
            struct raw_pcb *pcb = raw_new((uint8_t)arg3);
            if (!pcb) {
                net_sock_free(sock);
                return -105;
            }
            sock->pcb = pcb;
            raw_recv(pcb, sys_raw_recv_cb, sock);
        } else {
            struct tcp_pcb *pcb = tcp_new();
            if (!pcb) {
                net_sock_free(sock);
                return -105;
            }
            sock->pcb = pcb;
            tcp_arg(pcb, sock);
            tcp_recv(pcb, sys_tcp_recv_cb);
        }

        ts_fdt[fd].in_use = 1;
        ts_fdt[fd].ftype = VFS_FILE_TYPE_SOCKET;
        ts_fdt[fd].sock = sock;
        ts_fdt[fd].ref_count = 1;
        ts_fdt[fd].flags = 0;
        ts_fdt[fd].ops = NULL;
        return (int64_t)fd;
    }

    case SYS_bind: {
        int fd = (int)arg1;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *bvf = &THREAD_FDT(sched_current())[fd];
        if (!bvf->in_use)
            return -9;
        /* AF_UNIX bind */
        if (bvf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && bvf->usock) {
            if (!arg2) return -14;
            struct { uint16_t family; char path[108]; } *sun = (void *)arg2;
            return unix_sock_bind(bvf->usock, sun->path);
        }
        if (bvf->ftype != VFS_FILE_TYPE_SOCKET || !bvf->sock)
            return -9;
        if (!arg2)
            return -14;  /* EFAULT */
        struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; };
        struct sockaddr_in *sin = (struct sockaddr_in *)arg2;
        ip4_addr_t addr;
        addr.addr = sin->sin_addr;
        uint16_t port = lwip_ntohs(sin->sin_port);
        err_t e;
        if (bvf->sock->proto == SOCK_PROTO_UDP)
            e = udp_bind((struct udp_pcb *)bvf->sock->pcb, &addr, port);
        else if (bvf->sock->proto == SOCK_PROTO_TCP)
            e = tcp_bind((struct tcp_pcb *)bvf->sock->pcb, &addr, port);
        else
            return -95;  /* EOPNOTSUPP — raw sockets do not bind to a port */
        return (e == ERR_OK) ? 0 : -22;
    }

    case SYS_connect: {
        int fd = (int)arg1;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *cvf2 = &THREAD_FDT(sched_current())[fd];
        if (!cvf2->in_use)
            return -9;
        /* AF_UNIX connect */
        if (cvf2->ftype == VFS_FILE_TYPE_UNIX_SOCKET && cvf2->usock) {
            if (!arg2) return -14;
            struct { uint16_t family; char path[108]; } *sun = (void *)arg2;
            return unix_sock_connect(cvf2->usock, sun->path);
        }
        if (cvf2->ftype != VFS_FILE_TYPE_SOCKET || !cvf2->sock)
            return -9;
        net_sock_t *csock = cvf2->sock;
        if (!arg2)
            return -14;  /* EFAULT */
        struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; };
        struct sockaddr_in *sin = (struct sockaddr_in *)arg2;
        ip4_addr_t raddr;
        raddr.addr = sin->sin_addr;
        uint16_t rport = lwip_ntohs(sin->sin_port);
        if (csock->proto == SOCK_PROTO_UDP) {
            err_t ce = udp_connect((struct udp_pcb *)csock->pcb, &raddr, rport);
            if (ce == ERR_OK)
                csock->connected = 1;
            return (ce == ERR_OK) ? 0 : -111;
        }
        err_t ce = tcp_connect((struct tcp_pcb *)csock->pcb, &raddr, rport, sys_tcp_connected_cb);
        if (ce != ERR_OK)
            return -111;
        {
            extern volatile uint64_t lapic_tick_count;
            uint64_t conn_deadline = lapic_tick_count + 500; /* 5s at ~100Hz */
            while (!csock->connected && lapic_tick_count < conn_deadline) {
                lwip_netif_poll();
                sched_yield();
            }
        }
        return csock->connected ? 0 : -110;
    }

    case SYS_sendto: {
        int fd = (int)arg1;
        const void *sbuf = (const void *)arg2;
        size_t slen = (size_t)arg3;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *svf = &THREAD_FDT(sched_current())[fd];
        if (!svf->in_use)
            return -9;
        /* AF_UNIX sendto */
        if (svf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && svf->usock)
            return unix_sock_write(svf->usock, sbuf, slen);
        if (svf->ftype != VFS_FILE_TYPE_SOCKET || !svf->sock || !svf->sock->pcb)
            return -9;
        net_sock_t *ssock = svf->sock;
        if (slen > 65535)
            return -90;  /* EMSGSIZE */
        if (ssock->proto == SOCK_PROTO_UDP) {
            struct pbuf *sp = pbuf_alloc(PBUF_TRANSPORT, (u16_t)slen, PBUF_RAM);
            if (!sp)
                return -105;
            memcpy(sp->payload, sbuf, slen);
            err_t se;
            if (arg5 != 0) {
                struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t pad[8]; };
                struct sockaddr_in *dst = (struct sockaddr_in *)arg5;
                ip4_addr_t da;
                da.addr = dst->sin_addr;
                uint16_t dp = lwip_ntohs(dst->sin_port);
                se = udp_sendto((struct udp_pcb *)ssock->pcb, sp, (const ip_addr_t *)&da, dp);
            } else {
                se = udp_send((struct udp_pcb *)ssock->pcb, sp);
            }
            pbuf_free(sp);
            return (se == ERR_OK) ? (int64_t)slen : -5;
        } else if (ssock->proto == SOCK_PROTO_RAW) {
            struct pbuf *sp = pbuf_alloc(PBUF_IP, (u16_t)slen, PBUF_RAM);
            if (!sp)
                return -105;
            memcpy(sp->payload, sbuf, slen);
            /* D-03: no ICMP tracking — deliver packet directly */
            err_t se;
            if (arg5 != 0) {
                struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t pad[8]; };
                struct sockaddr_in *dst = (struct sockaddr_in *)arg5;
                ip4_addr_t da;
                da.addr = dst->sin_addr;
                se = raw_sendto((struct raw_pcb *)ssock->pcb, sp, (const ip_addr_t *)&da);
            } else {
                se = raw_send((struct raw_pcb *)ssock->pcb, sp);
            }
            pbuf_free(sp);
            if (se != ERR_OK)
                return -5;
            return (int64_t)slen;
        }

        err_t se = tcp_write((struct tcp_pcb *)ssock->pcb, sbuf, (u16_t)slen, TCP_WRITE_FLAG_COPY);
        if (se != ERR_OK)
            return -105;
        tcp_output((struct tcp_pcb *)ssock->pcb);
        return (int64_t)slen;
    }

    case SYS_recvfrom: {
        int fd = (int)arg1;
        void *rbuf = (void *)arg2;
        size_t rlen = (size_t)arg3;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *rvf = &THREAD_FDT(sched_current())[fd];
        if (!rvf->in_use)
            return -9;
        /* AF_UNIX recvfrom */
        if (rvf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && rvf->usock)
            return unix_sock_read(rvf->usock, rbuf, rlen);
        if (rvf->ftype != VFS_FILE_TYPE_SOCKET || !rvf->sock)
            return -9;
        net_sock_t *rsock = rvf->sock;
        {
            extern volatile uint64_t lapic_tick_count;
            /* D-06: use rcvtimeo_ticks if set, else 10-second default (1000 ticks at 100 Hz) */
            uint64_t recv_deadline = lapic_tick_count +
                (rsock->rcvtimeo_ticks ? rsock->rcvtimeo_ticks : 1000);
            while (!net_sock_has_data(rsock) && lapic_tick_count < recv_deadline) {
                lwip_netif_poll();
                /* INTR-03: ready data checked before signal */
                if (net_sock_has_data(rsock))
                    break;
                if (syscall_has_pending_signal()) {
                    /* recvfrom gets SA_RESTART */
                    sched_current()->syscall_restart_pending = 1;
                    return -4;  /* EINTR */
                }
                sched_yield();
            }
        }
        if (!net_sock_has_data(rsock))
            return -11;  /* EAGAIN — timeout */
        if (arg5 != 0 && arg6 != 0) {
            struct sockaddr_in { uint16_t sin_family; uint16_t sin_port; uint32_t sin_addr; uint8_t sin_zero[8]; };
            struct sockaddr_in *src = (struct sockaddr_in *)(uintptr_t)arg5;
            uint32_t *addrlen = (uint32_t *)(uintptr_t)arg6;
            if (*addrlen >= sizeof(*src)) {
                memset(src, 0, sizeof(*src));
                src->sin_family = 2;
                /* Read peer addr from ring head BEFORE net_sock_recv advances rx_head */
                src->sin_port = rsock->rx_ring[rsock->rx_head & (NET_SOCK_RX_RING_SLOTS - 1)].port_be;
                src->sin_addr = rsock->rx_ring[rsock->rx_head & (NET_SOCK_RX_RING_SLOTS - 1)].addr_be;
                *addrlen = sizeof(*src);
            }
        }
        return (int64_t)net_sock_recv(rsock, rbuf, rlen, 0);
    }

    case SYS_listen: {
        int fd = (int)arg1;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *lvf = &THREAD_FDT(sched_current())[fd];
        if (!lvf->in_use)
            return -9;
        /* AF_UNIX listen */
        if (lvf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && lvf->usock)
            return unix_sock_listen(lvf->usock, (int)arg2);
        if (lvf->ftype != VFS_FILE_TYPE_SOCKET || !lvf->sock)
            return -9;
        if (lvf->sock->proto != SOCK_PROTO_TCP)
            return -95;
        struct tcp_pcb *lpcb = tcp_listen((struct tcp_pcb *)lvf->sock->pcb);
        if (!lpcb)
            return -12;
        lvf->sock->pcb = lpcb;
        tcp_accept(lpcb, sys_tcp_accept_cb);
        return 0;
    }

    case SYS_accept: {
        int fd = (int)arg1;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        vfs_file_t *avf = &THREAD_FDT(sched_current())[fd];
        if (!avf->in_use)
            return -9;
        /* AF_UNIX accept */
        if (avf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && avf->usock) {
            unix_sock_t *conn = unix_sock_accept(avf->usock);
            if (!conn) return -11; /* EAGAIN/interrupted */
            vfs_file_t *afdt = THREAD_FDT(sched_current());
            int newfd = -1;
            for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
                if (!afdt[i].in_use) { newfd = i; break; }
            }
            if (newfd < 0) { unix_sock_close(conn); return -24; }
            afdt[newfd].in_use    = 1;
            afdt[newfd].ftype     = VFS_FILE_TYPE_UNIX_SOCKET;
            afdt[newfd].usock     = conn;
            afdt[newfd].ref_count = 1;
            afdt[newfd].flags     = 0;
            afdt[newfd].ops       = NULL;
            return (int64_t)newfd;
        }
        if (avf->ftype != VFS_FILE_TYPE_SOCKET || !avf->sock)
            return -9;
        if (avf->sock->proto != SOCK_PROTO_TCP)
            return -95;
        for (int _ai = 0; _ai < 1000 && !g_pending_accept_pcb; _ai++) {
            lwip_netif_poll();
            sched_yield();
        }
        if (!g_pending_accept_pcb)
            return -11;
        net_sock_t *newsock = net_sock_alloc(SOCK_PROTO_TCP);
        if (!newsock)
            return -24;
        newsock->pcb = g_pending_accept_pcb;
        newsock->connected = 1;
        g_pending_accept_pcb = NULL;

        /* Register callbacks on the accepted PCB */
        tcp_arg((struct tcp_pcb *)newsock->pcb, newsock);
        tcp_recv((struct tcp_pcb *)newsock->pcb, sys_tcp_recv_cb);
        struct thread *ta = sched_current();
        vfs_file_t *ta_fdt = THREAD_FDT(ta);
        int newfd = -1;
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
            if (!ta_fdt[i].in_use) {
                newfd = i;
                break;
            }
        }
        if (newfd < 0) {
            net_sock_free(newsock);
            return -24;
        }
        ta_fdt[newfd].in_use = 1;
        ta_fdt[newfd].ftype = VFS_FILE_TYPE_SOCKET;
        ta_fdt[newfd].sock = newsock;
        ta_fdt[newfd].ref_count = 1;
        return (int64_t)newfd;
    }

    case SYS_shutdown: {
        int sh_fd  = (int)arg1;
        int sh_how = (int)arg2;   /* SHUT_RD=0, SHUT_WR=1, SHUT_RDWR=2 */
        if (sh_fd < 0 || sh_fd >= VFS_MAX_FDS) return -9;   /* EBADF */
        vfs_file_t *shvf = &THREAD_FDT(sched_current())[sh_fd];
        if (!shvf->in_use) return -9;
        if (shvf->ftype == VFS_FILE_TYPE_SOCKET) {
            if (!shvf->sock) return -9;
            net_sock_t *shsock = shvf->sock;
            if (shsock->proto == SOCK_PROTO_TCP && shsock->pcb) {
                int shut_rx = (sh_how != 1 /* != SHUT_WR */);
                int shut_tx = (sh_how != 0 /* != SHUT_RD */);
                tcp_shutdown((struct tcp_pcb *)shsock->pcb, shut_rx, shut_tx);
            }
            /* UDP/RAW: connectionless; shutdown is a no-op */
        } else if (shvf->ftype == VFS_FILE_TYPE_UNIX_SOCKET) {
            if (!shvf->usock) return -9;
            unix_sock_t *us = shvf->usock;
            if (sh_how != 0 /* not SHUT_RD only */ && us->peer) {
                /* Signal peer that write end is closed */
                us->state = UNIX_STATE_FREE;
                us->peer  = NULL;
            }
        } else {
            return -88;  /* ENOTSOCK */
        }
        return 0;
    }

    case SYS_setsockopt: {
        int fd         = (int)arg1;
        int level      = (int)arg2;
        int optname    = (int)arg3;
        const void *ov = (const void *)(uintptr_t)arg4;
        uint32_t olen  = (uint32_t)arg5;
        /* T-48-08: fd bounds + socket type validation */
        if (fd < 0 || fd >= VFS_MAX_FDS) return -9;          /* EBADF */
        vfs_file_t *svf = &THREAD_FDT(sched_current())[fd];
        if (!svf->in_use || svf->ftype != VFS_FILE_TYPE_SOCKET || !svf->sock)
            return -9;
        if (level != 1 /* SOL_SOCKET */)
            return -92; /* ENOPROTOOPT */
        if (optname == 20 /* SO_RCVTIMEO */) {
            /* T-48-01: validate pointer and length before memcpy */
            struct timeval_be { int64_t tv_sec; int64_t tv_usec; } tv;
            if (!ov || olen < sizeof(tv)) return -22; /* EINVAL */
            memcpy(&tv, ov, sizeof(tv));
            /* T-48-01: reject negative values or out-of-range usec */
            if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000)
                return -22;
            /* Tick conversion: 100 Hz, 10ms per tick */
            uint64_t ticks = (uint64_t)tv.tv_sec * 100 + (uint64_t)tv.tv_usec / 10000;
            /* Clamp min to 1 so "1us" does not become 0 (infinite wait) */
            if (ticks == 0 && (tv.tv_sec || tv.tv_usec)) ticks = 1;
            /* T-48-02: clamp to u32 max (~497 days) to prevent deadline overflow */
            svf->sock->rcvtimeo_ticks = (uint32_t)(ticks > 0xffffffffULL ? 0xffffffffULL : ticks);
            return 0;
        }
        return -92; /* ENOPROTOOPT for anything else this phase */
    }

    case SYS_getsockopt: {
        /* D-07: stub — BusyBox does not use getsockopt in the applets in scope */
        return -92; /* ENOPROTOOPT */
    }

    case SYS_socketpair: {
        int sp_domain = (int)arg1;
        int sp_type   = (int)arg2;
        int *sv       = (int *)arg4;  /* arg4 = sockfds[] output */
        if (sp_domain != 1 || sp_type != 1) return -93; /* only AF_UNIX SOCK_STREAM */
        unix_sock_t *spa = NULL, *spb = NULL;
        if (unix_sock_socketpair(&spa, &spb) < 0) return -24;
        vfs_file_t *sp_fdt = THREAD_FDT(sched_current());
        int fd0 = -1, fd1 = -1;
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS && (fd0 < 0 || fd1 < 0); i++) {
            if (!sp_fdt[i].in_use) { if (fd0 < 0) fd0 = i; else fd1 = i; }
        }
        if (fd0 < 0 || fd1 < 0) { unix_sock_free(spa); unix_sock_free(spb); return -24; }
        sp_fdt[fd0].in_use = sp_fdt[fd1].in_use = 1;
        sp_fdt[fd0].ftype  = sp_fdt[fd1].ftype  = VFS_FILE_TYPE_UNIX_SOCKET;
        sp_fdt[fd0].usock  = spa; sp_fdt[fd1].usock = spb;
        sp_fdt[fd0].ref_count = sp_fdt[fd1].ref_count = 1;
        sp_fdt[fd0].flags  = sp_fdt[fd1].flags  = 0;
        sp_fdt[fd0].ops    = sp_fdt[fd1].ops    = NULL;
        sv[0] = fd0; sv[1] = fd1;
        return 0;
    }

    default:
        return SYSCALL_DISPATCH_UNHANDLED;
    }
}

/**
 * proc_close_all_sockets() - Close all open socket file descriptors for a thread.
 * @t: Thread whose sockets should be closed.
 *
 * Called directly from sched_exit_current() on every exit path (normal
 * SYS_exit, page faults, signals).  signal_dispatch() may also call it before
 * sched_exit_current(); the second call is harmless because freed fds have
 * in_use=0 and are silently skipped.
 *
 * net_sock_free() handles PCB teardown internally (raw_remove/udp_remove/
 * tcp_abort + null pcb before in_use=0).
 */
void proc_close_all_sockets(struct thread *t)
{
    if (!t)
        return;
    vfs_file_t *fdt = THREAD_FDT(t);
    for (int fd = 0; fd < VFS_MAX_FDS; fd++) {
        vfs_file_t *f = &fdt[fd];
        if (!f->in_use) continue;
        if (f->ftype == VFS_FILE_TYPE_SOCKET && f->sock) {
            net_sock_free(f->sock);
            f->sock   = NULL;
            f->in_use = 0;
            f->ftype  = 0;
        } else if (f->ftype == VFS_FILE_TYPE_UNIX_SOCKET && f->usock) {
            unix_sock_close(f->usock);
            f->usock  = NULL;
            f->in_use = 0;
            f->ftype  = 0;
        }
    }
}
