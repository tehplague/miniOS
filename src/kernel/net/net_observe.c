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

#include <miniOS/net/net_observe.h>
#include <miniOS/net/ethernet.h>
#include <miniOS/net/net_config.h>
#include <miniOS/fs/sysfs.h>
#include <miniOS/io.h>
#include <string.h>

#ifndef MINI_OS_DEBUG_NET_OBSERVE
#define MINI_OS_DEBUG_NET_OBSERVE 0
#endif

#if MINI_OS_DEBUG_NET_OBSERVE
#define NET_OBSERVE_DEBUG_PRINT(...) printk(__VA_ARGS__)
#else
#define NET_OBSERVE_DEBUG_PRINT(...) do { } while (0)
#endif

static netdev_t *g_eth0_netdev = NULL;

/* -----------------------------------------------------------------------
 * Sysfs show callbacks
 * --------------------------------------------------------------------- */

static int eth0_address_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "not available\n");
    return snprintf(buf, bufsiz, "%02x:%02x:%02x:%02x:%02x:%02x\n",
                    g_eth0_netdev->mac[0], g_eth0_netdev->mac[1],
                    g_eth0_netdev->mac[2], g_eth0_netdev->mac[3],
                    g_eth0_netdev->mac[4], g_eth0_netdev->mac[5]);
}

static int eth0_link_state_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "down\n");
    return snprintf(buf, bufsiz, "%s\n", g_eth0_netdev->link_up ? "up" : "down");
}

static int eth0_rx_packets_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.rx_packets);
}

static int eth0_tx_packets_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.tx_packets);
}

static int eth0_rx_dropped_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.rx_dropped);
}

static int eth0_tx_dropped_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.tx_dropped);
}

static int eth0_rx_bytes_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.rx_bytes);
}

static int eth0_tx_bytes_show(char *buf, uint32_t bufsiz) {
    if (!g_eth0_netdev) return snprintf(buf, bufsiz, "0\n");
    return snprintf(buf, bufsiz, "%llu\n",
                    (unsigned long long)g_eth0_netdev->stats.tx_bytes);
}

static int eth0_inet_addr_show(char *buf, uint32_t bufsiz) {
    const net_ipv4_config_t *cfg = net_config_get_active();
    return snprintf(buf, bufsiz, "%d.%d.%d.%d\n",
                    cfg->address[0], cfg->address[1],
                    cfg->address[2], cfg->address[3]);
}

/* -----------------------------------------------------------------------
 * RX handler — forwards device-received frames to net_observe_rx()
 * --------------------------------------------------------------------- */

static void net_observe_rx_handler(netdev_t *dev, const void *frame,
                                    size_t frame_len, void *ctx) {
    (void)ctx;
    net_observe_rx(dev, frame, frame_len);
}

/* -----------------------------------------------------------------------
 * Init
 * --------------------------------------------------------------------- */

void net_observe_init(void) {
    g_eth0_netdev = netdev_first();

    sysfs_node_t *class_dir = sysfs_create_dir(g_sysfs_root, "class");
    if (!class_dir) return;
    sysfs_node_t *net_dir = sysfs_create_dir(class_dir, "net");
    if (!net_dir) return;
    sysfs_node_t *eth0_dir = sysfs_create_dir(net_dir, "eth0");
    if (!eth0_dir) return;

    sysfs_create_file(eth0_dir, "address",    NULL, 0, eth0_address_show);
    sysfs_create_file(eth0_dir, "link_state", NULL, 0, eth0_link_state_show);
    sysfs_create_file(eth0_dir, "rx_packets", NULL, 0, eth0_rx_packets_show);
    sysfs_create_file(eth0_dir, "tx_packets", NULL, 0, eth0_tx_packets_show);
    sysfs_create_file(eth0_dir, "rx_dropped", NULL, 0, eth0_rx_dropped_show);
    sysfs_create_file(eth0_dir, "tx_dropped", NULL, 0, eth0_tx_dropped_show);

    sysfs_node_t *stats_dir = sysfs_create_dir(eth0_dir, "statistics");
    if (stats_dir) {
        sysfs_create_file(stats_dir, "rx_bytes", NULL, 0, eth0_rx_bytes_show);
        sysfs_create_file(stats_dir, "tx_bytes", NULL, 0, eth0_tx_bytes_show);
    }
    sysfs_create_file(eth0_dir, "inet_addr", NULL, 0, eth0_inet_addr_show);

    if (!g_eth0_netdev) return;

    NET_OBSERVE_DEBUG_PRINT("net: eth0 mac=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
                            g_eth0_netdev->mac[0], g_eth0_netdev->mac[1],
                            g_eth0_netdev->mac[2], g_eth0_netdev->mac[3],
                            g_eth0_netdev->mac[4], g_eth0_netdev->mac[5],
                            g_eth0_netdev->link_up ? "up" : "down");

    /* Register observability RX handler (overridden later by lwip_port_attach) */
    g_eth0_netdev->rx_handler     = net_observe_rx_handler;
    g_eth0_netdev->rx_handler_ctx = NULL;

    /* TX probe: send one minimal broadcast frame to verify the TX path */
    if (g_eth0_netdev->ops && g_eth0_netdev->ops->xmit) {
        uint8_t probe[ETH_MIN_FRAME_LEN];
        memset(probe, 0, sizeof(probe));
        memset(probe, 0xff, 6);               /* broadcast dst */
        memcpy(probe + 6, g_eth0_netdev->mac, 6); /* src = our MAC */
        probe[12] = 0x90; probe[13] = 0x00;   /* ethertype 0x9000 = test probe */
        if (g_eth0_netdev->ops->xmit(g_eth0_netdev, probe, sizeof(probe)) == 0) {
            g_eth0_netdev->stats.tx_packets++;
            NET_OBSERVE_DEBUG_PRINT("net: eth0 tx probe\n");
        }
    }
}

/* -----------------------------------------------------------------------
 * RX path — classify, count, drop short frames
 * --------------------------------------------------------------------- */

int net_observe_rx(netdev_t *dev, const void *frame, size_t frame_len) {
    (void)dev;

    if (!g_eth0_netdev) return 0;

    if (frame_len < ETH_HEADER_LEN) {
        g_eth0_netdev->stats.rx_dropped++;
        return 0;
    }

    g_eth0_netdev->stats.rx_packets++;
    g_eth0_netdev->stats.rx_bytes += frame_len;

    /* Classify ethertype for logging (verification-only, no protocol stack) */
    const uint8_t *bytes = (const uint8_t *)frame;
    uint16_t ethertype = (uint16_t)(((uint16_t)bytes[12] << 8) | bytes[13]);
    if (ethertype == ETHERTYPE_ARP)
        NET_OBSERVE_DEBUG_PRINT("net: eth0 rx ARP frame\n");
    else if (ethertype == ETHERTYPE_IPV4)
        NET_OBSERVE_DEBUG_PRINT("net: eth0 rx IPv4 frame\n");

    return 0;
}

/* -----------------------------------------------------------------------
 * TX path — reject undersized frames, count valid ones
 * --------------------------------------------------------------------- */

int net_observe_tx(netdev_t *dev, const void *frame, size_t frame_len) {
    (void)dev;
    (void)frame;

    if (frame_len < ETH_MIN_FRAME_LEN)
        return -1;

    if (g_eth0_netdev)
        g_eth0_netdev->stats.tx_packets++;

    return 0;
}
