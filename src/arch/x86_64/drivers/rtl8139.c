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

#include <miniOS/drivers/rtl8139.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/io.h>
#include <miniOS/mm/heap.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/net/ethernet.h>
#include <string.h>

#define RTL8139_REG_MAC0      0x00
#define RTL8139_REG_MAR0      0x08
#define RTL8139_REG_TXSTATUS0 0x10
#define RTL8139_REG_TXADDR0   0x20
#define RTL8139_REG_RXBUF     0x30
#define RTL8139_REG_CMD       0x37
#define RTL8139_REG_CAPR      0x38
#define RTL8139_REG_CBR       0x3A
#define RTL8139_REG_IMR       0x3C
#define RTL8139_REG_ISR       0x3E
#define RTL8139_REG_TCR       0x40
#define RTL8139_REG_RCR       0x44
#define RTL8139_REG_CONFIG1   0x52

#define RTL8139_CMD_RESET     0x10
#define RTL8139_CMD_RX_ENABLE 0x08
#define RTL8139_CMD_TX_ENABLE 0x04

#define RTL8139_ISR_RX_OK     0x0001
#define RTL8139_ISR_RX_ERR    0x0002
#define RTL8139_ISR_TX_OK     0x0004
#define RTL8139_ISR_TX_ERR    0x0008

#define RTL8139_RX_STATUS_ROK 0x0001

#define RTL8139_TX_DESC_COUNT 4
#define RTL8139_RX_BUFFER_SIZE 8192
#define RTL8139_RX_PAD_SIZE   16
#define RTL8139_RX_WRAP_GUARD 2048
#define RTL8139_RX_RING_SIZE  (RTL8139_RX_BUFFER_SIZE + RTL8139_RX_PAD_SIZE + RTL8139_RX_WRAP_GUARD)

typedef struct {
    uint16_t io_base;
    uint8_t irq_line;
    uint8_t mac[ETH_ADDR_LEN];
    uint32_t tx_slot;
    uint32_t rx_offset;
    uint32_t rx_drops;
    uint32_t rx_packets;
    uint32_t tx_packets;
} rtl8139_state_t;

/* Static DMA buffers in BSS — physical address is (virt - KERNEL_VMA), always valid */
static uint8_t g_rtl8139_rx_buf[RTL8139_RX_RING_SIZE];
static uint8_t g_rtl8139_tx_buf[RTL8139_TX_DESC_COUNT][NET_MAX_FRAME_SIZE + 4];

static rtl8139_state_t g_rtl8139_state;
static netdev_t g_rtl8139_netdev;

#ifdef TEST_BUILD
static uint8_t g_test_io_space[0x10000];

static inline void rtl_out8(uint16_t port, uint8_t value) { g_test_io_space[port] = value; }
static inline void rtl_out16(uint16_t port, uint16_t value) { memcpy(&g_test_io_space[port], &value, sizeof(value)); }
static inline void rtl_out32(uint16_t port, uint32_t value) { memcpy(&g_test_io_space[port], &value, sizeof(value)); }
static inline uint8_t rtl_in8(uint16_t port) { return g_test_io_space[port]; }
static inline uint16_t rtl_in16(uint16_t port) { uint16_t value; memcpy(&value, &g_test_io_space[port], sizeof(value)); return value; }
static inline uint32_t rtl_in32(uint16_t port) { uint32_t value; memcpy(&value, &g_test_io_space[port], sizeof(value)); return value; }
#else
static inline void rtl_out8(uint16_t port, uint8_t value) { outb(port, value); }
static inline void rtl_out16(uint16_t port, uint16_t value) { outw(port, value); }
static inline void rtl_out32(uint16_t port, uint32_t value) { outl(port, value); }
static inline uint8_t rtl_in8(uint16_t port) { return inb(port); }
static inline uint16_t rtl_in16(uint16_t port) { return inw(port); }
static inline uint32_t rtl_in32(uint16_t port) { return inl(port); }
#endif

bool rtl8139_tx_length_valid(size_t frame_len)
{
    return frame_len >= ETH_MIN_FRAME_LEN && frame_len <= NET_MAX_FRAME_SIZE;
}

bool rtl8139_rx_status_valid(uint16_t status, size_t frame_len, size_t avail_len)
{
    if ((status & RTL8139_RX_STATUS_ROK) == 0)
        return false;
    if (frame_len < ETH_HEADER_LEN || frame_len > NET_MAX_FRAME_SIZE)
        return false;
    if (avail_len < frame_len)
        return false;
    return true;
}

