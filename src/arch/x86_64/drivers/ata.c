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

#include <miniOS/drivers/ata.h>
#include <miniOS/drivers/pci.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/drivers/console.h>
#include <miniOS/io.h>
#include <miniOS/types.h>

/* ---------- Driver state ------------------------------------------------ */

static uint16_t      g_bmr_base  = 0;
static uint8_t      *g_dma_buf   = NULL;
static uint32_t      g_dma_phys  = 0;

/* Single PRD entry in .bss (4-byte-aligned by default for struct) */
static prd_entry_t   g_prd[1];

/* ---------- Busy-wait helpers ------------------------------------------- */

/* 400 ns delay: read alternate status 4 times (each I/O ~100 ns). */
static inline void ata_delay_400ns(void) {
    (void)inb(ATA_PRIMARY_CTRL);
    (void)inb(ATA_PRIMARY_CTRL);
    (void)inb(ATA_PRIMARY_CTRL);
    (void)inb(ATA_PRIMARY_CTRL);
}

/* Poll alternate-status until BSY clears AND DRDY is set.
 * DRDY=1 is required before issuing a new command; BSY=0 alone is not
 * sufficient — the drive may not be ready for a new command yet. */
static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRIMARY_CTRL);
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRDY))
            return 0;
    }
    printk("ATA: wait_ready timeout, alt_status=0x%x\n",
           (uint32_t)inb(ATA_PRIMARY_CTRL));
    return -1;
}

/* Poll BMR status until active bit clears (or error bit sets).
 * Returns 0 on completion, -1 on error/timeout. */
static int ata_wait_dma(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb((int)(g_bmr_base + BMR_STATUS));
        if (st & BMR_ERROR_BIT) {    /* DMA error */
            printk("ATA: DMA error bit set, bmr_status=0x%x ata_status=0x%x\n",
                   (uint32_t)st, (uint32_t)inb(ATA_PRIMARY_BASE + ATA_REG_STATUS));
            return -1;
        }
        if (!(st & BMR_ACTIVE_BIT)) /* active cleared — done */
            return 0;
    }
    printk("ATA: DMA timeout, bmr_status=0x%x ata_status=0x%x\n",
           (uint32_t)inb((int)(g_bmr_base + BMR_STATUS)),
           (uint32_t)inb(ATA_PRIMARY_BASE + ATA_REG_STATUS));
    return -1;
}

/* ---------- ata_init ----------------------------------------------------- */

/**
 * ata_init() - Locate IDE controller in pci_devices[] and set up DMA infrastructure.
 *
 * Searches pci_devices[] (populated by pci_init()) for class=0x01 subclass=0x01
 * (IDE controller). Reads BAR4 for Bus Master Register base port (g_bmr_base).
 * Enables PCI bus mastering via command register bit 2. Maps the DMA bounce
 * buffer: vmm_map_page(0xFFFF830000000000, pmm_alloc_frame(), PAGE_PRESENT|PAGE_WRITE).
 * Stores bounce buffer kernel VA in g_dma_buf and physical address in g_dma_phys.
 * Sets up g_prd[0] with phys_addr=g_dma_phys, byte_count=4096, flags=0x8000 (last entry).
 *
 * Must be called after pci_init().
 * @return: 0 on success, -1 if no PCI IDE controller found in pci_devices[].
 */
int ata_init(void) {
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];
        if (d->class_code != 0x01 || d->subclass != 0x01)
            continue;

        /* BAR4 already read during PCI enumeration; bit 0 = I/O space */
        uint32_t bar4 = d->bar[4];
        if (!(bar4 & 0x1)) {
            printk("ATA: BAR4 is not I/O space, skipping\n");
            continue;
        }
        g_bmr_base = (uint16_t)(bar4 & ~0x3u);

        /* Enable Bus Mastering in PCI command register (bit 2) */
        uint32_t cmd = pci_config_read32(d->bus, d->dev, d->fn, 0x04);
        cmd |= (1u << 2);
        pci_config_write32(d->bus, d->dev, d->fn, 0x04, cmd);

        printk("ATA: found IDE controller at %02x:%02x.%x, BMR base=0x%x\n",
               (uint32_t)d->bus, (uint32_t)d->dev, (uint32_t)d->fn,
               (uint32_t)g_bmr_base);

        /* Allocate and map 4 KiB DMA bounce buffer */
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            printk("ATA: OOM allocating DMA buffer\n");
            return -1;
        }
        if (vmm_map_page(DMA_BUFFER_VA, phys,
                         PAGE_PRESENT | PAGE_WRITE) < 0) {
            printk("ATA: vmm_map_page failed for DMA buffer\n");
            pmm_free_frame(phys);
            return -1;
        }
        g_dma_buf  = (uint8_t *)DMA_BUFFER_VA;
        g_dma_phys = (uint32_t)phys;

        return 0;
    }

    printk("ATA: no IDE controller found in PCI device list\n");
    return -1;
}

