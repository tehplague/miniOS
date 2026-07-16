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

#include <miniOS/drivers/virtio_net.h>
#include <miniOS/drivers/pci.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/net/netdev.h>
#include <miniOS/net/ethernet.h>
#include <miniOS/io.h>
#include <string.h>

/* ── PCI IDs ─────────────────────────────────────────────────────────────── */
#define VIRTIO_VENDOR_ID        0x1AF4u
#define VIRTIO_NET_DEVICE_ID    0x1000u

/* ── Legacy virtio I/O register offsets (BAR0 is I/O space) ─────────────── */
#define VREG_DEVICE_FEATURES    0x00u  /* 32-bit RO */
#define VREG_GUEST_FEATURES     0x04u  /* 32-bit WO */
#define VREG_QUEUE_PFN          0x08u  /* 32-bit RW: physical frame number of queue */
#define VREG_QUEUE_SIZE         0x0Cu  /* 16-bit RO: number of descriptors */
#define VREG_QUEUE_SELECT       0x0Eu  /* 16-bit WO: select queue index */
#define VREG_QUEUE_NOTIFY       0x10u  /* 16-bit WO: kick queue */
#define VREG_DEVICE_STATUS      0x12u  /*  8-bit RW: device status byte */
#define VREG_ISR_STATUS         0x13u  /*  8-bit RO, clears on read */
/* Device-specific config for virtio-net (legacy, starts at 0x14) */
#define VREG_NET_MAC            0x14u  /* 6 bytes: MAC address */
#define VREG_NET_STATUS         0x1Au  /* 16-bit RO: link status */

/* ── Virtio device status bits ───────────────────────────────────────────── */
#define VSTAT_ACKNOWLEDGE       0x01u
#define VSTAT_DRIVER            0x02u
#define VSTAT_DRIVER_OK         0x04u

/* ── Virtio-net feature bits ─────────────────────────────────────────────── */
#define VIRTIO_NET_F_MAC        (1u << 5)

/* ── Virtqueue descriptor flags ─────────────────────────────────────────── */
#define VIRTQ_DESC_F_NEXT       0x1u   /* descriptor chains to .next */
#define VIRTQ_DESC_F_WRITE      0x2u   /* device writes into buffer (RX) */

/* ── Queue geometry ──────────────────────────────────────────────────────── */
/*
 * VNET_QUEUE_SIZE must equal the device-reported QueueSize (QEMU default=256).
 * virtio legacy layout (all offsets from queue PFN*4096):
 *   desc:  16*N bytes at offset 0       → 4096 bytes  (page 0)
 *   avail:  6+2*N+2 bytes at offset 4096 → 518 bytes   (page 1, partial)
 *   used:   6+8*N+2 bytes at offset 8192 → 2054 bytes  (page 2, partial)
 * Total for N=256: 3 pages.
 */
#define VNET_QUEUE_SIZE         256u   /* must match device-reported QueueSize */
#define VNET_QUEUE_MASK         (VNET_QUEUE_SIZE - 1u)
#define VNET_QUEUE_PAGES        3u     /* 3 pages for N=256 layout */
#define VNET_RX_SLOTS           64u    /* pre-filled RX descriptors (≤ VNET_QUEUE_SIZE) */

/* ── Virtio-net header (no merge RX buf feature) ─────────────────────────── */
#define VIRTIO_NET_HDR_LEN      10u

/* ── virtqueue structures ────────────────────────────────────────────────── */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VNET_QUEUE_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VNET_QUEUE_SIZE];
    uint16_t avail_event;
} __attribute__((packed));

/* ── virtio-net header ────────────────────────────────────────────────────── */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

/* ── Per-queue state (each queue uses VNET_QUEUE_PAGES contiguous pages) ── */
typedef struct {
    struct virtq_desc  *desc;    /* virtual addr of descriptor table (page 0+) */
    struct virtq_avail *avail;   /* virtual addr of available ring (page 0) */
    struct virtq_used  *used;    /* virtual addr of used ring (page 1) */
    uint16_t last_used_idx;      /* last used.idx processed by driver */
    uint16_t next_avail;         /* next slot to fill in avail.ring */
} vnet_queue_t;

