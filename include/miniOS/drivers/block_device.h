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

#ifndef _MINIOS_DRIVERS_BLOCK_DEVICE_H_
#define _MINIOS_DRIVERS_BLOCK_DEVICE_H_

#include <miniOS/types.h>

/* Maximum number of tracked block devices (1 disk + up to 15 partitions). */
#define BLKDEV_MAX  16

#define BLKDEV_MAJOR_SATA  8   /* SCSI/ATA disk major, matching Linux convention */

typedef struct {
    char     name[16];      /* "sda", "sda1", "sda2", ... */
    uint8_t  major;
    uint8_t  minor;
    uint64_t lba_start;     /* first LBA (0 for whole-disk entry) */
    uint64_t lba_end;       /* last LBA inclusive (0 for whole-disk entry) */
    bool     is_partition;
} block_device_t;

/**
 * blkdev_init() - Clear the block device registry.
 * Called once during kernel startup before any blkdev_register() calls.
 */
void blkdev_init(void);

/**
 * blkdev_register() - Add a block device to the registry.
 * @name:         Short device name ("sda", "sda1", ...). Must be < 16 chars.
 * @major:        Device major number.
 * @minor:        Device minor number.
 * @lba_start:    First LBA of the device's data region.
 * @lba_end:      Last LBA inclusive (both 0 if not a partition).
 * @is_partition: True if this entry represents a partition.
 *
 * @return: 0 on success, -1 if the registry is full.
 */
int blkdev_register(const char *name, uint8_t major, uint8_t minor,
                    uint64_t lba_start, uint64_t lba_end, bool is_partition);

/**
 * blkdev_find() - Look up a block device by major and minor number.
 * @major: Device major number.
 * @minor: Device minor number.
 * @return: Pointer to the registry entry, or NULL if not found.
 */
const block_device_t *blkdev_find(uint8_t major, uint8_t minor);

/**
 * blkdev_get() - Get a block device by index.
 * @index: Zero-based index into the registry.
 * @return: Pointer to the entry, or NULL if @index is out of range.
 */
const block_device_t *blkdev_get(int index);

/**
 * blkdev_count() - Return the number of registered block devices.
 */
int blkdev_count(void);

#endif /* _MINIOS_DRIVERS_BLOCK_DEVICE_H_ */
