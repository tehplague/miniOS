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

/*
 * src/kernel/fs/ext2.c — Read-only ext2 filesystem parser
 *
 * Implements: ext2_mount, ext2_readdir, ext2_lookup, ext2_read_file
 *
 * Design notes:
 *   - No heap allocation: uses a single static 4096-byte block buffer.
 *     Calls are single-threaded (no preemption during FS operations).
 *   - Regular-file reads support direct blocks, single-indirect (i_block[12]),
 *     and double-indirect (i_block[13]). Triple-indirect is not supported.
 *     Double-indirect covers files up to 12 + N + N² blocks (N = ptrs/block),
 *     sufficient for BusyBox (~1.3 MiB) with 1 KiB blocks (max ~64 MiB).
 *   - Only the first block group's BGD is supported (single-group image).
 */

#include <miniOS/fs/ext2.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/drivers/block_layer.h>
#include <miniOS/drivers/block_device.h>
#include <string.h>
#include <miniOS/io.h>
#include <miniOS/types.h>

/* -----------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */

static uint64_t g_part_start;        /* partition start in 512-byte sectors */
static uint64_t g_part_end;          /* partition end (inclusive) in 512-byte sectors */
static uint32_t g_block_size;        /* bytes per block (1024, 2048, or 4096) */
static uint32_t g_sectors_per_block; /* g_block_size / 512 */
static uint32_t g_inodes_per_group;
static uint32_t g_blocks_per_group;
static uint32_t g_inode_size;        /* 128 for rev 0; from superblock for rev 1 */
static uint32_t g_first_data_block;  /* s_first_data_block */
static uint32_t g_sb_blocks_count;   /* cached from superblock at mount time */
static uint32_t g_sb_inodes_count;   /* cached from superblock at mount time */

/* Single reusable block buffer — avoids kmalloc dependency. */
static uint8_t g_block_buf[4096];
static uint8_t g_indirect_buf[4096];
static uint8_t g_dindirect_buf[4096]; /* double-indirect L1 table */

/* EXT2_FT_* file type constants are in ext2.h */

/* -----------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Read one filesystem block into a caller-supplied buffer. */
static int read_block(uint32_t block_no, void *buf)
{
    uint64_t lba = g_part_start + (uint64_t)block_no * g_sectors_per_block;
    if (lba + g_sectors_per_block - 1 > g_part_end) {
        printk("EXT2: read_block: attempt to read past partition end (block %u)\n", block_no);
        return -1;
    }
    int r = block_read(lba, (uint16_t)g_sectors_per_block, buf);
    if (r != 0)
        printk("EXT2: read_block failed: block=%u lba=%lu\n", block_no, lba);
    return r;
}

/* Write one filesystem block from a caller-supplied buffer. */
static int write_block(uint32_t block_no, const void *buf)
{
    uint64_t lba = g_part_start + (uint64_t)block_no * g_sectors_per_block;
    if (lba + g_sectors_per_block - 1 > g_part_end) {
        printk("EXT2: write_block: attempt to write past partition end (block %u)\n", block_no);
        return -1;
    }
    return block_write(lba, (uint16_t)g_sectors_per_block, buf);
}

/* Read the inode struct for inode number `ino` (1-based) into `out`. */
static int read_inode(uint32_t ino, ext2_inode_t *out)
{
    uint8_t local_block_buf[4096];
    uint32_t grp = (ino - 1) / g_inodes_per_group;
    uint32_t idx = (ino - 1) % g_inodes_per_group;

    /* BGD table lives at the block immediately after s_first_data_block. */
    uint32_t bgd_block = g_first_data_block + 1;
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;

    /* Locate this group's BGD within the block.
     * Assumption: all BGDs fit in one block (single-group 8 MiB image). */
    ext2_bgd_t *bgd = &((ext2_bgd_t *)local_block_buf)[grp];
    uint32_t inode_table_block = bgd->bg_inode_table;

    /* Byte offset of this inode within the inode table. */
    uint32_t byte_off  = idx * g_inode_size;
    uint32_t block_off = byte_off / g_block_size;
    uint32_t in_block  = byte_off % g_block_size;

    if (read_block(inode_table_block + block_off, local_block_buf) != 0)
        return -1;

    /* Copy — truncate to ext2_inode_t size if inode_size > 128. */
    uint32_t copy_size = (g_inode_size < sizeof(ext2_inode_t))
                         ? g_inode_size : sizeof(ext2_inode_t);
    for (uint32_t i = 0; i < copy_size; i++)
        ((uint8_t *)out)[i] = local_block_buf[in_block + i];

    return 0;
}

/* Write the inode struct for inode number `ino` (1-based) from `in` to disk.
 * Reads the inode table block, patches the bytes in place, writes it back. */
static int write_inode(uint32_t ino, const ext2_inode_t *in)
{
    uint8_t local_block_buf[4096];
    uint32_t grp = (ino - 1) / g_inodes_per_group;
    uint32_t idx = (ino - 1) % g_inodes_per_group;

    uint32_t bgd_block = g_first_data_block + 1;
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = &((ext2_bgd_t *)local_block_buf)[grp];
    uint32_t inode_table_block = bgd->bg_inode_table;

    uint32_t byte_off  = idx * g_inode_size;
    uint32_t block_off = byte_off / g_block_size;
    uint32_t in_block  = byte_off % g_block_size;

    /* Read the inode table block that contains this inode */
    if (read_block(inode_table_block + block_off, local_block_buf) != 0)
        return -1;

    /* Patch the inode bytes in place */
    uint32_t copy_size = (g_inode_size < sizeof(ext2_inode_t))
                         ? g_inode_size : sizeof(ext2_inode_t);
    for (uint32_t i = 0; i < copy_size; i++)
        local_block_buf[in_block + i] = ((const uint8_t *)in)[i];

    return write_block(inode_table_block + block_off, local_block_buf);
}

/* Free one previously-allocated data block.
 * Inverse of allocate_block(): clears the bit in the block bitmap,
 * increments superblock s_free_blocks_count and BGD bg_free_blocks_count.
 * Returns 0 on success, -1 on I/O error. */
static int free_block(uint32_t block_no)
{
    uint8_t local_block_buf[4096];
    uint8_t local_bitmap_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;

    /* Read BGD to get bitmap block number */
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_block_buf;
    uint32_t bitmap_block = bgd->bg_block_bitmap;

    /* Read block bitmap into local_bitmap_buf */
    if (read_block(bitmap_block, local_bitmap_buf) != 0)
        return -1;

    /* Clear the bit: block_no is 0-based index */
    uint32_t byte = block_no / 8;
    uint8_t  bit  = (uint8_t)(block_no % 8);
    local_bitmap_buf[byte] &= ~(1u << bit);

    /* Write updated bitmap back */
    if (write_block(bitmap_block, local_bitmap_buf) != 0)
        return -1;

    /* Update superblock s_free_blocks_count (re-read for freshness) */
    if (read_block(g_first_data_block, local_block_buf) != 0)
        return -1;
    ext2_superblock_t *sb = (ext2_superblock_t *)local_block_buf;
    sb->s_free_blocks_count++;
    if (write_block(g_first_data_block, local_block_buf) != 0)
        return -1;

    /* Update BGD bg_free_blocks_count (re-read for freshness) */
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    bgd = (ext2_bgd_t *)local_block_buf;
    bgd->bg_free_blocks_count++;
    if (write_block(bgd_block, local_block_buf) != 0)
        return -1;

    return 0;
}

/**
 * ext2_truncate_inode() - Truncate a regular file to zero bytes.
 * @ino: Inode number of the file to truncate.
 *
 * Walks i_block[0..11] (direct blocks) and i_block[12..14] (indirect pointers,
 * treated as direct block references — single-group, small
 * files only use direct blocks). Frees each allocated block via free_block().
 * Clears all block pointer entries in the inode (i_block[0..14] = 0).
 * Resets i_size = 0 and i_blocks = 0. Writes the updated inode back to disk.
 *
 * Only truncates regular files (i_mode & EXT2_S_IFREG). Returns -1 for
 * directories or other non-regular types.
 *
 * @return: 0 on success, -1 on type mismatch or I/O error.
 */
int ext2_truncate_inode(uint32_t ino, uint64_t new_size)
{
    (void)new_size; /* ext2: truncate-to-zero only */
    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0)
        return -1;

    /* Only truncate regular files — never directories */
    if ((inode.i_mode & 0xF000) != EXT2_S_IFREG)
        return -1;

    /* Free all allocated direct and indirect block pointers */
    for (int i = 0; i < 15; i++) {
        if (inode.i_block[i] != 0) {
            if (free_block(inode.i_block[i]) != 0)
                return -1;
            inode.i_block[i] = 0;
        }
    }

    /* Reset size and block count */
    inode.i_size   = 0;
    inode.i_blocks = 0;

    return write_inode(ino, &inode);
}

