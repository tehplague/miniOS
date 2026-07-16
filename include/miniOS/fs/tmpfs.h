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

#ifndef _MINIOS_FS_TMPFS_H_
#define _MINIOS_FS_TMPFS_H_

#include <miniOS/fs/vfs.h>

/**
 * tmpfs_init() - Initialise the tmpfs inode pool and register the "tmpfs" type.
 *
 * Initialises the shared inode table (inode 1 = internal sentinel root) and
 * calls register_filesystem("tmpfs", ...) so that vfs_mount_fstype("tmpfs", ...)
 * works. Safe to call multiple times — subsequent calls are no-ops.
 * Must be called before any vfs_mount_fstype("tmpfs", ...) invocation.
 */
void tmpfs_init(void);

/**
 * tmpfs_get_ops() - Return a pointer to the static tmpfs vfs_ops_t table.
 *
 * The returned pointer is valid for the lifetime of the kernel.
 * @return: Pointer to static vfs_ops_t with lookup, read, readdir, write, flush set.
 */
vfs_ops_t *tmpfs_get_ops(void);

/**
 * tmpfs_alloc_root() - Allocate a fresh root directory inode for a new mount.
 *
 * Each call to `mount tmpfs <target>` should call this to get an isolated
 * root directory. The returned inode number is stored in vfs_mount_t.root_ino
 * and activated via set_root() before each path lookup on that mount.
 * @return: New inode number, or 0 on failure (inode table full).
 */
uint32_t tmpfs_alloc_root(void);

/**
 * tmpfs_create() - Create a new regular file in a tmpfs directory.
 * @parent_ino: Inode number of the parent directory (must be a directory).
 * @name:       Filename (not full path; no '/' allowed).
 * @new_ino_out: Set to the new inode number on success.
 *
 * Allocates a new inode with EXT2_S_IFREG | (mode & 0777); 0 mode defaults to
 * 0644. Appends an ext2_dirent_t entry to the parent directory's data block.
 * @return: 0 on success, -1 if parent not found, not a directory, name too long,
 *          or inode table full.
 */
int tmpfs_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino_out);

int tmpfs_create_device(uint32_t parent_ino, const char *name, uint8_t device_type,
                        uint32_t *new_ino_out);

/**
 * tmpfs_mknod() - Create a device special file (char or block device node).
 * @parent_ino:  Inode number of the parent directory.
 * @name:        Filename (no '/' allowed).
 * @mode:        EXT2_S_IFCHR|perms or EXT2_S_IFBLK|perms.
 * @dev:         Device number (major<<8)|minor; stored in i_rdev.
 * @new_ino_out: Set to the new inode number on success.
 *
 * Maps well-known (major,minor) pairs to VFS_DEVICE_* constants so that
 * open/read/write dispatch works for TTY (5,0) and null (1,3) devices.
 * @return: 0 on success, -1 on error, -22 if mode type is not IFCHR/IFBLK.
 */
int tmpfs_mknod(uint32_t parent_ino, const char *name, uint16_t mode,
                uint32_t dev, uint32_t *new_ino_out);

/**
 * tmpfs_mkdir() - Create a new directory in a tmpfs directory.
 * @parent_ino:  Inode number of the parent directory.
 * @name:        Directory name (no '/' allowed).
 * @new_ino_out: Set to the new inode number on success.
 *
 * Allocates a new inode (EXT2_S_IFDIR | 0755), adds "." and ".." entries,
 * links the new directory into parent.
 * @return: 0 on success, -1 on error.
 */
int tmpfs_mkdir(uint32_t parent_ino, const char *name, uint32_t *new_ino_out);

/**
 * tmpfs_unlink() - Remove a file from its parent directory and free its inode.
 * @parent_ino: Inode number of the parent directory.
 * @name:       Name of the file to remove (regular file only).
 *
 * Removes the directory entry from the parent. Frees all data blocks and
 * clears the inode slot. Immediate deletion (no open-file keep-alive).
 * @return: 0 on success, -1 if not found or is a directory.
 */
int tmpfs_unlink(uint32_t parent_ino, const char *name);

/**
 * tmpfs_rmdir() - Remove an empty directory.
 * @parent_ino: Inode number of the parent directory.
 * @name:       Name of the subdirectory to remove.
 *
 * Only succeeds if the directory contains only "." and ".." entries.
 * @return: 0 on success, -1 if not found, not empty, or not a directory.
 */
int tmpfs_rmdir(uint32_t parent_ino, const char *name);

#endif /* _MINIOS_FS_TMPFS_H_ */