uint32_t rtl8139_ring_advance(uint32_t ring_offset, uint32_t consumed_len, uint32_t ring_size)
{
    uint32_t advanced = ring_offset + consumed_len + 4;
    advanced = (advanced + 3U) & ~3U;
    if (advanced >= ring_size)
        advanced -= ring_size;
    return advanced;
}

static void rtl8139_reset(uint16_t io_base)
{
    rtl_out8(io_base + RTL8139_REG_CMD, RTL8139_CMD_RESET);
    for (uint32_t spins = 0; spins < 100000; spins++) {
        if ((rtl_in8(io_base + RTL8139_REG_CMD) & RTL8139_CMD_RESET) == 0)
            return;
    }
}

static int rtl8139_open(netdev_t *dev)
{
    (void)dev;
    return 0;
}

static int rtl8139_get_mac(netdev_t *dev, uint8_t mac_out[6])
{
    rtl8139_state_t *state = (rtl8139_state_t *)dev->driver_data;
    memcpy(mac_out, state->mac, ETH_ADDR_LEN);
    return 0;
}

static bool rtl8139_get_link(netdev_t *dev)
{
    (void)dev;
    return true;
}

static int rtl8139_xmit(netdev_t *dev, const void *frame, size_t frame_len)
{
    rtl8139_state_t *state = (rtl8139_state_t *)dev->driver_data;
    uint32_t slot;
    uint16_t io_base;
    uint32_t tx_status;

    if (!state || !frame || !rtl8139_tx_length_valid(frame_len))
        return -1;

    slot = state->tx_slot;
    io_base = state->io_base;
    tx_status = rtl_in32(io_base + RTL8139_REG_TXSTATUS0 + (slot * 4));
    if ((tx_status & (1U << 13)) == 0 && tx_status != 0)
        return -1;

    uint8_t *txbuf = g_rtl8139_tx_buf[slot];
    memcpy(txbuf, frame, frame_len);
    if (frame_len < 60) {
        memset(txbuf + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }
    uint32_t paddr = (uint32_t)((uint64_t)txbuf - KERNEL_VMA);
    rtl_out32(io_base + RTL8139_REG_TXADDR0 + (slot * 4), paddr);
    uint32_t addr_readback = rtl_in32(io_base + RTL8139_REG_TXADDR0 + (slot * 4));
    (void)addr_readback;

    rtl_out32(io_base + RTL8139_REG_TXSTATUS0 + (slot * 4), (uint32_t)frame_len);

    /* Poll for TX completion: OWN=1 (bit 13) set by NIC when done */
    uint32_t ts;
    uint32_t spins;
    for (spins = 0; spins < 100000; spins++) {
        ts = rtl_in32(io_base + RTL8139_REG_TXSTATUS0 + (slot * 4));
        if (ts & (1U << 13)) break;
    }
    (void)ts;

    state->tx_slot = (slot + 1U) % RTL8139_TX_DESC_COUNT;
    state->tx_packets++;
    return 0;
}

int rtl8139_poll(netdev_t *dev)
{
    rtl8139_state_t *state = (rtl8139_state_t *)dev->driver_data;
    uint16_t io_base;
    uint8_t *entry;
    uint16_t status;
    uint16_t raw_len;
    size_t frame_len;
    ethernet_frame_info_t info;

    if (!state)
        return -1;

    io_base = state->io_base;

    uint16_t isr = rtl_in16(io_base + RTL8139_REG_ISR);
    if (!(isr & RTL8139_ISR_RX_OK))
        return 0;
    rtl_out16(io_base + RTL8139_REG_ISR, RTL8139_ISR_RX_OK);

    entry = g_rtl8139_rx_buf + state->rx_offset;
    memcpy(&status, entry, sizeof(status));
    memcpy(&raw_len, entry + 2, sizeof(raw_len));
    frame_len = (size_t)(raw_len >= 4 ? raw_len - 4 : 0);

    if (!rtl8139_rx_status_valid(status, frame_len, RTL8139_RX_RING_SIZE - state->rx_offset - 4)) {
        state->rx_drops++;
        printk("rtl8139: rx drop — bad status 0x%04x len=%u\n", status, raw_len);
        state->rx_offset = rtl8139_ring_advance(state->rx_offset, raw_len, RTL8139_RX_BUFFER_SIZE);
        rtl_out16(io_base + RTL8139_REG_CAPR, (uint16_t)(state->rx_offset - 16));
        return -1;
    }

    if (!ethernet_frame_info_parse(entry + 4, frame_len, &info)) {
        state->rx_drops++;
        printk("rtl8139: rx drop — eth parse failed len=%zu\n", frame_len);
        state->rx_offset = rtl8139_ring_advance(state->rx_offset, raw_len, RTL8139_RX_BUFFER_SIZE);
        rtl_out16(io_base + RTL8139_REG_CAPR, (uint16_t)(state->rx_offset - 16));
        return -1;
    }

    state->rx_packets++;
    if (dev->rx_handler)
        dev->rx_handler(dev, entry + 4, frame_len, dev->rx_handler_ctx);

    state->rx_offset = rtl8139_ring_advance(state->rx_offset, raw_len, RTL8139_RX_BUFFER_SIZE);
    rtl_out16(io_base + RTL8139_REG_CAPR, (uint16_t)(state->rx_offset - 16));
    return 0;
}

static const netdev_ops_t g_rtl8139_ops = {
    .open = rtl8139_open,
    .poll = rtl8139_poll,
    .xmit = rtl8139_xmit,
    .get_mac = rtl8139_get_mac,
    .get_link = rtl8139_get_link,
};

int rtl8139_probe_and_init(void)
{
    rtl8139_state_t *state = &g_rtl8139_state;

    memset(state, 0, sizeof(*state));
    memset(&g_rtl8139_netdev, 0, sizeof(g_rtl8139_netdev));

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *pdev = &pci_devices[i];
        if (pdev->vendor_id != RTL8139_PCI_VENDOR_ID || pdev->device_id != RTL8139_PCI_DEVICE_ID)
            continue;

        state->io_base = (uint16_t)(pdev->bar[0] & ~0x3U);
        state->irq_line = pdev->irq_line;

        /* Enable PCI bus mastering (bit 2) so DMA to/from physical memory works */
        uint32_t pci_cmd = pci_config_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
        pci_config_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, pci_cmd | 0x04);

        rtl_out8(state->io_base + RTL8139_REG_CONFIG1, 0x00);
        rtl8139_reset(state->io_base);
        uint32_t rxbuf_phys = (uint32_t)((uint64_t)g_rtl8139_rx_buf - KERNEL_VMA);
        rtl_out32(state->io_base + RTL8139_REG_RXBUF, rxbuf_phys);
        for (uint32_t slot = 0; slot < RTL8139_TX_DESC_COUNT; slot++)
            rtl_out32(state->io_base + RTL8139_REG_TXADDR0 + (slot * 4),
                      (uint32_t)((uint64_t)g_rtl8139_tx_buf[slot] - KERNEL_VMA));
        rtl_out32(state->io_base + RTL8139_REG_RCR, 0x0000E48A);  /* RBLEN=0 (8KB ring) */
        rtl_out32(state->io_base + RTL8139_REG_TCR, 0x03000700);
        rtl_out16(state->io_base + RTL8139_REG_IMR, RTL8139_ISR_RX_OK | RTL8139_ISR_RX_ERR | RTL8139_ISR_TX_OK | RTL8139_ISR_TX_ERR);
        rtl_out16(state->io_base + RTL8139_REG_ISR, 0xFFFF);
        rtl_out8(state->io_base + RTL8139_REG_CMD, RTL8139_CMD_RX_ENABLE | RTL8139_CMD_TX_ENABLE);

        for (uint32_t mac_i = 0; mac_i < ETH_ADDR_LEN; mac_i++)
            state->mac[mac_i] = rtl_in8(state->io_base + RTL8139_REG_MAC0 + mac_i);

        g_rtl8139_netdev.mtu = 1500;
        g_rtl8139_netdev.link_up = true;
        g_rtl8139_netdev.driver_data = state;
        g_rtl8139_netdev.ops = &g_rtl8139_ops;

        printk("RTL8139: found at 0000:%02x:%02x.%u io=0x%x irq=%u\n",
               (unsigned)pdev->bus, (unsigned)pdev->dev, (unsigned)pdev->fn,
               (unsigned)state->io_base, (unsigned)state->irq_line);
        return netdev_register(&g_rtl8139_netdev);
    }

    return -1;
}

/* Diagnostic: return 1 if the RX buffer is non-empty, 0 if empty */
int rtl8139_rx_ready(void)
{
    if (!g_rtl8139_state.io_base) return -1;
    return (rtl_in8(g_rtl8139_state.io_base + RTL8139_REG_CMD) & 0x01) ? 0 : 1;
}
