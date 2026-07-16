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
// OUT of OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef _MINIOS_FS_EXT2_H_
#define _MINIOS_FS_EXT2_H_

#include <miniOS/types.h>
#include <miniOS/fs/fs_types.h>

typedef struct vfs_inode_info vfs_inode_info_t;

/* -----------------------------------------------------------------------
 * ext2 on-disk constants
 * ---------------------------------------------------------------------- */
#define EXT2_MAGIC    0xEF53
#define EXT2_ROOT_INO 2

/* Inode mode type bits */
#define EXT2_S_IFREG  0x8000   /* regular file */
#define EXT2_S_IFDIR  0x4000   /* directory */
#define EXT2_S_IFLNK  0xA000   /* symbolic link */
#define EXT2_S_IFCHR  0x2000   /* character device */
#define EXT2_S_IFBLK  0x6000   /* block device */

/* Directory entry file_type values */
#define EXT2_FT_UNKNOWN   0
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_SYMLINK   7

/* -----------------------------------------------------------------------
 * On-disk structures (all little-endian; packed to match disk layout)
 * ---------------------------------------------------------------------- */

/* ext2 superblock — located at byte offset 1024 from partition start
 * (block 1 when block_size == 1024) */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   /* 1 for 1024-byte blocks, 0 for 2048+ */
    uint32_t s_log_block_size;     /* block_size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;          /* 0 = original, 1 = dynamic */
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV fields (only valid if s_rev_level >= 1) */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* pad to 1024 bytes */
    uint8_t  s_padding[820];
} __attribute__((packed)) ext2_superblock_t;

/* Block Group Descriptor — 32 bytes each, in a table at block 2
 * (for 1024-byte blocks) */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

/* Inode — 128 bytes (rev 0) or s_inode_size bytes (rev 1) */
typedef struct {
    uint16_t i_mode;        /* file type + permissions */
    uint16_t i_uid;
    uint32_t i_size;        /* file size in bytes */
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;      /* count in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];   /* 0-11: direct, 12: indirect, 13: dbl-indirect, 14: triple */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* Directory entry — variable length */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;   /* 0=unknown, 1=reg, 2=dir, 7=symlink */
    char     name[];      /* NOT null-terminated; length = name_len */
} __attribute__((packed)) ext2_dirent_t;

/* -----------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/**
 * ext2_mount() - Mount an ext2 partition by reading and validating its superblock.
 * @part_lba_start: First sector of the partition (LBA offset from disk start).
 * @part_lba_end: Last sector of the partition (inclusive).
 *
 * Reads the superblock from byte offset 1024 within the partition (sector
 * part_lba_start + 2 for 512-byte sectors). Validates the ext2 magic number
 * (0xEF53). Initialises global filesystem state: block size, sectors per block,
 * inodes per group, inode size, first data block. Reads block group descriptor
 * table to locate the inode table. Assumes a single block group.
 *
 * @return: 0 on success, -1 if superblock magic is wrong or ATA I/O failed.
 */
int ext2_mount(uint64_t part_lba_start, uint64_t part_lba_end);

/**
 * ext2_lookup() - Resolve an absolute path to typed inode metadata.
 * @path: Absolute path string (e.g., "/hello.txt", "/bin/sh").
 *        Must begin with '/'. Components separated by '/'.
 * @out: Output metadata populated on success.
 *
 * Starts at root inode (inode 2) and descends through each path component
 * by iterating directory entries via ext2_readdir with a find_entry_cb callback.
 * Directory traversal still handles only direct blocks (i_block[0..11]),
 * which is sufficient for the current single-group test image. Uses static
 * g_block_buf — not reentrant.
 *
 * Context: Single-threaded; uses static g_block_buf. Not ISR-safe.
 * @return: 0 on success, -1 if path not found or on error.
 */
int ext2_lookup(const char *path, vfs_inode_info_t *out);

/**
 * ext2_read_file() - Read bytes from an open inode.
 * @ino: Inode number of the file (>= 2; must be a regular file).
 * @off: Byte offset within the file to start reading from.
 * @buf: Destination buffer for read data.
 * @len: Maximum number of bytes to read.
 *
 * Maps byte range [@off, @off+@len) to direct block indices plus the first
 * single-indirect block (i_block[12]). Reads each required block via
 * ata_read_sectors into g_block_buf, then copies the relevant slice to @buf.
 * Clamps to file size (i_size). Handles cross-block reads by iterating over
 * block boundaries.
 *
 * Context: Single-threaded; uses static g_block_buf. Not reentrant.
 * @return: Number of bytes actually read (may be less than @len at EOF),
 *          or -1 on I/O error or unsupported inode type.
 */
int ext2_read_file(uint32_t ino, uint64_t off, void *buf, uint32_t len);

/**
 * ext2_readdir() - Iterate directory entries in an inode.
 * @dir_ino: Inode number of the directory to read (must have i_mode dir bit set).
 * @offset: Caller-owned byte cursor. Entries before *@offset are skipped; on
 *          return it points just past the last emitted record.
 * @cb: Callback invoked for each directory entry with (name, name_len, inode,
 *      file_type, ud). If @cb returns non-zero, iteration stops immediately.
 * @ud: Opaque user data pointer forwarded to @cb unchanged.
 *
 * Reads all direct blocks of the directory inode and iterates ext2_dirent_t
 * records within each block. Skips entries with inode == 0 (deleted entries).
 * Handles variable-length records via rec_len. Direct blocks only.
 *
 * Context: Single-threaded; uses static g_block_buf. Not reentrant.
 * @return: 0 when all entries have been visited (or directory was empty),
 *          or the non-zero value returned by @cb if iteration was stopped early.
 */