/* Allocate one free block via first-fit linear scan of the block bitmap.
 * Updates the bitmap, superblock s_free_blocks_count, and BGD bg_free_blocks_count.
 * Returns 0 on success and sets *block_no. Returns -2 on ENOSPC, -1 on I/O error. */
static int allocate_block(uint32_t *block_no)
{
    uint8_t local_block_buf[4096];
    uint8_t local_bitmap_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;

    /* Read BGD to get bitmap block number */
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_block_buf;
    if (bgd->bg_free_blocks_count == 0)
        return -2;  /* ENOSPC */
    uint32_t bitmap_block = bgd->bg_block_bitmap;

    /* Read block bitmap into local_bitmap_buf */
    if (read_block(bitmap_block, local_bitmap_buf) != 0)
        return -1;

    /* First-fit scan: byte * 8 + bit = block index (0-based) */
    uint32_t total_bytes = g_block_size;
    for (uint32_t byte = 0; byte < total_bytes; byte++) {
        uint8_t bm = local_bitmap_buf[byte];
        if (bm == 0xFF)
            continue;  /* all bits set in this byte, skip */
        for (uint8_t bit = 0; bit < 8; bit++) {
            if ((bm & (1u << bit)) == 0) {
                /* Found free block */
                local_bitmap_buf[byte] |= (1u << bit);
                uint32_t found = byte * 8 + bit;

                /* Write updated bitmap back */
                if (write_block(bitmap_block, local_bitmap_buf) != 0)
                    return -1;

                /* Update superblock s_free_blocks_count (re-read to avoid stale data) */
                if (read_block(g_first_data_block, local_block_buf) != 0)
                    return -1;
                ext2_superblock_t *sb = (ext2_superblock_t *)local_block_buf;
                sb->s_free_blocks_count--;
                if (write_block(g_first_data_block, local_block_buf) != 0)
                    return -1;

                /* Update BGD bg_free_blocks_count (re-read to get fresh copy) */
                if (read_block(bgd_block, local_block_buf) != 0)
                    return -1;
                bgd = (ext2_bgd_t *)local_block_buf;
                bgd->bg_free_blocks_count--;
                if (write_block(bgd_block, local_block_buf) != 0)
                    return -1;

                *block_no = found;
                return 0;
            }
        }
    }
    return -2;  /* ENOSPC — no free block found */
}

/* Allocate one free inode via first-fit linear scan of the inode bitmap.
 * Skips inodes 1 and 2 (reserved: bad blocks and root). Inode numbers are
 * 1-based; bitmap bit b corresponds to inode (b + 1). bit 0 → inode 1 (reserved),
 * bit 1 → inode 2 (root, reserved), so scan starts from bit 2 (inode 3).
 * Updates the bitmap, superblock s_free_inodes_count, and BGD bg_free_inodes_count.
 * Returns 0 on success and sets *ino. Returns -2 on ENOSPC, -1 on I/O error. */
static int allocate_inode(uint32_t *ino)
{
    uint8_t local_block_buf[4096];
    uint8_t local_bitmap_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;

    /* Read BGD to get inode bitmap block number */
    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_block_buf;
    if (bgd->bg_free_inodes_count == 0)
        return -2;  /* ENOSPC */
    uint32_t inode_bitmap_block = bgd->bg_inode_bitmap;

    /* Read inode bitmap into local_bitmap_buf */
    if (read_block(inode_bitmap_block, local_bitmap_buf) != 0)
        return -1;

    /* Scan bits 2..g_inodes_per_group-1 (skip bits 0 and 1 = inodes 1 and 2) */
    uint32_t total_inodes = g_inodes_per_group;
    for (uint32_t b = 2; b < total_inodes; b++) {
        uint32_t byte = b / 8;
        uint8_t  bit  = (uint8_t)(b % 8);
        if ((local_bitmap_buf[byte] & (1u << bit)) == 0) {
            /* Found free inode at bit b → inode number = b + 1 */
            local_bitmap_buf[byte] |= (1u << bit);

            /* Write updated inode bitmap back */
            if (write_block(inode_bitmap_block, local_bitmap_buf) != 0)
                return -1;

            /* Update superblock s_free_inodes_count */
            if (read_block(g_first_data_block, local_block_buf) != 0)
                return -1;
            ext2_superblock_t *sb = (ext2_superblock_t *)local_block_buf;
            sb->s_free_inodes_count--;
            if (write_block(g_first_data_block, local_block_buf) != 0)
                return -1;

            /* Update BGD bg_free_inodes_count */
            if (read_block(bgd_block, local_block_buf) != 0)
                return -1;
            bgd = (ext2_bgd_t *)local_block_buf;
            bgd->bg_free_inodes_count--;
            if (write_block(bgd_block, local_block_buf) != 0)
                return -1;

            *ino = b + 1;
            return 0;
        }
    }
    return -2;  /* ENOSPC */
}

static int free_inode(uint32_t ino)
{
    uint8_t local_block_buf[4096];
    uint8_t local_bitmap_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;

    if (ino <= EXT2_ROOT_INO)
        return -1;

    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_block_buf;
    uint32_t inode_bitmap_block = bgd->bg_inode_bitmap;

    if (read_block(inode_bitmap_block, local_bitmap_buf) != 0)
        return -1;

    uint32_t bit_index = ino - 1;
    uint32_t byte = bit_index / 8;
    uint8_t bit = (uint8_t)(bit_index % 8);
    local_bitmap_buf[byte] &= (uint8_t)~(1u << bit);
    if (write_block(inode_bitmap_block, local_bitmap_buf) != 0)
        return -1;

    if (read_block(g_first_data_block, local_block_buf) != 0)
        return -1;
    ext2_superblock_t *sb = (ext2_superblock_t *)local_block_buf;
    sb->s_free_inodes_count++;
    if (write_block(g_first_data_block, local_block_buf) != 0)
        return -1;

    if (read_block(bgd_block, local_block_buf) != 0)
        return -1;
    bgd = (ext2_bgd_t *)local_block_buf;
    bgd->bg_free_inodes_count++;
    if (write_block(bgd_block, local_block_buf) != 0)
        return -1;

    ext2_inode_t zero_inode;
    memset(&zero_inode, 0, sizeof(zero_inode));
    return write_inode(ino, &zero_inode);
}

static uint8_t inode_vfs_type(const ext2_inode_t *inode)
{
    uint16_t type_bits = inode->i_mode & 0xF000;
    if (type_bits == EXT2_S_IFDIR) return VFS_FILE_TYPE_DIR;
    if (type_bits == EXT2_S_IFLNK) return VFS_FILE_TYPE_SYMLINK;
    if (type_bits == EXT2_S_IFCHR) return VFS_FILE_TYPE_CHAR;
    if (type_bits == EXT2_S_IFBLK) return VFS_FILE_TYPE_BLK;
    return VFS_FILE_TYPE_REG;  /* EXT2_S_IFREG or unknown */
}

/* Resolve a logical file block index to an on-disk block number.
 * Supports direct blocks, single-indirect (i_block[12]), and
 * double-indirect (i_block[13]). */
static int inode_data_block(const ext2_inode_t *inode, uint32_t block_idx,
                            uint32_t *block_no)
{
    uint32_t ptrs_per_block = g_block_size / sizeof(uint32_t);

    /* Direct blocks: i_block[0..11] */
    if (block_idx < 12) {
        *block_no = inode->i_block[block_idx];
        return 0;
    }
    block_idx -= 12;

    /* Single-indirect: i_block[12] */
    if (block_idx < ptrs_per_block) {
        if (inode->i_block[12] == 0) {
            *block_no = 0;
            return 0;
        }
        if (read_block(inode->i_block[12], g_indirect_buf) != 0)
            return -1;
        *block_no = ((uint32_t *)g_indirect_buf)[block_idx];
        return 0;
    }
    block_idx -= ptrs_per_block;

    /* Double-indirect: i_block[13] */
    if (block_idx < ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[13] == 0) {
            *block_no = 0;
            return 0;
        }
        /* L1: read the double-indirect block → table of indirect block numbers */
        if (read_block(inode->i_block[13], g_dindirect_buf) != 0)
            return -1;
        uint32_t l1_idx = block_idx / ptrs_per_block;
        uint32_t l2_idx = block_idx % ptrs_per_block;
        uint32_t indirect_block = ((uint32_t *)g_dindirect_buf)[l1_idx];
        if (indirect_block == 0) {
            *block_no = 0;
            return 0;
        }
        /* L2: read the indirect block → table of data block numbers */
        if (read_block(indirect_block, g_indirect_buf) != 0)
            return -1;
        *block_no = ((uint32_t *)g_indirect_buf)[l2_idx];
        return 0;
    }

    printk("EXT2: file too large — triple-indirect blocks not supported\n");
    return -2;
}

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * ext2_mount() - Read superblock and initialise filesystem global state.
 * @part_lba_start: LBA of partition start; stored in g_part_start.
 *
 * Reads 2 sectors at part_lba_start + 2 (superblock at 1024-byte offset).
 * Checks s_magic == 0xEF53. Sets g_block_size = 1024 << s_log_block_size,
 * g_sectors_per_block, g_inodes_per_group, g_inode_size (128 for rev 0,
 * s_inode_size for rev 1), g_first_data_block. Reads block group descriptor
 * table at block g_first_data_block+1 to obtain inode table base.
 * Single block group assumed (8 MiB image has exactly one group).
 *
 * @return: 0 on success, -1 on ATA I/O error or invalid magic.
 *
 * Validates the superblock (magic + size-vs-partition sanity) using the
 * caller-supplied range before touching any global state, so a failed call
 * never corrupts an already-mounted instance's g_part_start/g_part_end.
 */
