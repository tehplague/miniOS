#ifndef MINIOS_BLOCK_LAYER_H
#define MINIOS_BLOCK_LAYER_H

#include <miniOS/types.h>

/**
 * @file block_layer.h
 * @defgroup block_layer Block Layer
 * @brief A simple, cached block layer that abstracts the underlying ATA driver.
 *
 * Provides a block-oriented read/write interface with a write-through cache.
 * Currently only supports single-block operations when caching is active.
 * @{
 */

/**
 * block_read() - Read one or more blocks from the block device.
 * @lba: The logical block address to start reading from.
 * @count: The number of blocks to read.
 * @buf: A buffer large enough to hold `count` blocks.
 *
 * @brief Reads `count` blocks starting at `lba` into `buf`. If `count` is 1,
 * this operation will be serviced by the cache. Multi-block reads currently
 * bypass the cache and read directly from the disk.
 *
 * @return 0 on success, negative error code on failure.
 */
int block_read(uint64_t lba, uint16_t count, void *buf);

/**
 * block_write() - Write one or more blocks to the block device.
 * @lba: The logical block address to start writing to.
 * @count: The number of blocks to write.
 * @buf: A buffer containing the data to write.
 *
 * @brief Writes `count` blocks starting at `lba`. This is a write-through
 * operation. The data is written to the underlying disk, and if a cached
 * copy of the block exists, it is updated.
 *
 * @return 0 on success, negative error code on failure.
 */
int block_write(uint64_t lba, uint16_t count, const void *buf);

/**
 * block_layer_init() - Initializes the block layer and its cache.
 * @brief Called once during kernel startup to allocate memory for the cache
 * and prepare the block layer for use.
 */
void block_layer_init(void);

/** @} */

#endif // MINIOS_BLOCK_LAYER_H