int ext2_readdir(uint32_t dir_ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud);

/**
 * ext2_write_file() - Write bytes into an existing inode's data blocks.
 * @ino: Inode number of the target regular file.
 * @off: Byte offset within the file to start writing at.
 * @buf: Source buffer containing bytes to write.
 * @len: Number of bytes to write.
 *
 * Allocates new data blocks as needed when the write extends past existing
 * allocated blocks. Updates i_size if (off + len) > current i_size.
 * Does NOT flush to disk — caller must call ext2_flush_inode() then ata_flush().
 *
 * @return: Number of bytes written, or -1 on allocation or I/O error.
 */
int ext2_write_file(uint32_t ino, uint64_t off, const void *buf, uint32_t len);

/**
 * ext2_create() - Create a new regular file inode and directory entry.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated filename (no path separators).
 * @out_ino: Output: inode number of the newly created file.
 *
 * Allocates a new inode via the inode bitmap (first-fit). Initialises the
 * inode with i_mode = EXT2_S_IFREG | (mode & 0777), i_links_count = 1, all
 * block pointers zero, i_size = 0. Appends an ext2_dirent_t to the parent
 * directory. Writes back the new inode, updated bitmaps, superblock, and BGD.
 * @mode: Lower 9 bits used as permission bits; 0 defaults to 0644.
 *
 * @return: 0 on success, -1 on allocation or I/O error, -28 if parent full.
 */
int ext2_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *out_ino);

/**
 * ext2_flush_inode() - Write the in-memory inode state back to disk.
 * @ino: Inode number to flush.
 *
 * Reads the inode table block containing @ino, updates the inode bytes in
 * place with the current on-disk inode data, then writes the block back.
 * Called from vfs_close() via ops->flush to persist i_size and i_block[]
 * after a write sequence.
 *
 * @return: 0 on success, -1 on I/O error.
 */
int ext2_flush_inode(uint32_t ino);

/**
 * ext2_truncate_inode() - Truncate a regular file inode to zero bytes.
 * @ino: Inode number of the file to truncate (must be a regular file).
 *
 * Frees all data blocks referenced by the inode, clears i_block[0..14],
 * resets i_size = 0 and i_blocks = 0, and writes the inode back to disk.
 * Updates block bitmap, superblock s_free_blocks_count, and BGD
 * bg_free_blocks_count for each freed block.
 *
 * @return: 0 on success, -1 on type mismatch or I/O error.
 */
int ext2_truncate_inode(uint32_t ino, uint64_t new_size);

/**
 * ext2_readlink() - Read the target of a symlink inode (POSIX: not null-terminated).
 * @ino: Inode number of the symlink.
 * @buf: Output buffer.
 * @len: Maximum bytes to copy.
 * @return: Bytes copied (>= 0) or -1 on error / not a symlink.
 */
int ext2_readlink(uint32_t ino, char *buf, uint32_t len);

/**
 * ext2_symlink() - Create a symlink named @name in directory @parent_ino.
 * @parent_ino: Parent directory inode.
 * @name: Symlink filename (no path separators).
 * @target: Target string stored as-is (may be relative or absolute).
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC.
 */
int ext2_symlink(uint32_t parent_ino, const char *name, const char *target);

/**
 * ext2_mknod() - Create a device-file inode (char or block) in a directory.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated filename (no '/' allowed).
 * @mode: Inode mode: EXT2_S_IFCHR|perms or EXT2_S_IFBLK|perms.
 * @dev:  Device number encoded as (major<<8)|minor stored in i_block[0].
 * @out_ino: Set to the new inode number on success.
 *
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC, -22 on bad mode.
 */
int ext2_mknod(uint32_t parent_ino, const char *name, uint16_t mode,
               uint32_t dev, uint32_t *out_ino);

/**
 * ext2_mkdir() - Create a new directory inode and link it into @parent_ino.
 * @parent_ino: Inode number of the parent directory.
 * @name: Null-terminated directory name (no '/' allowed).
 * @out_ino: Set to the new directory inode number on success.
 *
 * Allocates an inode (i_mode=EXT2_S_IFDIR|0755, i_links_count=2) and a data
 * block containing '.' (→ new_ino) and '..' (→ parent_ino) entries.
 * Inserts a directory entry with file_type=EXT2_FT_DIR in the parent, then
 * increments the parent's i_links_count for the '..' back-reference and
 * bumps bg_used_dirs_count in the block group descriptor.
 *
 * @return: 0 on success, -1 on I/O error, -2 on ENOSPC.
 */
int ext2_mkdir(uint32_t parent_ino, const char *name, uint32_t *out_ino);

/**
 * ext2_rmdir() - Remove an empty directory inode and its parent entry.
 * @parent_ino: Inode number of the parent directory.
 * @name: Directory name to remove.
 *
 * The target must exist, be a directory, and contain only "." and "..".
 * Frees the directory data block and inode, removes the parent directory
 * entry, decrements the parent's link count, and updates bg_used_dirs_count.
 *
 * @return: 0 on success, or a negative errno-style value on failure.
 */
int ext2_rmdir(uint32_t parent_ino, const char *name);

/* -----------------------------------------------------------------------
 * Filesystem type registry integration
 * ---------------------------------------------------------------------- */

/**
 * ext2_init() - Register the ext2 filesystem type with the VFS registry.
 *
 * After this call, vfs_mount_fstype("ext2", source, target, data) can be
 * used to mount any ext2 partition by passing a vfs_mount_data_t as @data.
 * ext2_init() does NOT mount anything itself; it only registers the type.
 * Call once during kernel startup, before the first ext2 mount.
 */
void ext2_init(void);

#endif /* _MINIOS_FS_EXT2_H_ */