int ext2_mount(uint64_t part_lba_start, uint64_t part_lba_end)
{
    /* Superblock is at byte 1024 from partition start.
     * With 512-byte sectors: LBA offset = 2 sectors into the partition. */
    if (block_read(part_lba_start + 2, 2, g_block_buf) != 0) {
        printk("EXT2: I/O error reading superblock\n");
        return -1;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)g_block_buf;

    if (sb->s_magic != EXT2_MAGIC) {
        printk("EXT2: bad magic 0x%x (expected 0x%x)\n",
               (unsigned)sb->s_magic, (unsigned)EXT2_MAGIC);
        return -1;
    }

    uint32_t block_size        = 1024u << sb->s_log_block_size;
    uint32_t sectors_per_block = block_size / 512;

    /* Sanity check: filesystem size vs partition size */
    uint64_t fs_blocks = sb->s_blocks_count;
    uint64_t fs_sectors = fs_blocks * sectors_per_block;
    if (part_lba_start + fs_sectors - 1 > part_lba_end) {
        printk("EXT2: filesystem size (%lu sectors) exceeds partition boundary (%lu)\n",
               fs_sectors, part_lba_end - part_lba_start + 1);
        return -1;
    }

    /* Validated — safe to commit to global state. */
    g_part_start        = part_lba_start;
    g_part_end          = part_lba_end;
    g_block_size        = block_size;
    g_sectors_per_block = sectors_per_block;
    g_inodes_per_group  = sb->s_inodes_per_group;
    g_blocks_per_group  = sb->s_blocks_per_group;
    g_inode_size        = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    g_first_data_block  = sb->s_first_data_block;
    g_sb_blocks_count   = sb->s_blocks_count;
    g_sb_inodes_count   = sb->s_inodes_count;

    printk("EXT2: mounted, block_size=%u, inodes=%u\n",
           g_block_size, sb->s_inodes_count);
    return 0;
}

/**
 * ext2_readdir() - Iterate directory entries in all direct blocks of a directory inode.
 * @dir_ino: Directory inode number.
 * @cb: Called for each valid entry (inode != 0) with name (not null-terminated),
 *      name_len, child inode, file_type, and @ud.
 * @ud: Forwarded to @cb unchanged.
 *
 * Reads each direct block (i_block[0..11]) of the directory inode. Within each
 * block, walks ext2_dirent_t records by incrementing a byte pointer by rec_len.
 * Skips entries with inode==0 (deleted or padding entries). Stops block iteration
 * when the block pointer address reaches block_end. Returns early if @cb returns
 * non-zero.
 *
 * @return: 0 when all entries visited, or first non-zero return value from @cb.
 */
int ext2_readdir(uint32_t dir_ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud)
{
    uint8_t local_block_buf[4096];
    ext2_inode_t inode;
    if (read_inode(dir_ino, &inode) != 0)
        return -1;
    if ((inode.i_mode & EXT2_S_IFDIR) != EXT2_S_IFDIR || !offset)
        return -1;

    uint64_t dir_pos = 0;

    /* Iterate over direct blocks only (i_block[0..11]). */
    for (int k = 0; k < 12; k++) {
        if (inode.i_block[k] == 0)
            break;

        if (read_block(inode.i_block[k], local_block_buf) != 0)
            return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(local_block_buf + ptr);

            /* Guard against malformed/truncated entries. */
            if (de->rec_len < 8 || (de->rec_len & 3) != 0) {
                break;
            }

            uint64_t next_pos = dir_pos + de->rec_len;
            if (next_pos <= *offset) {
                dir_pos = next_pos;
                ptr += de->rec_len;
                continue;
            }

            if (de->inode != 0 && de->name_len > 0) {
                int r = cb(de->name, de->name_len,
                           de->inode, de->file_type, ud);
                *offset = next_pos;
                if (r != 0)
                    return r;
            }

            dir_pos = next_pos;
            ptr += de->rec_len;
        }
    }

    *offset = dir_pos;
    return 0;
}

/* Callback context for ext2_lookup path traversal. */
struct find_ctx {
    const char *target; /* null-terminated component to find */
    uint32_t    found;  /* inode number if found, else 0 */
};

static int find_entry_cb(const char *name, uint8_t nlen,
                          uint32_t ino, uint8_t ftype, void *ud)
{
    (void)ftype;
    struct find_ctx *ctx = (struct find_ctx *)ud;
    const char *t = ctx->target;

    /* Compute length of null-terminated target. */
    uint8_t tlen = 0;
    while (t[tlen])
        tlen++;

    if (tlen != nlen)
        return 0;

    for (uint8_t i = 0; i < nlen; i++)
        if (name[i] != t[i])
            return 0;

    ctx->found = ino;
    return 1; /* stop iteration */
}

static int names_match(const char *name, uint8_t nlen, const char *target)
{
    uint8_t tlen = 0;
    while (target[tlen])
        tlen++;
    if (tlen != nlen)
        return 0;
    for (uint8_t i = 0; i < nlen; i++)
        if (name[i] != target[i])
            return 0;
    return 1;
}

struct dir_empty_ctx {
    int non_dot_count;
};

static int dir_empty_cb(const char *name, uint8_t nlen,
                        uint32_t ino, uint8_t ftype, void *ud)
{
    (void)ino;
    (void)ftype;
    struct dir_empty_ctx *ctx = (struct dir_empty_ctx *)ud;
    if ((nlen == 1 && name[0] == '.') ||
        (nlen == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    ctx->non_dot_count++;
    return 1;
}

static int ext2_dir_is_empty(uint32_t dir_ino)
{
    struct dir_empty_ctx ctx = {0};
    uint64_t offset = 0;
    int rc = ext2_readdir(dir_ino, &offset, dir_empty_cb, &ctx);
    if (rc < 0)
        return rc;
    return (ctx.non_dot_count == 0) ? 0 : -39;
}

static int ext2_remove_dirent(uint32_t parent_ino, const char *name,
                              uint32_t *removed_ino_out, uint8_t *removed_ftype_out)
{
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0)
        return -1;
    if ((parent_inode.i_mode & 0xF000) != EXT2_S_IFDIR)
        return -20;

    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0)
            break;
        if (read_block(parent_inode.i_block[k], g_block_buf) != 0)
            return -1;

        uint32_t ptr = 0;
        uint32_t prev_ptr = (uint32_t)-1;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0)
                break;

            if (de->inode != 0 && names_match(de->name, de->name_len, name)) {
                if (removed_ino_out) *removed_ino_out = de->inode;
                if (removed_ftype_out) *removed_ftype_out = de->file_type;
                if (prev_ptr != (uint32_t)-1) {
                    ext2_dirent_t *prev = (ext2_dirent_t *)(g_block_buf + prev_ptr);
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    de->inode = 0;
                }

                if (write_block(parent_inode.i_block[k], g_block_buf) != 0)
                    return -1;
                return 0;
            }

            prev_ptr = ptr;
            ptr += de->rec_len;
        }
    }

    return -2;
}

/**
 * ext2_lookup() - Resolve path components starting from root inode 2.
 * @path: Absolute path; must start with '/'.
 *
 * Tokenises @path on '/' using strtok-like manual scanning. For each component,
 * calls ext2_readdir with find_entry_cb to search the current directory inode
 * for a matching entry. Updates current_ino to the matched child inode and
 * continues to the next component. Returns current_ino after all components.
 *
 * @return: Final inode number (>= 2), or 0 if any component is not found.
 */
