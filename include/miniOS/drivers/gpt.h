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

#ifndef _MINIOS_DRIVERS_GPT_H_
#define _MINIOS_DRIVERS_GPT_H_

#include <miniOS/types.h>

/* GPT header lives at LBA 1; backup at the last sector of the disk. */
#define GPT_HEADER_LBA      1
#define GPT_REVISION        0x00010000U

#define GPT_MAX_PARTITIONS  128

/* 16-byte GUID */
typedef struct {
    uint8_t b[16];
} gpt_guid_t;

/* On-disk GPT header (UEFI spec §5.3.2). First 92 bytes are defined; the
 * rest of the 512-byte sector is unused / reserved. */
typedef struct {
    uint8_t    signature[8];             /* "EFI PART" */
    uint32_t   revision;                 /* 0x00010000 */
    uint32_t   header_size;              /* 92 bytes */
    uint32_t   header_crc32;
    uint32_t   reserved;
    uint64_t   my_lba;                   /* LBA of this header */
    uint64_t   alternate_lba;            /* LBA of backup header */
    uint64_t   first_usable_lba;
    uint64_t   last_usable_lba;
    gpt_guid_t disk_guid;
    uint64_t   partition_entries_lba;    /* typically 2 */
    uint32_t   num_partition_entries;    /* typically 128 */
    uint32_t   partition_entry_size;     /* typically 128 bytes */
    uint32_t   partition_entry_array_crc32;
} __attribute__((packed)) gpt_header_t;

/* On-disk GPT partition entry — 128 bytes. */
typedef struct {
    gpt_guid_t type_guid;     /* all-zeros = unused entry */
    gpt_guid_t unique_guid;
    uint64_t   start_lba;
    uint64_t   end_lba;       /* inclusive */
    uint64_t   attributes;
    uint16_t   name[36];      /* UTF-16LE, null-terminated partition name */
} __attribute__((packed)) gpt_partition_entry_t;

/* Caller-facing parsed partition info. */
typedef struct {
    uint64_t   lba_start;
    uint64_t   lba_end;       /* inclusive */
    gpt_guid_t type_guid;
    gpt_guid_t unique_guid;
} gpt_partition_t;

/**
 * gpt_parse() - Read and validate the GPT from the boot disk.
 * @partitions: Output array for parsed partitions.
 * @max_count:  Maximum number of entries @partitions can hold.
 * @count:      Output: number of valid (non-empty) partition entries found.
 *
 * Reads LBA 1 (GPT header), validates the "EFI PART" signature and revision,
 * then iterates the partition entry table (default: LBA 2-33) and populates
 * @partitions with all non-empty entries, up to @max_count.
 *
 * @return: 0 on success, -1 if the signature is invalid or an I/O error occurs.
 */
int gpt_parse(gpt_partition_t *partitions, int max_count, int *count);

#endif /* _MINIOS_DRIVERS_GPT_H_ */