/* ── RX buffers: one per pre-filled RX slot ──────────────────────────────── */
#define VNET_RXBUF_SIZE     (VIRTIO_NET_HDR_LEN + 1518u)
static uint8_t g_rx_bufs[VNET_RX_SLOTS][VNET_RXBUF_SIZE];

/* ── TX buffer ───────────────────────────────────────────────────────────── */
static uint8_t g_tx_buf[VIRTIO_NET_HDR_LEN + NET_MAX_FRAME_SIZE];

/* ── Driver state ────────────────────────────────────────────────────────── */
typedef struct {
    uint16_t    io_base;
    uint8_t     mac[6];
    vnet_queue_t rxq;
    vnet_queue_t txq;
    uint16_t    txq_free_head;  /* next free TX descriptor index */
} vnet_state_t;

static vnet_state_t  g_vnet_state;
static netdev_t      g_vnet_netdev;

/* ── I/O helpers ─────────────────────────────────────────────────────────── */
static inline void vnet_out8(uint16_t reg, uint8_t v)  { outb((uint16_t)(g_vnet_state.io_base + reg), v); }
static inline void vnet_out16(uint16_t reg, uint16_t v){ outw((uint16_t)(g_vnet_state.io_base + reg), v); }
static inline void vnet_out32(uint16_t reg, uint32_t v){ outl((uint16_t)(g_vnet_state.io_base + reg), v); }
static inline uint8_t  vnet_in8(uint16_t reg)  { return inb((uint16_t)(g_vnet_state.io_base + reg)); }
static inline uint16_t vnet_in16(uint16_t reg) { return inw((uint16_t)(g_vnet_state.io_base + reg)); }

/* ── Virtqueue layout offsets (N=256)
 *   desc  at 0     : 16*256 = 4096 bytes  → page 0
 *   avail at 4096  : 6+2*256+2 = 520 B   → page 1 (partial)
 *   used  at 8192  : 6+8*256+2 = 2054 B  → page 2 (partial)
 * ─────────────────────────────────────────────────────────────────────── */
#define VNET_DESC_OFFSET    0u
#define VNET_AVAIL_OFFSET   (sizeof(struct virtq_desc) * VNET_QUEUE_SIZE)  /* 4096 */
#define VNET_USED_OFFSET    8192u   /* page-aligned: align(4096+520,4096)=8192 */

static int vnet_setup_queue(vnet_queue_t *q, uint16_t queue_idx)
{
    uint64_t phys = pmm_alloc_contiguous(VNET_QUEUE_PAGES);
    if (phys == 0)
        return -1;

    uint64_t virt = phys + KERNEL_VMA;
    memset((void *)virt, 0, VNET_QUEUE_PAGES * PAGE_SIZE);

    q->desc          = (struct virtq_desc  *)(virt + VNET_DESC_OFFSET);
    q->avail         = (struct virtq_avail *)(virt + VNET_AVAIL_OFFSET);
    q->used          = (struct virtq_used  *)(virt + VNET_USED_OFFSET);
    q->last_used_idx = 0;
    q->next_avail    = 0;

    vnet_out16(VREG_QUEUE_SELECT, queue_idx);
    uint16_t qsz = vnet_in16(VREG_QUEUE_SIZE);
    if (qsz == 0 || qsz != (uint16_t)VNET_QUEUE_SIZE) {
        /* device queue size must match our static layout (QEMU default=256) */
        printk("virtio-net: queue %u size=%u expected %u\n",
               (unsigned)queue_idx, (unsigned)qsz, (unsigned)VNET_QUEUE_SIZE);
        pmm_free_contiguous(phys, VNET_QUEUE_PAGES);
        return -1;
    }
    vnet_out32(VREG_QUEUE_PFN, (uint32_t)(phys >> 12));
    return 0;
}