/* ---------- ata_read_sectors -------------------------------------------- */

/**
 * ata_read_sectors() - Read sectors from primary master ATA drive via Bus Mastering DMA.
 * @lba: 48-bit LBA of the first sector.
 * @count: Number of 512-byte sectors (1-8; 4 KiB bounce buffer limits to 8 max).
 * @buf: Destination buffer (at least @count * 512 bytes).
 *
 * Programs the primary ATA controller (base port 0x1F0): selects master drive
 * (0xA0), writes high and low bytes of sector count and 48-bit LBA to
 * LBA registers, issues READ DMA EXT command (0x25). Writes g_prd physical
 * address to BMR_PRDT, clears BMR_STATUS interrupt/error bits, and sets
 * BMR_CMD bit 0 to start DMA. Polls BMR_STATUS until bit 2 (IRQ) is set or
 * bit 1 (error) is set. Stops DMA via BMR_CMD. Copies from g_dma_buf to @buf.
 *
 * Context: Polling-based; not ISR-safe.
 * @return: 0 on success, -1 on DMA or ATA drive error.
 */
int ata_read_sectors(uint64_t lba, uint16_t count, void *buf) {
    if (count == 0 || count > MAX_SECTORS_PER_RW)
        return -1;
    if (g_bmr_base == 0 || g_dma_buf == NULL)
        return -1;

    /* Select device: LBA48 master. Bits 7,5 obs=1; bit 6 LBA=1; bit 4 DEV=0 → 0xE0 */
    outb(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xE0);

    /* 400 ns settling time after device select, then wait for BSY=0+DRDY=1. */
    ata_delay_400ns();
    if (ata_wait_ready() < 0)
        return -1;

    /* Send 48-bit LBA — high bytes first, then low bytes */
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, 0);                  /* count high */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0,      (lba >> 24) & 0xFF); /* LBA[31:24] */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1,      (lba >> 32) & 0xFF); /* LBA[39:32] */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2,      (lba >> 40) & 0xFF); /* LBA[47:40] */
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, count & 0xFF);       /* count low  */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0,      lba & 0xFF);         /* LBA[7:0]   */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1,      (lba >>  8) & 0xFF); /* LBA[15:8]  */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2,      (lba >> 16) & 0xFF); /* LBA[23:16] */

    /* Set up PRD table */
    g_prd[0].phys_addr  = g_dma_phys;
    g_prd[0].byte_count = (uint16_t)(count * SECTOR_SIZE);
    g_prd[0].flags      = PRD_LAST_ENTRY;

    /* Write physical address of PRD table to BMR_PRDT.
     * g_prd is in .bss at a higher-half VA; subtract KERNEL_VMA for phys. */
    uint32_t prd_phys = (uint32_t)((uint64_t)g_prd - KERNEL_VMA);
    outl((int)(g_bmr_base + BMR_PRDT), prd_phys);

    /* Clear error and IRQ status bits */
    outb((int)(g_bmr_base + BMR_STATUS), BMR_ERROR_BIT | BMR_IRQ_BIT);

    /* Issue READ DMA EXT command */
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_READ_DMA_EXT);

    /* 400 ns delay after command write before starting DMA engine. */
    ata_delay_400ns();

    /* Start DMA: bit 0 = 1 (start), bit 3 = 0 (read from disk) */
    outb((int)(g_bmr_base + BMR_CMD), BMR_START_BIT);

    /* Poll until DMA completes */
    int err = ata_wait_dma();

    /* Stop DMA */
    outb((int)(g_bmr_base + BMR_CMD), 0x00);

    if (err < 0)
        return -1;

    /* Wait for ATA drive to clear BSY */
    for (int i = 0; i < 10000; i++) {
        uint8_t st = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY))
            break;
    }

    /* Copy from bounce buffer to caller's buffer */
    uint8_t *dst = (uint8_t *)buf;
    uint32_t nbytes = (uint32_t)count * SECTOR_SIZE;
    for (uint32_t i = 0; i < nbytes; i++)
        dst[i] = g_dma_buf[i];

    return 0;
}