int ext2_lookup(const char *path, vfs_inode_info_t *out)
{
    uint32_t cur_ino = EXT2_ROOT_INO;
    ext2_inode_t inode;

    if (!path || !out)
        return -1;

    /* Skip leading slash(es). */
    while (*path == '/')
        path++;

    /* Empty path (or just "/") → root inode. */
    if (*path == '\0') {
        if (read_inode(cur_ino, &inode) != 0)
            return -1;
        out->inode = cur_ino;
        out->file_type = inode_vfs_type(&inode);
        out->dev = (out->file_type == VFS_FILE_TYPE_CHAR || out->file_type == VFS_FILE_TYPE_BLK)
                   ? inode.i_block[0] : 0;
        out->size = inode.i_size;
        out->mode = inode.i_mode & 0x1FF;
        return 0;
    }

    while (*path != '\0') {
        /* Extract one path component into a local null-terminated buffer. */
        char component[256];
        uint32_t clen = 0;
        while (path[clen] != '\0' && path[clen] != '/' && clen < 255) {
            component[clen] = path[clen];
            clen++;
        }
        component[clen] = '\0';
        path += clen;
        /* Skip separator. */
        while (*path == '/')
            path++;

        struct find_ctx ctx = { .target = component, .found = 0 };
        uint64_t offset = 0;
        if (ext2_readdir(cur_ino, &offset, find_entry_cb, &ctx) < 0)
            return -1;
        if (ctx.found == 0)
            return -1; /* not found */

        cur_ino = ctx.found;
    }

    if (read_inode(cur_ino, &inode) != 0)
        return -1;

    out->inode = cur_ino;
    out->file_type = inode_vfs_type(&inode);
    out->dev = (out->file_type == VFS_FILE_TYPE_CHAR || out->file_type == VFS_FILE_TYPE_BLK)
               ? inode.i_block[0] : 0;
    out->size = inode.i_size;
    out->mode = inode.i_mode & 0x1FF;
    return 0;
}

/**
 * ext2_read_file() - Copy file data from inode blocks into caller's buffer.
 * @ino: Inode number of the regular file.
 * @off: Starting byte offset within the file.
 * @buf: Destination buffer.
 * @len: Requested byte count.
 *
 * Reads the inode to get i_size and i_block[]. Clamps read to i_size.
 * Computes starting block index as off / g_block_size, byte offset within
 * block as off % g_block_size. Resolves each logical block through direct
 * pointers and the first single-indirect block, then copies the relevant
 * slice. Iterates until @len bytes are copied or EOF.
 *
 * @return: Bytes copied (0..@len), or -1 on I/O error.
 */
int ext2_read_file(uint32_t ino, uint64_t off, void *buf, uint32_t len)
{
    uint8_t local_block_buf[4096];
    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0)
        return -1;

    /* Clamp to EOF. */
    if (off >= inode.i_size)
        return 0;
    if (off + len > inode.i_size)
        len = (uint32_t)(inode.i_size - off);

    uint32_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (bytes_read < len) {
        uint64_t cur_off     = off + bytes_read;
        uint32_t block_idx   = (uint32_t)(cur_off / g_block_size);
        uint32_t in_block_off = (uint32_t)(cur_off % g_block_size);

        uint32_t avail = g_block_size - in_block_off;
        uint32_t copy  = (avail < (len - bytes_read)) ? avail : (len - bytes_read);
        uint32_t block_no = 0;
        int r = inode_data_block(&inode, block_idx, &block_no);
        if (r < 0) {
            if (r == -2)
                break;
            return -1;
        }

        if (block_no == 0) {
            /* Sparse block: hole in file — return zeros */
            for (uint32_t i = 0; i < copy; i++)
                dst[bytes_read + i] = 0;
        } else {
            if (read_block(block_no, local_block_buf) != 0)
                return -1;
            for (uint32_t i = 0; i < copy; i++)
                dst[bytes_read + i] = local_block_buf[in_block_off + i];
        }

        bytes_read += copy;
    }

    return (int)bytes_read;
}

/**
 * ext2_create() - Allocate a new inode and add a directory entry to @parent_ino.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated filename (no '/' allowed).
 * @out_ino: Set to the new inode number on success.
 *
 * Algorithm:
 * 1. Allocate a new inode number via allocate_inode().
 * 2. Initialise the inode struct: i_mode=EXT2_S_IFREG|0644, i_links_count=1,
 *    i_size=0, i_blocks=0, all i_block[]=0. Zero all other fields.
 * 3. Write the new inode to disk via write_inode().
 * 4. Insert a new ext2_dirent_t in the parent directory:
 *    a. Iterate parent directory blocks; find the last block with space.
 *       "Space" = last entry's actual size (8 + name_len rounded up to 4)
 *       is less than its rec_len (meaning the last entry was padded).
 *    b. Shrink the last entry's rec_len to its minimal size (8 + name_len
 *       aligned up to 4 bytes), then append the new entry using the freed space.
 *    c. The new entry's rec_len fills to the end of the block.
 *    d. If no space in existing blocks, allocate a new block, update i_block[k]
 *       and i_blocks in the parent inode, write parent inode back, then place
 *       the new entry at the start of the new block with rec_len=g_block_size.
 * 5. Write the modified directory block back to disk.
 *
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC (no free inodes/blocks).
 */
int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *out_ino)
{
    if (!name || !out_ino)
        return -1;

    /* Compute name length */
    uint8_t nlen = 0;
    while (name[nlen] && nlen < 255)
        nlen++;
    if (nlen == 0)
        return -1;

    /* Step 1: Allocate new inode */
    uint32_t new_ino = 0;
    int r = allocate_inode(&new_ino);
    if (r != 0)
        return r;  /* -1 or -2 */

    /* Step 2+3: Initialise and write new inode */
    ext2_inode_t new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++)
        ((uint8_t *)&new_inode)[i] = 0;
    new_inode.i_mode        = (uint16_t)(EXT2_S_IFREG | (mode ? (mode & 0777) : 0644));
    new_inode.i_links_count = 1;
    new_inode.i_size        = 0;
    new_inode.i_blocks      = 0;

    if (write_inode(new_ino, &new_inode) != 0)
        return -1;

    /* Step 4: Insert directory entry in parent */
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0)
        return -1;

    /* Minimum aligned size of a dirent for the new entry */
    uint32_t new_min_size = 8 + ((uint32_t)(nlen + 3) & ~3u);

    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0)
            break;

        if (read_block(parent_inode.i_block[k], g_block_buf) != 0)
            return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0)
                break;

            uint32_t next_ptr = ptr + de->rec_len;
            int is_last = (next_ptr >= g_block_size);

            if (is_last) {
                /* Compute actual occupied size of this last entry */
                uint32_t actual = 8 + ((uint32_t)(de->name_len + 3) & ~3u);
                if (de->inode == 0) actual = 0;  /* deleted entry: reuse it */
                uint32_t free_space = de->rec_len - actual;

                if (free_space >= new_min_size) {
                    /* Shrink last entry and place new one after it */
                    if (de->inode != 0)
                        de->rec_len = (uint16_t)actual;

                    uint32_t new_ptr = (de->inode != 0) ? (ptr + actual) : ptr;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(g_block_buf + new_ptr);
                    ne->inode     = new_ino;
                    ne->rec_len   = (uint16_t)(g_block_size - new_ptr);
                    ne->name_len  = nlen;
                    ne->file_type = EXT2_FT_REG_FILE;
                    for (uint8_t i = 0; i < nlen; i++)
                        ne->name[i] = name[i];

                    if (write_block(parent_inode.i_block[k], g_block_buf) != 0)
                        return -1;

                    *out_ino = new_ino;
                    return 0;
                }
            }
            ptr += de->rec_len;
        }
    }

    /* No space in existing directory blocks — allocate a new one */
    uint32_t new_block = 0;
    if (allocate_block(&new_block) != 0)
        return -2;

    /* Find empty i_block[] slot in parent */
    int slot = -1;
    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) {
            slot = k;
            break;
        }
    }
    if (slot < 0)
        return -2;  /* directory has 12 full direct blocks */

    /* Zero the new block before writing the entry */
    for (uint32_t i = 0; i < g_block_size; i++)
        g_block_buf[i] = 0;

    ext2_dirent_t *ne = (ext2_dirent_t *)g_block_buf;
    ne->inode     = new_ino;
    ne->rec_len   = (uint16_t)g_block_size;
    ne->name_len  = nlen;
    ne->file_type = EXT2_FT_REG_FILE;
    for (uint8_t i = 0; i < nlen; i++)
        ne->name[i] = name[i];

    if (write_block(new_block, g_block_buf) != 0)
        return -1;

    /* Update parent inode */
    parent_inode.i_block[slot] = new_block;
    parent_inode.i_blocks     += (uint32_t)(g_block_size / 512);
    if (write_inode(parent_ino, &parent_inode) != 0)
        return -1;

    *out_ino = new_ino;
    return 0;
}

/**
 * ext2_readlink() - Read the target string of a symlink inode.
 * @ino: Inode number of the symlink (i_mode must have EXT2_S_IFLNK set).
 * @buf: Output buffer. NOT null-terminated (POSIX readlink semantics).
 * @len: Maximum bytes to copy into buf.
 *
 * Fast-link (i_blocks == 0): target stored inline in i_block[0..14] bytes.
 * Block-link (i_blocks > 0): target stored in data block i_block[0].
 *
 * @return: Number of bytes copied (>= 0), or -1 on error.
 */