/* Pre-fill VNET_RX_SLOTS RX descriptors and put them in the available ring. */
static void vnet_rxq_fill(void)
{
    vnet_queue_t *q = &g_vnet_state.rxq;
    for (uint16_t i = 0; i < VNET_RX_SLOTS; i++) {
        uint64_t phys = (uint64_t)g_rx_bufs[i] - KERNEL_VMA;
        q->desc[i].addr  = phys;
        q->desc[i].len   = VNET_RXBUF_SIZE;
        q->desc[i].flags = VIRTQ_DESC_F_WRITE;
        q->desc[i].next  = 0;

        q->avail->ring[i] = i;
    }
    q->avail->idx = VNET_RX_SLOTS;
    q->next_avail = VNET_RX_SLOTS;
    /* Kick RX queue so device picks up pre-filled descriptors. */
    vnet_out16(VREG_QUEUE_NOTIFY, 0);
}

/* ── netdev_ops_t implementation ─────────────────────────────────────────── */

static int vnet_open(netdev_t *dev)
{
    (void)dev;
    return 0;
}

static int vnet_get_mac(netdev_t *dev, uint8_t mac_out[6])
{
    vnet_state_t *s = (vnet_state_t *)dev->driver_data;
    memcpy(mac_out, s->mac, 6);
    return 0;
}

static bool vnet_get_link(netdev_t *dev)
{
    (void)dev;
    return true;
}

static int vnet_xmit(netdev_t *dev, const void *frame, size_t frame_len)
{
    (void)dev;
    vnet_state_t *s = &g_vnet_state;
    vnet_queue_t *q = &s->txq;

    if (!frame || frame_len == 0 || frame_len > NET_MAX_FRAME_SIZE)
        return -1;

    /* Prepend virtio_net_hdr (all-zero = no offload). */
    memset(g_tx_buf, 0, VIRTIO_NET_HDR_LEN);
    memcpy(g_tx_buf + VIRTIO_NET_HDR_LEN, frame, frame_len);
    size_t total = VIRTIO_NET_HDR_LEN + frame_len;

    uint16_t idx = s->txq_free_head;
    uint64_t phys = (uint64_t)g_tx_buf - KERNEL_VMA;

    q->desc[idx].addr  = phys;
    q->desc[idx].len   = (uint32_t)total;
    q->desc[idx].flags = 0;
    q->desc[idx].next  = 0;

    q->avail->ring[q->next_avail & VNET_QUEUE_MASK] = idx;
    q->next_avail++;
    __asm__ volatile("" ::: "memory");   /* compiler barrier before kick */
    q->avail->idx = q->next_avail;
    __asm__ volatile("" ::: "memory");

    vnet_out16(VREG_QUEUE_NOTIFY, 1);  /* TX queue = 1 */

    /* Poll for completion (OOM-safe busy wait): used.idx must advance. */
    for (uint32_t spins = 0; spins < 200000; spins++) {
        __asm__ volatile("" ::: "memory");
        if (q->used->idx != q->last_used_idx) {
            q->last_used_idx = q->used->idx;
            break;
        }
    }

    return 0;
}

