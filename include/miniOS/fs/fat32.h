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

#ifndef _MINIOS_FS_FAT32_H_
#define _MINIOS_FS_FAT32_H_

#include <miniOS/fs/vfs.h>

typedef struct
{
    uint32_t sectors_per_fat;
    uint16_t flags;
    uint16_t version;
    uint32_t root_directory_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved2;
    uint8_t signature;
    uint32_t volume_id;
    char volume_label[11];
    char system_id[8];
    uint8_t boot_code[420];
    uint16_t boot_signature;
} __attribute__((packed)) fat32_ebr_t;

typedef struct
{
    uint8_t jump[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sector_count;
    uint8_t media_descriptor_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sector_count;
    uint32_t large_sector_count;

    fat32_ebr_t ebr;
}  __attribute__((packed)) fat32_bpb_t;

typedef struct
{
    uint32_t lead_signature;
    uint8_t reserved[480];
    uint32_t signature;
    uint32_t last_free_cluster_count;
    uint32_t last_allocated_cluster;
    uint8_t reserved2[12];
    uint32_t trail_signature;
} __attribute__((packed)) fat32_fsinfo_t;

typedef struct
{
    char name[8];
    char extension[3];
    uint8_t attributes;
    uint8_t lowercase;
    uint8_t ctime_ms;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t filesize;
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct
{
    uint8_t order;
    uint16_t name1[5];
    uint8_t attribute;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} __attribute__((packed)) fat32_long_entry_t;

/* -----------------------------------------------------------------------
 * Filesystem type registry integration
 * ---------------------------------------------------------------------- */

/**
 * fat32_init() - Register the FAT32 filesystem type with the VFS registry.
 *
 * After this call, vfs_mount_fstype("fat32", source, target, data) can be
 * used to mount any FAT32 partition by passing a vfs_mount_data_t as @data.
 * fat32_init() does NOT mount anything itself; it only registers the type.
 * Call once during kernel startup, before the first FAT32 mount.
 */
void fat32_init(void);

#endif
