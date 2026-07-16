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

#ifndef _MINIOS_DRIVERS_ATA_H_
#define _MINIOS_DRIVERS_ATA_H_

#include <miniOS/types.h> 


/* ATA I/O base ports */
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* IRQ lines */
#define ATA_PRIMARY_IRQ     14
#define ATA_SECONDARY_IRQ   15

/* Drive select patterns */
#define ATA_MASTER          0xA0
#define ATA_SLAVE           0xB0

/* ATA register offsets from base port */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01    /* read */
#define ATA_REG_FEATURES    0x01    /* write */
#define ATA_REG_SECCOUNT0   0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_STATUS      0x07    /* read */
#define ATA_REG_COMMAND     0x07    /* write */

/* ATA status register bits */
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

/* Bus Mastering DMA register offsets from BMR base port */
#define BMR_CMD             0x00    /* bit 0 = start/stop, bit 3 = dir (0=read) */
#define BMR_STATUS          0x02    /* bit 0 = active, bit 1 = error, bit 2 = IRQ */
#define BMR_PRDT            0x04    /* 32-bit physical address of PRD table */

/* ATA and DMA size constants */
#define SECTOR_SIZE         512    /* bytes per ATA sector */
#define MAX_SECTORS_PER_RW  8     /* 8 x 512 B = 4 KiB bounce buffer */

/* ATA commands */
#define ATA_CMD_READ_DMA_EXT  0x25  /* READ DMA EXT (48-bit LBA) */
#define ATA_CMD_WRITE_DMA_EXT  0x35  /* WRITE DMA EXT (48-bit LBA) */
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA /* CACHE FLUSH EXT — flushes write-back cache */

/* PRD table entry flag */
#define PRD_LAST_ENTRY      0x8000  /* bit 15 = last entry in PRD table */

/* BMR_STATUS bit masks */
#define BMR_ACTIVE_BIT      0x01   /* DMA transfer in progress */
#define BMR_ERROR_BIT       0x02   /* DMA error occurred */
#define BMR_IRQ_BIT         0x04   /* IRQ fired (DMA complete) */
#define BMR_START_BIT       0x01   /* BMR_CMD bit 0: start DMA */

/* Physical Region Descriptor entry — 8 bytes, 4-byte aligned */
typedef struct {
    uint32_t phys_addr;     /* physical base of transfer buffer */
    uint16_t byte_count;    /* 0 means 65536 bytes */
    uint16_t flags;         /* bit 15 = 1 -> last entry in table */
} __attribute__((packed)) prd_entry_t;

/**
 * ata_init() - Initialise the ATA driver via PCI Bus Mastering DMA.
 *
 * Scans PCI configuration space (bus 0, devices 0-31, function 0) for a
 * PCI IDE controller (class 0x01, subclass 0x01). Reads the Bus Master
 * Register (BMR) base from BAR4. Allocates a 4 KiB DMA bounce buffer at
 * the fixed virtual address 0xFFFF830000000000 (PML4 slot 262). Sets up a
 * single Physical Region Descriptor (PRD) entry pointing to the bounce
 * buffer's physical address.
 *
 * @return: 0 on success, -1 if no PCI IDE controller was found.
 */
int ata_init(void);

/**
 * ata_read_sectors() - Read sectors from the primary master ATA drive via DMA.
 * @lba: 48-bit Logical Block Address of the first sector to read.
 * @count: Number of 512-byte sectors to read. Maximum 8 (4 KiB bounce buffer limit).
 * @buf: Destination buffer. Must be at least @count * 512 bytes.
 *
 * Programs the ATA controller with 48-bit LBA addressing, starts a Bus
 * Mastering DMA read into the bounce buffer at 0xFFFF830000000000, waits
 * for the DMA IRQ (polling BMR_STATUS bit 2), then copies from bounce
 * buffer to @buf.
 *
 * Context: Polling-based (no interrupts used for synchronisation in this
 *          implementation). Not ISR-safe.
 * @return: 0 on success, -1 on DMA error (BMR status error bit set) or
 *          ATA drive error (status register ERR bit set).
 */
int ata_read_sectors(uint64_t lba, uint16_t count, void *buf);

/**
 * ata_write_sectors() - Write sectors to the primary master ATA drive via DMA.
 * @lba: 48-bit Logical Block Address of the first sector to write.
 * @count: Number of 512-byte sectors to write. Maximum 8 (4 KiB bounce buffer limit).
 * @buf: Source buffer. Must be at least @count * 512 bytes.
 *
 * Copies @buf into the DMA bounce buffer, programs the ATA controller with
 * 48-bit LBA, starts a Bus Mastering DMA write (BMR_CMD direction bit 3 = 1),
 * issues WRITE DMA EXT (0x35), waits for DMA completion, then stops DMA.
 *
 * Context: Polling-based. Not ISR-safe.
 * @return: 0 on success, -1 on DMA or ATA error.
 */
int ata_write_sectors(uint64_t lba, uint16_t count, const void *buf);

/**
 * ata_flush() - Flush the ATA drive's write-back cache to stable storage.
 *
 * Issues CACHE FLUSH EXT command (0xEA) to the primary master ATA drive.
 * Waits for BSY=0 after issuing the command. Required after write sequences
 * to guarantee persistence across power-off.
 *
 * @return: 0 on success, -1 on timeout or drive error.
 */
int ata_flush(void);

#endif /* _MINIOS_DRIVERS_ATA_H_ */
