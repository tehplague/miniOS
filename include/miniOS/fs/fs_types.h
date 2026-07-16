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
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef _MINIOS_FS_TYPES_H_
#define _MINIOS_FS_TYPES_H_

#include <miniOS/types.h>

/* Callback for iterating directory entries across all FS drivers */
typedef int (*vfs_dirent_cb_t)(const char *name, uint8_t name_len,
                                uint32_t inode, uint8_t file_type, void *ud);

/**
 * vfs_mount_data_t - Generic mount parameters for block-backed filesystems.
 *
 * Passed as the @data argument to vfs_mount_fstype() for any filesystem that
 * reads from a block device.  The FS implementation resolves the block device
 * via blkdev_find(major, minor) to obtain the LBA bounds internally.
 *
 * Example:
 *   vfs_mount_data_t d = { .major = BLKDEV_MAJOR_SATA, .minor = 1 };
 *   vfs_mount_fstype("ext2", NULL, "/", &d);
 */
typedef struct {
    uint8_t major;
    uint8_t minor;
} vfs_mount_data_t;

#endif /* _MINIOS_FS_TYPES_H_ */