/* ---------- ata_write_sectors ------------------------------------------- */

/**
 * ata_write_sectors() - Write sectors to the primary master ATA drive via DMA.
 * @lba: 48-bit LBA of the first sector.
 * @count: Number of 512-byte sectors (1-8; 4 KiB bounce buffer limits to 8 max).
 * @buf: Source buffer (at least @count * 512 bytes).
 *
 * Copies @buf into the DMA bounce buffer (write direction: caller → g_dma_buf).
 * Programs the ATA controller with 48-bit LBA, sets BMR direction bit 3=1 (write),
 * issues WRITE DMA EXT (0x35). Polls BMR_STATUS until completion.
 *
 * Context: Polling-based; not ISR-safe.
 * @return: 0 on success, -1 on DMA or ATA drive error.
 */
int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf) {
    if (count == 0 || count > MAX_SECTORS_PER_RW)
        return -1;
    if (g_bmr_base == 0 || g_dma_buf == NULL)
        return -1;

    /* Copy source buffer into DMA bounce buffer (write: caller → DMA buf) */
    uint32_t nbytes = (uint32_t)count * SECTOR_SIZE;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < nbytes; i++)
        g_dma_buf[i] = src[i];

    /* Select device: LBA48 master */
    outb(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xE0);
    ata_delay_400ns();
    if (ata_wait_ready() < 0)
        return -1;

    /* Send 48-bit LBA — high bytes first, then low bytes */
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, 0);                  /* count high */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0,      (lba >> 24) & 0xFF); /* LBA[31:24] */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1,      (lba >> 32) & 0xFF); /* LBA[39:32] */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2,      (lba >> 40) & 0xFF); /* LBA[47:40] */
    outb(ATA_PRIMARY_BASE + ATA_REG_SECCOUNT0, count & 0xFF);       /* count low  */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA0,      lba & 0xFF);         /* LBA[7:0]   */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA1,      (lba >>  8) & 0xFF); /* LBA[15:8]  */
    outb(ATA_PRIMARY_BASE + ATA_REG_LBA2,      (lba >> 16) & 0xFF); /* LBA[23:16] */

    /* Set up PRD table */
    g_prd[0].phys_addr  = g_dma_phys;
    g_prd[0].byte_count = (uint16_t)(count * SECTOR_SIZE);
    g_prd[0].flags      = PRD_LAST_ENTRY;

    /* Write PRD physical address to BMR */
    uint32_t prd_phys = (uint32_t)((uint64_t)g_prd - KERNEL_VMA);
    outl((int)(g_bmr_base + BMR_PRDT), prd_phys);

    /* Clear error and IRQ status bits */
    outb((int)(g_bmr_base + BMR_STATUS), BMR_ERROR_BIT | BMR_IRQ_BIT);

    /* Issue WRITE DMA EXT command */
    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA_EXT);
    ata_delay_400ns();

    /* Start DMA: bit 0 = 1 (start), bit 3 = 1 (write to disk direction) */
    outb((int)(g_bmr_base + BMR_CMD), BMR_START_BIT | 0x08);

    /* Poll until DMA completes */
    int err = ata_wait_dma();

    /* Stop DMA */
    outb((int)(g_bmr_base + BMR_CMD), 0x00);

    if (err < 0)
        return -1;

    /* Wait for ATA drive to clear BSY */
    for (int i = 0; i < 10000; i++) {
        uint8_t st = inb(ATA_PRIMARY_BASE + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY))
            break;
    }

    return 0;
}

/* ---------- ata_flush ----------------------------------------------------- */

/**
 * ata_flush() - Issue CACHE FLUSH EXT to force write-back cache to disk.
 *
 * Waits for drive ready, issues CACHE FLUSH EXT (0xEA), then waits for BSY=0.
 * Must be called after all ext2 write operations to guarantee EXT2W-07 (data
 * survives power-off / QEMU restart).
 *
 * @return: 0 on success, -1 on timeout.
 */
int ata_flush(void) {
    if (g_bmr_base == 0)
        return -1;

    outb(ATA_PRIMARY_BASE + ATA_REG_HDDEVSEL, 0xE0);
    ata_delay_400ns();
    if (ata_wait_ready() < 0)
        return -1;

    outb(ATA_PRIMARY_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH_EXT);

    /* Wait for BSY to clear after flush */
    for (int i = 0; i < 100000; i++) {
        uint8_t st = inb(ATA_PRIMARY_CTRL);
        if (!(st & ATA_SR_BSY))
            return 0;
    }
    return -1;  /* timeout */
}
