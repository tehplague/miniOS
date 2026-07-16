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
 * @file lwip_netif.c
 * @brief lwIP netif initialization and poll driver for miniOS.
 *
 * Bridges the lwip_port Ethernet seam to lwIP's netif layer.
 * Operates in NO_SYS=1 mode — no threading, no tcpip_thread.
 */

#include <miniOS/net/lwip_netif.h>
#include <miniOS/net/lwip_port.h>
#include <miniOS/net/net_config.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

static struct netif g_lwip_netif;

/**
 * @brief linkoutput callback: transmit a pbuf chain via lwip_port_tx().
 *
 * Flattens the pbuf chain into a contiguous frame buffer before TX.
 * Frames exceeding 1514 bytes (max Ethernet payload) are rejected with
 * ERR_MEM to satisfy threat T-42-02-02 (oversized frame in linkoutput).
 */
static err_t miniOS_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    /* Validate length before buffer copy — T-42-02-02 mitigation */
    if (p->tot_len > 1514)
        return ERR_MEM;

    /* Flatten pbuf chain into contiguous frame before TX */
    uint8_t frame[1514];
    uint16_t len = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        memcpy(frame + len, q->payload, q->len);
        len += q->len;
    }
    return lwip_port_tx(frame, len) == 0 ? ERR_OK : ERR_IF;
}

/**
 * @brief netif initialization callback called by netif_add().
 */
static err_t miniOS_netif_init(struct netif *netif)
{
    const net_ipv4_config_t *cfg = net_config_get_active();

    netif->name[0]    = 'e';
    netif->name[1]    = '0';
    netif->output     = etharp_output;
    netif->linkoutput = miniOS_netif_linkoutput;
    netif->hwaddr_len = ETHARP_HWADDR_LEN;
    memcpy(netif->hwaddr, cfg->mac, ETHARP_HWADDR_LEN);
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

/**
 * @brief RX callback wired into lwip_port_attach().
 *
 * Receives classified Ethernet frames from lwip_port and feeds them
 * into lwIP's ethernet_input() for ARP and IPv4 processing.
 * Unsupported frames (LWIP_PORT_FRAME_UNSUPPORTED) are silently dropped.
 *
 * T-42-02-01: drops frame when pbuf_alloc returns NULL (pool exhausted).
 * T-42-02-04: NULL ctx guard is defensive; ctx is always &g_lwip_netif.
 */
static void miniOS_lwip_rx_callback(netdev_t *dev,
                                    lwip_port_frame_kind_t kind,
                                    const void *frame,
                                    size_t frame_len,
                                    const ethernet_frame_info_t *info,
                                    void *ctx)
{
    (void)dev;
    (void)info;

    if (kind == LWIP_PORT_FRAME_UNSUPPORTED)
        return;

    /* Defensive NULL check — T-42-02-04 mitigation */
    if (!ctx)
        return;

    struct netif *netif = (struct netif *)ctx;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)frame_len, PBUF_POOL);
    if (!p)
        return;  /* pool exhausted — drop frame; T-42-02-01 mitigation */

    pbuf_take(p, frame, frame_len);

    if (netif->input(p, netif) != ERR_OK)
        pbuf_free(p);  /* input() did not consume pbuf — free to avoid leak */
}

/**
 * @brief Timeout callback wired into lwip_port_attach().
 *
 * Called by lwip_port_poll_timeouts() when the elapsed threshold is reached.
 * Drives lwIP's internal timers (TCP retransmit, ARP aging, etc.).
 */
static void miniOS_lwip_timeout_cb(uint32_t elapsed_ms, void *ctx)
{
    (void)elapsed_ms;
    (void)ctx;
    sys_check_timeouts();
}

void lwip_netif_init(void)
{
    const net_ipv4_config_t *cfg = net_config_get_active();

    ip4_addr_t ip, nm, gw, dns;
    IP4_ADDR(&ip, cfg->address[0], cfg->address[1], cfg->address[2], cfg->address[3]);
    IP4_ADDR(&nm, cfg->netmask[0], cfg->netmask[1], cfg->netmask[2], cfg->netmask[3]);
    IP4_ADDR(&gw, cfg->gateway[0], cfg->gateway[1], cfg->gateway[2], cfg->gateway[3]);
    IP4_ADDR(&dns, cfg->dns[0], cfg->dns[1], cfg->dns[2], cfg->dns[3]);

    lwip_init();  /* must be first: initializes memp pools */

    netif_add(&g_lwip_netif, &ip, &nm, &gw, NULL,
              miniOS_netif_init, ethernet_input);
    netif_set_default(&g_lwip_netif);
    dns_setserver(0, (const ip_addr_t *)&dns);
    netif_set_up(&g_lwip_netif);
    netif_set_link_up(&g_lwip_netif);

    /* Wire RX and timeout callbacks to the Ethernet seam */
    lwip_port_attach(miniOS_lwip_rx_callback, &g_lwip_netif,
                     miniOS_lwip_timeout_cb, NULL);

    printk("net: lwIP initialized, ip=%u.%u.%u.%u\n",
           cfg->address[0], cfg->address[1],
           cfg->address[2], cfg->address[3]);
}

void lwip_netif_poll(void)
{
    lwip_port_poll();
    lwip_port_poll_timeouts(10);  /* ~10ms per poll iteration */
    sys_check_timeouts();
}
