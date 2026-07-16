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

#include <miniOS/drivers/gpt.h>
#include <miniOS/drivers/block_layer.h>
#include <miniOS/io.h>
#include <string.h>

#define SECTOR_SIZE 512

static const uint8_t GPT_SIG[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

static bool guid_is_zero(const gpt_guid_t *g) {
    for (int i = 0; i < 16; i++)
        if (g->b[i]) return false;
    return true;
}

int gpt_parse(gpt_partition_t *partitions, int max_count, int *count) {
    uint8_t buf[SECTOR_SIZE];
    *count = 0;

    if (block_read(GPT_HEADER_LBA, 1, buf) != 0) {
        printk("GPT: I/O error reading header at LBA 1\n");
        return -1;
    }

    gpt_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));

    if (memcmp(hdr.signature, GPT_SIG, 8) != 0) {
        printk("GPT: invalid signature\n");
        return -1;
    }
    if (hdr.revision != GPT_REVISION) {
        printk("GPT: unsupported revision 0x%08x\n", (uint32_t)hdr.revision);
        return -1;
    }

    uint32_t entry_size = hdr.partition_entry_size;   /* 128 per spec */
    uint32_t n = hdr.num_partition_entries;
    if (n > GPT_MAX_PARTITIONS) n = GPT_MAX_PARTITIONS;

    /* Entries are packed 4 per sector (512 / 128 = 4). */
    uint32_t entries_per_sector = SECTOR_SIZE / entry_size;

    uint8_t sector_buf[SECTOR_SIZE];
    uint64_t last_lba = (uint64_t)-1;

    for (uint32_t e = 0; e < n; e++) {
        uint64_t lba = hdr.partition_entries_lba + e / entries_per_sector;
        uint32_t off = (e % entries_per_sector) * entry_size;

        if (lba != last_lba) {
            if (block_read(lba, 1, sector_buf) != 0) {
                printk("GPT: I/O error reading entries at LBA %llu\n", (unsigned long long)lba);
                return -1;
            }
            last_lba = lba;
        }

        gpt_partition_entry_t entry;
        memcpy(&entry, sector_buf + off, sizeof(entry));

        if (guid_is_zero(&entry.type_guid))
            continue;

        if (*count >= max_count)
            break;

        partitions[*count].lba_start   = entry.start_lba;
        partitions[*count].lba_end     = entry.end_lba;
        partitions[*count].type_guid   = entry.type_guid;
        partitions[*count].unique_guid = entry.unique_guid;
        (*count)++;

        printk("GPT: partition %d LBA %llu-%llu\n",
               *count, (unsigned long long)entry.start_lba,
               (unsigned long long)entry.end_lba);
    }

    printk("GPT: found %d partition(s)\n", *count);
    return 0;
}