static int vnet_poll(netdev_t *dev)
{
    vnet_queue_t *q = &g_vnet_state.rxq;

    while (q->used->idx != q->last_used_idx) {
        __asm__ volatile("" ::: "memory");
        uint16_t slot = q->last_used_idx & VNET_QUEUE_MASK;
        uint32_t desc_idx = q->used->ring[slot].id;
        uint32_t rx_len   = q->used->ring[slot].len;
        q->last_used_idx++;

        if (rx_len > VIRTIO_NET_HDR_LEN && rx_len <= VNET_RXBUF_SIZE &&
            desc_idx < VNET_RX_SLOTS) {
            uint8_t *frame = g_rx_bufs[desc_idx] + VIRTIO_NET_HDR_LEN;
            size_t  flen   = rx_len - VIRTIO_NET_HDR_LEN;

            ethernet_frame_info_t info;
            if (ethernet_frame_info_parse(frame, flen, &info)) {
                if (dev->rx_handler)
                    dev->rx_handler(dev, frame, flen, dev->rx_handler_ctx);
            }
        }

        /* Return descriptor to available ring. */
        q->avail->ring[q->next_avail & VNET_QUEUE_MASK] = (uint16_t)desc_idx;
        q->next_avail++;
        __asm__ volatile("" ::: "memory");
        q->avail->idx = q->next_avail;
        __asm__ volatile("" ::: "memory");
        vnet_out16(VREG_QUEUE_NOTIFY, 0);  /* kick RX queue */
    }
    return 0;
}

static const netdev_ops_t g_vnet_ops = {
    .open     = vnet_open,
    .poll     = vnet_poll,
    .xmit     = vnet_xmit,
    .get_mac  = vnet_get_mac,
    .get_link = vnet_get_link,
};

/* ── Probe + init ────────────────────────────────────────────────────────── */

int virtio_net_probe_and_init(void)
{
    vnet_state_t *s = &g_vnet_state;
    memset(s, 0, sizeof(*s));
    memset(&g_vnet_netdev, 0, sizeof(g_vnet_netdev));

    pci_device_t *pdev = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == VIRTIO_VENDOR_ID &&
            pci_devices[i].device_id == VIRTIO_NET_DEVICE_ID) {
            pdev = &pci_devices[i];
            break;
        }
    }
    if (!pdev)
        return -1;

    /* BAR0 must be an I/O bar (bit 0 set). */
    if ((pdev->bar[0] & 0x1u) == 0)
        return -1;

    s->io_base = (uint16_t)(pdev->bar[0] & ~0x3u);

    /* Enable PCI bus mastering. */
    uint32_t cmd = pci_config_read32(pdev->bus, pdev->dev, pdev->fn, 0x04);
    pci_config_write32(pdev->bus, pdev->dev, pdev->fn, 0x04, cmd | 0x04u);

    /* virtio legacy init sequence. */
    vnet_out8(VREG_DEVICE_STATUS, 0);                              /* reset */
    vnet_out8(VREG_DEVICE_STATUS, VSTAT_ACKNOWLEDGE);
    vnet_out8(VREG_DEVICE_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    /* Accept only MAC feature — disable all offloads. */
    uint32_t dev_features = inl((uint16_t)(s->io_base + VREG_DEVICE_FEATURES));
    (void)dev_features;
    vnet_out32(VREG_GUEST_FEATURES, VIRTIO_NET_F_MAC);

    /* Setup RX queue (index 0) and TX queue (index 1). */
    if (vnet_setup_queue(&s->rxq, 0) != 0)
        return -1;
    if (vnet_setup_queue(&s->txq, 1) != 0)
        return -1;

    s->txq_free_head = 0;

    vnet_out8(VREG_DEVICE_STATUS,
              VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_DRIVER_OK);

    /* Read MAC from device config space. */
    for (int i = 0; i < 6; i++)
        s->mac[i] = vnet_in8((uint16_t)(VREG_NET_MAC + i));

    /* Pre-fill RX descriptors so the device can receive immediately. */
    vnet_rxq_fill();

    g_vnet_netdev.mtu         = 1500;
    g_vnet_netdev.link_up     = true;
    g_vnet_netdev.driver_data = s;
    g_vnet_netdev.ops         = &g_vnet_ops;

    printk("virtio-net: found at 0000:%02x:%02x.%u io=0x%x\n",
           (unsigned)pdev->bus, (unsigned)pdev->dev, (unsigned)pdev->fn,
           (unsigned)s->io_base);

    return netdev_register(&g_vnet_netdev);
}