int ext2_readlink(uint32_t ino, char *buf, uint32_t len)
{
    if (!buf || len == 0) return -1;

    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0) return -1;
    if ((inode.i_mode & 0xF000) != EXT2_S_IFLNK) return -1;

    uint32_t target_len = inode.i_size;
    if (target_len == 0) return 0;

    uint32_t copy_len = (target_len < len) ? target_len : len;

    if (inode.i_blocks == 0) {
        /* Fast-link: target inline in i_block[] bytes */
        const char *inline_target = (const char *)inode.i_block;
        for (uint32_t i = 0; i < copy_len; i++)
            buf[i] = inline_target[i];
    } else {
        /* Block-link: target in data block i_block[0] */
        if (inode.i_block[0] == 0) return -1;
        if (read_block(inode.i_block[0], g_block_buf) != 0) return -1;
        for (uint32_t i = 0; i < copy_len; i++)
            buf[i] = g_block_buf[i];
    }

    return (int)copy_len;
}

/**
 * ext2_symlink() - Create a new symlink inode and directory entry.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated filename (no '/' allowed; max 255 chars).
 * @target: Symlink target string (null-terminated; stored as-is).
 *
 * Fast-link used when strlen(target) <= 60; target stored inline in i_block[].
 * Block-link used when strlen(target) > 60; allocates one data block.
 * Directory entry file_type set to 7 (EXT2_FT_SYMLINK).
 *
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC.
 */
int ext2_symlink(uint32_t parent_ino, const char *name, const char *target)
{
    if (!name || !target) return -1;

    uint8_t nlen = 0;
    while (name[nlen] && nlen < 255) nlen++;
    if (nlen == 0) return -1;

    uint32_t tlen = 0;
    while (target[tlen] && tlen < 4096) tlen++;
    if (tlen == 0) return -1;

    /* Allocate new inode */
    uint32_t new_ino = 0;
    int r = allocate_inode(&new_ino);
    if (r != 0) return r;

    ext2_inode_t new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++)
        ((uint8_t *)&new_inode)[i] = 0;
    new_inode.i_mode        = (uint16_t)(EXT2_S_IFLNK | 0777);
    new_inode.i_links_count = 1;
    new_inode.i_size        = tlen;

    if (tlen <= 60) {
        /* Fast-link: store inline in i_block[] */
        char *inline_dst = (char *)new_inode.i_block;
        for (uint32_t i = 0; i < tlen; i++)
            inline_dst[i] = target[i];
        new_inode.i_blocks = 0;
    } else {
        /* Block-link: allocate a data block */
        uint32_t data_block = 0;
        if (allocate_block(&data_block) != 0) return -2;
        /* Write target into block */
        for (uint32_t i = 0; i < g_block_size; i++)
            g_block_buf[i] = 0;
        for (uint32_t i = 0; i < tlen && i < g_block_size; i++)
            g_block_buf[i] = target[i];
        if (write_block(data_block, g_block_buf) != 0) return -1;
        new_inode.i_block[0] = data_block;
        new_inode.i_blocks   = g_block_size / 512;
    }

    if (write_inode(new_ino, &new_inode) != 0) return -1;

    /* Insert directory entry — reuse ext2_create's dir-entry logic.
     * Read parent, find free space, write entry with file_type=7. */
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0) return -1;

    uint32_t new_min_size = 8 + ((uint32_t)(nlen + 3) & ~3u);

    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) break;
        if (read_block(parent_inode.i_block[k], g_block_buf) != 0) return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0) break;

            uint32_t next_ptr = ptr + de->rec_len;
            int is_last = (next_ptr >= g_block_size);
            if (is_last) {
                uint32_t actual = (de->inode == 0) ? 0 : (8 + ((uint32_t)(de->name_len + 3) & ~3u));
                uint32_t free_space = de->rec_len - actual;
                if (free_space >= new_min_size) {
                    if (de->inode != 0) de->rec_len = (uint16_t)actual;
                    uint32_t new_ptr = (de->inode != 0) ? (ptr + actual) : ptr;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(g_block_buf + new_ptr);
                    ne->inode     = new_ino;
                    ne->rec_len   = (uint16_t)(g_block_size - new_ptr);
                    ne->name_len  = nlen;
                    ne->file_type = EXT2_FT_SYMLINK;
                    for (uint8_t i = 0; i < nlen; i++) ne->name[i] = name[i];
                    if (write_block(parent_inode.i_block[k], g_block_buf) != 0) return -1;
                    return 0;
                }
            }
            ptr += de->rec_len;
        }
    }

    /* No space in existing blocks — allocate new directory block */
    uint32_t new_block = 0;
    if (allocate_block(&new_block) != 0) return -2;
    int slot = -1;
    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) { slot = k; break; }
    }
    if (slot < 0) return -2;

    for (uint32_t i = 0; i < g_block_size; i++) g_block_buf[i] = 0;
    ext2_dirent_t *ne = (ext2_dirent_t *)g_block_buf;
    ne->inode     = new_ino;
    ne->rec_len   = (uint16_t)g_block_size;
    ne->name_len  = nlen;
    ne->file_type = EXT2_FT_SYMLINK;
    for (uint8_t i = 0; i < nlen; i++) ne->name[i] = name[i];
    if (write_block(new_block, g_block_buf) != 0) return -1;

    parent_inode.i_block[slot] = new_block;
    parent_inode.i_blocks     += g_block_size / 512;
    if (write_inode(parent_ino, &parent_inode) != 0) return -1;
    return 0;
}

/**
 * ext2_write_file() - Write bytes to an inode's data blocks, allocating as needed.
 * @ino: Inode number of the target regular file.
 * @off: Byte offset within the file.
 * @buf: Source data.
 * @len: Number of bytes to write.
 *
 * For each block touched by the range [off, off+len):
 *   - If the block is not yet allocated (i_block[k]==0), allocate it and zero it.
 *   - If the write is a partial block, read-modify-write (read existing, patch bytes).
 *   - If the write covers a full block, write directly without reading first.
 * Updates i_size = max(i_size, off + bytes_written). Does NOT write back the inode
 * (caller calls ext2_flush_inode then ata_flush via vfs_close).
 *
 * @return: Number of bytes written, or -1 on I/O or allocation error.
 */
int ext2_write_file(uint32_t ino, uint64_t off, const void *buf, uint32_t len)
{
    if (len == 0)
        return 0;

    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0)
        return -1;

    /* Only write to regular files */
    if ((inode.i_mode & EXT2_S_IFREG) == 0)
        return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bytes_written = 0;

    uint32_t ptrs_per_block = g_block_size / sizeof(uint32_t);

    while (bytes_written < len) {
        uint64_t cur_off      = off + bytes_written;
        uint32_t block_idx    = (uint32_t)(cur_off / g_block_size);
        uint32_t in_block_off = (uint32_t)(cur_off % g_block_size);
        uint32_t avail        = g_block_size - in_block_off;
        uint32_t to_write     = (avail < (len - bytes_written)) ? avail : (len - bytes_written);

        uint32_t block_no = 0;

        if (block_idx < 12) {
            /* Direct blocks: i_block[0..11] */
            block_no = inode.i_block[block_idx];
            if (block_no == 0) {
                if (allocate_block(&block_no) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_block_buf[i] = 0;
                if (write_block(block_no, g_block_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_block[block_idx] = block_no;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
        } else if (block_idx < 12 + ptrs_per_block) {
            /* Single-indirect: i_block[12] */
            uint32_t ind_idx = block_idx - 12;
            uint32_t ind_bno = inode.i_block[12];
            if (ind_bno == 0) {
                if (allocate_block(&ind_bno) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_indirect_buf[i] = 0;
                if (write_block(ind_bno, g_indirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_block[12] = ind_bno;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
            if (read_block(ind_bno, g_indirect_buf) != 0)
                return (bytes_written > 0) ? (int)bytes_written : -1;
            uint32_t *ind_ptrs = (uint32_t *)g_indirect_buf;
            block_no = ind_ptrs[ind_idx];
            if (block_no == 0) {
                if (allocate_block(&block_no) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_block_buf[i] = 0;
                if (write_block(block_no, g_block_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                ind_ptrs[ind_idx] = block_no;
                if (write_block(ind_bno, g_indirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
        } else if (block_idx < 12 + ptrs_per_block + ptrs_per_block * ptrs_per_block) {
            /* Double-indirect: i_block[13] */
            uint32_t di_idx = block_idx - 12 - ptrs_per_block;
            uint32_t l1_idx = di_idx / ptrs_per_block;
            uint32_t l2_idx = di_idx % ptrs_per_block;
            uint32_t di_bno = inode.i_block[13];
            if (di_bno == 0) {
                if (allocate_block(&di_bno) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_dindirect_buf[i] = 0;
                if (write_block(di_bno, g_dindirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_block[13] = di_bno;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
            if (read_block(di_bno, g_dindirect_buf) != 0)
                return (bytes_written > 0) ? (int)bytes_written : -1;
            uint32_t *di_ptrs = (uint32_t *)g_dindirect_buf;
            uint32_t ind_bno  = di_ptrs[l1_idx];
            if (ind_bno == 0) {
                if (allocate_block(&ind_bno) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_indirect_buf[i] = 0;
                if (write_block(ind_bno, g_indirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                di_ptrs[l1_idx] = ind_bno;
                if (write_block(di_bno, g_dindirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
            if (read_block(ind_bno, g_indirect_buf) != 0)
                return (bytes_written > 0) ? (int)bytes_written : -1;
            uint32_t *ind_ptrs = (uint32_t *)g_indirect_buf;
            block_no = ind_ptrs[l2_idx];
            if (block_no == 0) {
                if (allocate_block(&block_no) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                for (uint32_t i = 0; i < g_block_size; i++) g_block_buf[i] = 0;
                if (write_block(block_no, g_block_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                ind_ptrs[l2_idx] = block_no;
                if (write_block(ind_bno, g_indirect_buf) != 0)
                    return (bytes_written > 0) ? (int)bytes_written : -1;
                inode.i_blocks += (uint32_t)(g_block_size / 512);
            }
        } else {
            printk("EXT2: file too large — triple-indirect blocks not supported for writes\n");
            break;
        }

        if (in_block_off == 0 && to_write == g_block_size) {
            /* Full block write — no need to read first */
            for (uint32_t i = 0; i < g_block_size; i++)
                g_block_buf[i] = src[bytes_written + i];
        } else {
            /* Partial block — read-modify-write */
            if (read_block(block_no, g_block_buf) != 0)
                return (bytes_written > 0) ? (int)bytes_written : -1;
            for (uint32_t i = 0; i < to_write; i++)
                g_block_buf[in_block_off + i] = src[bytes_written + i];
        }

        if (write_block(block_no, g_block_buf) != 0)
            return (bytes_written > 0) ? (int)bytes_written : -1;

        bytes_written += to_write;
    }

    /* Extend i_size if write went past current file size */
    uint64_t new_end = off + bytes_written;
    if (new_end > (uint64_t)inode.i_size)
        inode.i_size = (uint32_t)new_end;

    /* Write inode back with updated i_size and i_block[] */
    if (write_inode(ino, &inode) != 0)
        return -1;

    return (int)bytes_written;
}

/**
 * ext2_flush_inode() - Write the current inode state back to disk.
 * @ino: Inode number to flush.
 *
 * Reads the inode from disk and immediately writes it back. Used by vfs_close()
 * (via ops->flush) to ensure i_size and i_block[] are durable after writes.
 * The ATA flush command is issued separately by the caller.
 *
 * @return: 0 on success, -1 on I/O error.
 */
int ext2_flush_inode(uint32_t ino)
{
    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0)
        return -1;
    return write_inode(ino, &inode);
}

/**
 * ext2_mkdir() - Create a new directory inode and add a directory entry in @parent_ino.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated directory name (no '/' allowed).
 * @out_ino: Set to the new directory inode number on success.
 *
 * Algorithm:
 * 1. Allocate a new inode and a data block.
 * 2. Initialise the new inode: i_mode=EXT2_S_IFDIR|0755, i_links_count=2
 *    (one for the parent's dirent, one for the '.' entry inside the new dir),
 *    i_size=g_block_size, i_blocks=g_block_size/512, i_block[0]=new_block.
 * 3. Write the new inode's data block with '.' (points to new_ino) and '..'
 *    (points to parent_ino) directory entries filling the full block.
 * 4. Insert a new ext2_dirent_t (file_type=EXT2_FT_DIR) in the parent directory,
 *    using the same split-and-append or new-block logic as ext2_create().
 * 5. Increment the parent inode's i_links_count (the '..' hard link).
 * 6. Increment bg_used_dirs_count in the block group descriptor.
 *
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC (no free inodes/blocks).
 */
int ext2_mkdir(uint32_t parent_ino, const char *name, uint32_t *out_ino)
{
    if (!name || !out_ino)
        return -1;

    uint8_t nlen = 0;
    while (name[nlen] && nlen < 255)
        nlen++;
    if (nlen == 0)
        return -1;

    /* Step 1: Allocate new inode and data block */
    uint32_t new_ino = 0;
    if (allocate_inode(&new_ino) != 0)
        return -2;

    uint32_t dir_block = 0;
    if (allocate_block(&dir_block) != 0)
        return -2;

    /* Step 2: Build '.' and '..' entries in the new directory block */
    for (uint32_t i = 0; i < g_block_size; i++)
        g_block_buf[i] = 0;

    /* '.' entry: rec_len = 12 (8 header + 4-byte aligned name of 1 char) */
    ext2_dirent_t *dot = (ext2_dirent_t *)g_block_buf;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    /* '..' entry: fills remainder of block */
    ext2_dirent_t *dotdot = (ext2_dirent_t *)(g_block_buf + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(g_block_size - 12);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    if (write_block(dir_block, g_block_buf) != 0)
        return -1;

    /* Step 3: Initialise and write new directory inode */
    ext2_inode_t new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++)
        ((uint8_t *)&new_inode)[i] = 0;
    new_inode.i_mode        = EXT2_S_IFDIR | 0755;
    new_inode.i_links_count = 2;  /* parent dirent + '.' */
    new_inode.i_size        = g_block_size;
    new_inode.i_blocks      = g_block_size / 512;
    new_inode.i_block[0]    = dir_block;

    if (write_inode(new_ino, &new_inode) != 0)
        return -1;

    /* Step 4: Insert dirent (EXT2_FT_DIR) in parent directory */
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0)
        return -1;

    uint32_t new_min_size = 8 + ((uint32_t)(nlen + 3) & ~3u);

    int inserted = 0;
    for (int k = 0; k < 12 && !inserted; k++) {
        if (parent_inode.i_block[k] == 0)
            break;

        if (read_block(parent_inode.i_block[k], g_block_buf) != 0)
            return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0)
                break;

            uint32_t next_ptr = ptr + de->rec_len;
            int is_last = (next_ptr >= g_block_size);

            if (is_last) {
                uint32_t actual = 8 + ((uint32_t)(de->name_len + 3) & ~3u);
                if (de->inode == 0) actual = 0;
                uint32_t free_space = de->rec_len - actual;

                if (free_space >= new_min_size) {
                    if (de->inode != 0)
                        de->rec_len = (uint16_t)actual;

                    uint32_t new_ptr = (de->inode != 0) ? (ptr + actual) : ptr;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(g_block_buf + new_ptr);
                    ne->inode     = new_ino;
                    ne->rec_len   = (uint16_t)(g_block_size - new_ptr);
                    ne->name_len  = nlen;
                    ne->file_type = EXT2_FT_DIR;
                    for (uint8_t i = 0; i < nlen; i++)
                        ne->name[i] = name[i];

                    if (write_block(parent_inode.i_block[k], g_block_buf) != 0)
                        return -1;

                    inserted = 1;
                    break;
                }
            }
            ptr += de->rec_len;
        }
    }

    if (!inserted) {
        /* No space in existing parent blocks — allocate a new one */
        uint32_t new_pblock = 0;
        if (allocate_block(&new_pblock) != 0)
            return -2;

        int slot = -1;
        for (int k = 0; k < 12; k++) {
            if (parent_inode.i_block[k] == 0) {
                slot = k;
                break;
            }
        }
        if (slot < 0)
            return -2;

        for (uint32_t i = 0; i < g_block_size; i++)
            g_block_buf[i] = 0;

        ext2_dirent_t *ne = (ext2_dirent_t *)g_block_buf;
        ne->inode     = new_ino;
        ne->rec_len   = (uint16_t)g_block_size;
        ne->name_len  = nlen;
        ne->file_type = EXT2_FT_DIR;
        for (uint8_t i = 0; i < nlen; i++)
            ne->name[i] = name[i];

        if (write_block(new_pblock, g_block_buf) != 0)
            return -1;

        parent_inode.i_block[slot] = new_pblock;
        parent_inode.i_blocks     += g_block_size / 512;
    }

    /* Step 5: Increment parent i_links_count for the '..' back-reference */
    parent_inode.i_links_count++;
    if (write_inode(parent_ino, &parent_inode) != 0)
        return -1;

    /* Step 6: Increment bg_used_dirs_count in block group descriptor */
    uint8_t local_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;
    if (read_block(bgd_block, local_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_buf;
    bgd->bg_used_dirs_count++;
    if (write_block(bgd_block, local_buf) != 0)
        return -1;

    *out_ino = new_ino;
    return 0;
}

int ext2_rmdir(uint32_t parent_ino, const char *name)
{
    if (!name || !name[0])
        return -22;
    if ((name[0] == '.' && name[1] == '\0') ||
        (name[0] == '.' && name[1] == '.' && name[2] == '\0'))
        return -22;

    struct find_ctx ctx = { .target = name, .found = 0 };
    uint64_t offset = 0;
    if (ext2_readdir(parent_ino, &offset, find_entry_cb, &ctx) < 0)
        return -1;
    if (ctx.found == 0)
        return -2;

    ext2_inode_t child_inode;
    if (read_inode(ctx.found, &child_inode) != 0)
        return -1;
    if ((child_inode.i_mode & 0xF000) != EXT2_S_IFDIR)
        return -20;

    int empty_rc = ext2_dir_is_empty(ctx.found);
    if (empty_rc != 0)
        return empty_rc;

    if (ext2_remove_dirent(parent_ino, name, NULL, NULL) != 0)
        return -1;

    for (int i = 0; i < 15; i++) {
        if (child_inode.i_block[i] != 0) {
            if (free_block(child_inode.i_block[i]) != 0)
                return -1;
            child_inode.i_block[i] = 0;
        }
    }
    child_inode.i_size = 0;
    child_inode.i_blocks = 0;
    child_inode.i_links_count = 0;
    if (write_inode(ctx.found, &child_inode) != 0)
        return -1;

    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0)
        return -1;
    if (parent_inode.i_links_count > 0)
        parent_inode.i_links_count--;
    if (write_inode(parent_ino, &parent_inode) != 0)
        return -1;

    uint8_t local_buf[4096];
    uint32_t bgd_block = g_first_data_block + 1;
    if (read_block(bgd_block, local_buf) != 0)
        return -1;
    ext2_bgd_t *bgd = (ext2_bgd_t *)local_buf;
    if (bgd->bg_used_dirs_count > 0)
        bgd->bg_used_dirs_count--;
    if (write_block(bgd_block, local_buf) != 0)
        return -1;

    return free_inode(ctx.found);
}

/* Insert an existing inode as a named directory entry in parent_ino. */
static int ext2_add_dirent(uint32_t parent_ino, const char *name,
                           uint32_t ino, uint8_t ftype)
{
    uint8_t nlen = 0;
    while (name[nlen] && nlen < 255) nlen++;
    if (nlen == 0) return -1;

    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0) return -1;

    uint32_t new_min_size = 8 + ((uint32_t)(nlen + 3) & ~3u);

    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) break;
        if (read_block(parent_inode.i_block[k], g_block_buf) != 0) return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0) break;
            uint32_t next_ptr = ptr + de->rec_len;
            if (next_ptr >= g_block_size) {
                uint32_t actual = (de->inode == 0) ? 0
                    : 8 + ((uint32_t)(de->name_len + 3) & ~3u);
                uint32_t free_space = de->rec_len - actual;
                if (free_space >= new_min_size) {
                    if (de->inode != 0) de->rec_len = (uint16_t)actual;
                    uint32_t new_ptr = (de->inode != 0) ? (ptr + actual) : ptr;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(g_block_buf + new_ptr);
                    ne->inode     = ino;
                    ne->rec_len   = (uint16_t)(g_block_size - new_ptr);
                    ne->name_len  = nlen;
                    ne->file_type = ftype;
                    for (uint8_t i = 0; i < nlen; i++) ne->name[i] = name[i];
                    if (write_block(parent_inode.i_block[k], g_block_buf) != 0) return -1;
                    return 0;
                }
            }
            ptr += de->rec_len;
        }
    }

    uint32_t new_block = 0;
    if (allocate_block(&new_block) != 0) return -2;
    int slot = -1;
    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) { slot = k; break; }
    }
    if (slot < 0) { free_block(new_block); return -2; }

    for (uint32_t i = 0; i < g_block_size; i++) g_block_buf[i] = 0;
    ext2_dirent_t *ne = (ext2_dirent_t *)g_block_buf;
    ne->inode     = ino;
    ne->rec_len   = (uint16_t)g_block_size;
    ne->name_len  = nlen;
    ne->file_type = ftype;
    for (uint8_t i = 0; i < nlen; i++) ne->name[i] = name[i];
    if (write_block(new_block, g_block_buf) != 0) return -1;

    parent_inode.i_block[slot] = new_block;
    parent_inode.i_blocks += (uint32_t)(g_block_size / 512);
    if (write_inode(parent_ino, &parent_inode) != 0) return -1;
    return 0;
}

static uint8_t ext2_mode_to_ft(uint16_t mode)
{
    switch (mode & 0xF000) {
        case EXT2_S_IFDIR: return EXT2_FT_DIR;
        case EXT2_S_IFLNK: return EXT2_FT_SYMLINK;
        case EXT2_S_IFCHR: return EXT2_FT_CHRDEV;
        case EXT2_S_IFBLK: return EXT2_FT_BLKDEV;
        default:           return EXT2_FT_REG_FILE;
    }
}

static int ext2_rename(uint32_t old_parent_ino, const char *old_name,
                       uint32_t new_parent_ino, const char *new_name)
{
    if (!old_name || !old_name[0] || !new_name || !new_name[0])
        return -22;

    struct find_ctx old_ctx = { .target = old_name, .found = 0 };
    uint64_t offset = 0;
    ext2_readdir(old_parent_ino, &offset, find_entry_cb, &old_ctx);
    if (old_ctx.found == 0) return -2;  /* ENOENT */

    ext2_inode_t old_inode;
    if (read_inode(old_ctx.found, &old_inode) != 0) return -1;
    int old_is_dir = ((old_inode.i_mode & 0xF000) == EXT2_S_IFDIR);
    uint8_t moved_ftype = ext2_mode_to_ft(old_inode.i_mode);

    /* Handle existing destination entry */
    struct find_ctx new_ctx = { .target = new_name, .found = 0 };
    offset = 0;
    ext2_readdir(new_parent_ino, &offset, find_entry_cb, &new_ctx);
    if (new_ctx.found != 0) {
        if (new_ctx.found == old_ctx.found) return 0;  /* renaming to self */
        ext2_inode_t new_inode;
        if (read_inode(new_ctx.found, &new_inode) != 0) return -1;
        int new_is_dir = ((new_inode.i_mode & 0xF000) == EXT2_S_IFDIR);
        if (old_is_dir && !new_is_dir) return -20;   /* ENOTDIR */
        if (!old_is_dir && new_is_dir) return -21;   /* EISDIR */
        if (new_is_dir) {
            int empty_rc = ext2_dir_is_empty(new_ctx.found);
            if (empty_rc != 0) return empty_rc;  /* ENOTEMPTY */
            for (int i = 0; i < 15; i++) {
                if (new_inode.i_block[i] != 0) free_block(new_inode.i_block[i]);
            }
        } else {
            for (int i = 0; i < 15; i++) {
                if (new_inode.i_block[i] != 0) free_block(new_inode.i_block[i]);
            }
        }
        new_inode.i_size = 0; new_inode.i_links_count = 0;
        write_inode(new_ctx.found, &new_inode);
        free_inode(new_ctx.found);
        if (ext2_remove_dirent(new_parent_ino, new_name, NULL, NULL) != 0)
            return -1;
    }

    uint32_t moved_ino = 0;
    uint8_t  dummy_ft  = 0;
    if (ext2_remove_dirent(old_parent_ino, old_name, &moved_ino, &dummy_ft) != 0)
        return -1;
    (void)dummy_ft;

    if (ext2_add_dirent(new_parent_ino, new_name, moved_ino, moved_ftype) != 0)
        return -1;

    /* Update ".." in moved directory when crossing directories */
    if (old_is_dir && old_parent_ino != new_parent_ino) {
        ext2_inode_t moved_dir_inode;
        if (read_inode(moved_ino, &moved_dir_inode) == 0 &&
            moved_dir_inode.i_block[0] != 0 &&
            read_block(moved_dir_inode.i_block[0], g_block_buf) == 0) {
            uint32_t ptr = 0;
            while (ptr < g_block_size) {
                ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
                if (de->rec_len < 8) break;
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                    de->inode = new_parent_ino;
                    write_block(moved_dir_inode.i_block[0], g_block_buf);
                    break;
                }
                ptr += de->rec_len;
            }
        }
    }

    return 0;
}

static int ext2_unlink(uint32_t parent_ino, const char *name)
{
    if (!name || !name[0])
        return -22;

    struct find_ctx ctx = { .target = name, .found = 0 };
    uint64_t offset = 0;
    if (ext2_readdir(parent_ino, &offset, find_entry_cb, &ctx) < 0)
        return -1;
    if (ctx.found == 0)
        return -2;

    ext2_inode_t inode;
    if (read_inode(ctx.found, &inode) != 0)
        return -1;
    if ((inode.i_mode & 0xF000) == EXT2_S_IFDIR)
        return -21;  /* EISDIR */

    uint32_t removed_ino = 0;
    uint8_t removed_ftype = 0;
    int rm_rc = ext2_remove_dirent(parent_ino, name, &removed_ino, &removed_ftype);
    if (rm_rc != 0)
        return rm_rc;
    (void)removed_ino;
    (void)removed_ftype;

    if ((inode.i_mode & 0xF000) == EXT2_S_IFREG ||
        ((inode.i_mode & 0xF000) == EXT2_S_IFLNK && inode.i_blocks > 0)) {
        for (int i = 0; i < 15; i++) {
            if (inode.i_block[i] != 0) {
                if (free_block(inode.i_block[i]) != 0)
                    return -1;
                inode.i_block[i] = 0;
            }
        }
    }

    inode.i_size = 0;
    inode.i_blocks = 0;
    inode.i_links_count = 0;
    if (write_inode(ctx.found, &inode) != 0)
        return -1;

    return free_inode(ctx.found);
}

/**
 * ext2_chmod() - Change permission bits of an inode on disk.
 * @ino:  Inode number.
 * @mode: New permission bits (low 12 bits); file type bits are preserved.
 *
 * Reads the inode, replaces the low 12 bits of i_mode, writes it back.
 * @return: 0 on success, -1 on I/O error.
 */
int ext2_chmod(uint32_t ino, uint16_t mode)
{
    ext2_inode_t inode;
    if (read_inode(ino, &inode) != 0)
        return -1;
    inode.i_mode = (inode.i_mode & 0xF000) | (mode & 0x0FFF);
    return write_inode(ino, &inode);
}

/**
 * ext2_mknod() - Create a char or block device inode in @parent_ino.
 *
 * Allocates an inode with i_mode = @mode (EXT2_S_IFCHR|perms or
 * EXT2_S_IFBLK|perms), i_links_count = 1, no data blocks, and stores
 * @dev in i_block[0] (traditional Linux ext2 device number encoding).
 * Inserts a directory entry with EXT2_FT_CHRDEV or EXT2_FT_BLKDEV into
 * the parent directory using the same split-and-append logic as ext2_create.
 */
int ext2_mknod(uint32_t parent_ino, const char *name, uint16_t mode,
               uint32_t dev, uint32_t *out_ino)
{
    if (!name || !out_ino)
        return -1;

    uint16_t type_bits = mode & 0xF000;
    if (type_bits != EXT2_S_IFCHR && type_bits != EXT2_S_IFBLK)
        return -22;  /* EINVAL */

    uint8_t nlen = 0;
    while (name[nlen] && nlen < 255)
        nlen++;
    if (nlen == 0)
        return -1;

    /* Allocate new inode */
    uint32_t new_ino = 0;
    int r = allocate_inode(&new_ino);
    if (r != 0)
        return r;

    /* Initialise inode: no data blocks, device number in i_block[0] */
    ext2_inode_t new_inode;
    for (uint32_t i = 0; i < sizeof(ext2_inode_t); i++)
        ((uint8_t *)&new_inode)[i] = 0;
    new_inode.i_mode        = mode;
    new_inode.i_links_count = 1;
    new_inode.i_size        = 0;
    new_inode.i_blocks      = 0;
    new_inode.i_block[0]    = dev;   /* (major<<8)|minor, traditional encoding */

    if (write_inode(new_ino, &new_inode) != 0)
        return -1;

    /* Directory entry file_type */
    uint8_t ft = (type_bits == EXT2_S_IFCHR) ? EXT2_FT_CHRDEV : EXT2_FT_BLKDEV;

    /* Insert directory entry in parent — same logic as ext2_create */
    ext2_inode_t parent_inode;
    if (read_inode(parent_ino, &parent_inode) != 0)
        return -1;

    uint32_t new_min_size = 8 + ((uint32_t)(nlen + 3) & ~3u);

    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0)
            break;

        if (read_block(parent_inode.i_block[k], g_block_buf) != 0)
            return -1;

        uint32_t ptr = 0;
        while (ptr < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_block_buf + ptr);
            if (de->rec_len < 8 || (de->rec_len & 3) != 0)
                break;

            uint32_t next_ptr = ptr + de->rec_len;
            int is_last = (next_ptr >= g_block_size);

            if (is_last) {
                uint32_t actual = 8 + ((uint32_t)(de->name_len + 3) & ~3u);
                if (de->inode == 0) actual = 0;
                uint32_t free_space = de->rec_len - actual;

                if (free_space >= new_min_size) {
                    if (de->inode != 0)
                        de->rec_len = (uint16_t)actual;

                    uint32_t new_ptr = (de->inode != 0) ? (ptr + actual) : ptr;
                    ext2_dirent_t *ne = (ext2_dirent_t *)(g_block_buf + new_ptr);
                    ne->inode     = new_ino;
                    ne->rec_len   = (uint16_t)(g_block_size - new_ptr);
                    ne->name_len  = nlen;
                    ne->file_type = ft;
                    for (uint8_t i = 0; i < nlen; i++)
                        ne->name[i] = name[i];

                    if (write_block(parent_inode.i_block[k], g_block_buf) != 0)
                        return -1;

                    *out_ino = new_ino;
                    return 0;
                }
            }
            ptr += de->rec_len;
        }
    }

    /* No space in existing parent blocks — allocate a new one */
    uint32_t new_block = 0;
    if (allocate_block(&new_block) != 0)
        return -2;

    int slot = -1;
    for (int k = 0; k < 12; k++) {
        if (parent_inode.i_block[k] == 0) { slot = k; break; }
    }
    if (slot < 0)
        return -2;

    for (uint32_t i = 0; i < g_block_size; i++)
        g_block_buf[i] = 0;

    ext2_dirent_t *ne = (ext2_dirent_t *)g_block_buf;
    ne->inode     = new_ino;
    ne->rec_len   = (uint16_t)g_block_size;
    ne->name_len  = nlen;
    ne->file_type = ft;
    for (uint8_t i = 0; i < nlen; i++)
        ne->name[i] = name[i];

    if (write_block(new_block, g_block_buf) != 0)
        return -1;

    parent_inode.i_block[slot] = new_block;
    parent_inode.i_blocks     += (uint32_t)(g_block_size / 512);
    if (write_inode(parent_ino, &parent_inode) != 0)
        return -1;

    *out_ino = new_ino;
    return 0;
}

/* -----------------------------------------------------------------------
 * Filesystem type registry integration
 * ---------------------------------------------------------------------- */

static int ext2_statfs(vfs_statfs_t *out)
{
    /* Re-read superblock to get current free counts (write-through cache keeps it fresh) */
    uint8_t sb_buf[1024];
    if (block_read(g_part_start + 2, 2, sb_buf) != 0)
        return -1;
    const ext2_superblock_t *sb = (const ext2_superblock_t *)sb_buf;
    out->f_type    = EXT2_MAGIC;
    out->f_bsize   = g_block_size;
    out->f_blocks  = g_sb_blocks_count;
    out->f_bfree   = sb->s_free_blocks_count;
    out->f_bavail  = sb->s_free_blocks_count;
    out->f_files   = g_sb_inodes_count;
    out->f_ffree   = sb->s_free_inodes_count;
    out->f_namelen = 255;
    return 0;
}

/* ext2 keeps a single global instance (g_part_start/g_block_size/etc. — see
 * "Module state" above), so only one ext2 mount may be active at a time.
 * Set once vfs_register_mount() succeeds; cleared by ext2_do_unmount(). */
static int g_ext2_mounted = 0;

static void ext2_do_unmount(void) {
    g_ext2_mounted = 0;
}

static vfs_ops_t g_ext2_vfs_ops = {
    .lookup   = ext2_lookup,
    .read     = ext2_read_file,
    .readdir  = ext2_readdir,
    .write    = ext2_write_file,
    .flush    = ext2_flush_inode,
    .create   = ext2_create,
    .unlink   = ext2_unlink,
    .rename   = ext2_rename,
    .mkdir    = ext2_mkdir,
    .rmdir    = ext2_rmdir,
    .truncate = ext2_truncate_inode,
    .readlink = ext2_readlink,
    .symlink  = ext2_symlink,
    .mknod    = ext2_mknod,
    .chmod    = ext2_chmod,
    .statfs   = ext2_statfs,
    .unmount  = ext2_do_unmount,
};

static int ext2_fs_mount(const char *source, const char *target, const void *data)
{
    (void)source;
    if (g_ext2_mounted) {
        printk("EXT2: already mounted elsewhere (single-instance filesystem); "
               "unmount it first\n");
        return -16;  /* EBUSY */
    }
    if (!data) {
        printk("EXT2: mount called without vfs_mount_data_t\n");
        return -22;  /* EINVAL */
    }
    const vfs_mount_data_t *opts = (const vfs_mount_data_t *)data;
    const block_device_t *blkdev = blkdev_find(opts->major, opts->minor);
    if (!blkdev) {
        printk("EXT2: block device %u:%u not found\n", opts->major, opts->minor);
        return -22;
    }
    if (ext2_mount(blkdev->lba_start, blkdev->lba_end) != 0)
        return -1;
    int rc = vfs_register_mount(target, &g_ext2_vfs_ops, 0);
    if (rc == 0) g_ext2_mounted = 1;
    return rc;
}

void ext2_init(void)
{
    register_filesystem("ext2", ext2_fs_mount);
}
