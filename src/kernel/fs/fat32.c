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

#include <miniOS/io.h>
#include <miniOS/fs/fat32.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/drivers/block_layer.h>
#include <miniOS/drivers/block_device.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Module state — populated at mount time from BPB
 * ---------------------------------------------------------------------- */

static uint64_t g_part_start;
static uint64_t g_part_end;
static uint32_t g_bytes_per_sector;
static uint32_t g_sectors_per_cluster;
static uint32_t g_first_data_sector;   /* partition-relative */
static uint32_t g_fat_start_sector;    /* partition-relative */
static uint32_t g_root_cluster;

/* Not reentrant — single-threaded kernel assumption matches ext2. */
static uint8_t g_fat_buf[512];   /* FAT sector reads */
static uint8_t g_dir_buf[512];   /* directory / data sector reads */

/* -----------------------------------------------------------------------
 * Low-level I/O
 * ---------------------------------------------------------------------- */

static int read_part_sector(uint32_t part_sector, void *buf)
{
    uint64_t lba = g_part_start + part_sector;
    if (lba > g_part_end) {
        printk("FAT32: read past partition end (sector %u)\n", part_sector);
        return -1;
    }
    return block_read(lba, 1, buf);
}

/* Return next cluster in chain, or 0x0FFFFFFF on error/EOC. */
static uint32_t fat_entry(uint32_t cluster)
{
    uint32_t entries_per_sector = g_bytes_per_sector / 4;
    uint32_t sector = g_fat_start_sector + cluster / entries_per_sector;
    uint32_t idx    = cluster % entries_per_sector;
    if (read_part_sector(sector, g_fat_buf) != 0)
        return 0x0FFFFFFF;
    return ((uint32_t *)g_fat_buf)[idx] & 0x0FFFFFFF;
}

static uint32_t cluster_to_part_sector(uint32_t cluster)
{
    return g_first_data_sector + (cluster - 2) * g_sectors_per_cluster;
}

/* Starting cluster encoded in a directory entry (0 → root). */
static uint32_t entry_cluster(const fat32_dir_entry_t *e)
{
    uint32_t c = ((uint32_t)e->cluster_high << 16) | e->cluster_low;
    return (c == 0) ? g_root_cluster : c;
}

/* -----------------------------------------------------------------------
 * LFN helpers
 * ---------------------------------------------------------------------- */

static uint8_t lfn_checksum(const char *name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name11[i];
    return sum;
}

/* Decode up to slot_count * 13 UCS-2 characters to ASCII into out[0..out_size-1]. */
static uint32_t lfn_decode(const uint16_t *lfn, int slot_count,
                            char *out, uint32_t out_size)
{
    uint32_t n = 0;
    for (int j = 0; j < slot_count * 13 && n + 1 < out_size; j++) {
        uint16_t c = lfn[j];
        if (c == 0x0000 || c == 0xFFFF)
            break;
        out[n++] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = '\0';
    return n;
}

/* Build null-terminated 8.3 name from a directory entry.  Returns length. */
static uint32_t short_name(const fat32_dir_entry_t *e, char *out, uint32_t out_size)
{
    uint32_t n = 0;
    for (int j = 0; j < 8 && e->name[j] != ' ' && n + 1 < out_size; j++)
        out[n++] = e->name[j];
    if (e->extension[0] != ' ' && n + 2 < out_size) {
        out[n++] = '.';
        for (int j = 0; j < 3 && e->extension[j] != ' ' && n + 1 < out_size; j++)
            out[n++] = e->extension[j];
    }
    out[n] = '\0';
    return n;
}

/* -----------------------------------------------------------------------
 * Directory scanner
 *
 * Iterates all 32-byte raw entries in the cluster chain starting at
 * dir_cluster.  Handles LFN accumulation.  Calls cb for each visible
 * file or directory entry (including "." and "..").
 *
 * offset_p: raw-entry-index cursor for resume.  On entry, raw entries
 *   with index < *offset_p are skipped (LFN state is reset for each
 *   skipped short entry).  On exit, *offset_p points just past the last
 *   emitted short entry's raw slot.  Pass NULL to scan all entries.
 *
 * Returns 0 when all entries visited, first non-zero cb return for early
 * exit, or -1 on I/O error.
 * ---------------------------------------------------------------------- */

typedef int (*fat32_scan_cb_t)(const char *name, uint8_t name_len,
                                uint32_t cluster, uint8_t is_dir,
                                uint32_t filesize, void *ud);

static int fat32_scan_dir(uint32_t dir_cluster, uint64_t *offset_p,
                          fat32_scan_cb_t cb, void *ud)
{
    /* LFN accumulator: up to 20 LFN slots × 13 UCS-2 chars */
    uint16_t lfn_buf[20 * 13 + 1];
    int      lfn_count = 0;
    uint8_t  lfn_csum  = 0;

    uint64_t resume_at = offset_p ? *offset_p : 0;
    uint64_t raw_idx   = 0;

    for (uint32_t cur = dir_cluster;
         cur >= 2 && cur < 0x0FFFFFF8;
         cur = fat_entry(cur)) {

        uint32_t base_sector = cluster_to_part_sector(cur);

        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            if (read_part_sector(base_sector + s, g_dir_buf) != 0)
                return -1;

            uint32_t entries_per_sector = g_bytes_per_sector / sizeof(fat32_dir_entry_t);
            fat32_dir_entry_t *entries = (fat32_dir_entry_t *)g_dir_buf;

            for (uint32_t i = 0; i < entries_per_sector; i++) {
                fat32_dir_entry_t *e = &entries[i];

                if ((uint8_t)e->name[0] == 0x00)
                    return 0; /* end of directory */

                if ((uint8_t)e->name[0] == 0xE5) {
                    lfn_count = 0;
                    raw_idx++;
                    continue;
                }

                if (e->attributes == 0x0F) {
                    /* LFN entry */
                    fat32_long_entry_t *lfn = (fat32_long_entry_t *)e;
                    int seq = (lfn->order & 0x3F) - 1; /* 0-based slot index */
                    if (lfn->order & 0x40) {
                        /* highest-numbered (first on disk) — reset */
                        lfn_count = seq + 1;
                        lfn_csum  = lfn->checksum;
                        for (int x = 0; x < 20 * 13; x++) lfn_buf[x] = 0xFFFF;
                        lfn_buf[lfn_count * 13] = 0;
                    }
                    if (seq >= 0 && seq < 20) {
                        uint16_t *dst = lfn_buf + seq * 13;
                        for (int x = 0; x < 5; x++) dst[x]      = lfn->name1[x];
                        for (int x = 0; x < 6; x++) dst[5 + x]  = lfn->name2[x];
                        for (int x = 0; x < 2; x++) dst[11 + x] = lfn->name3[x];
                    }
                    raw_idx++;
                    continue;
                }

                if (e->attributes & 0x08) {
                    /* volume label */
                    lfn_count = 0;
                    raw_idx++;
                    continue;
                }

                /* Regular file or directory */
                raw_idx++;

                if (raw_idx <= resume_at) {
                    lfn_count = 0;
                    continue;
                }

                char name[20 * 13 + 1];
                uint32_t name_len;

                if (lfn_count > 0) {
                    char raw11[11];
                    for (int x = 0; x < 8; x++) raw11[x]     = e->name[x];
                    for (int x = 0; x < 3; x++) raw11[8 + x] = e->extension[x];
                    if (lfn_checksum(raw11) == lfn_csum) {
                        name_len = lfn_decode(lfn_buf, lfn_count, name, sizeof(name));
                    } else {
                        name_len = short_name(e, name, sizeof(name));
                    }
                    lfn_count = 0;
                } else {
                    name_len = short_name(e, name, sizeof(name));
                }

                if (name_len == 0) continue;

                uint8_t is_dir = (e->attributes & 0x10) != 0;
                uint32_t cluster = entry_cluster(e);

                if (offset_p)
                    *offset_p = raw_idx;

                int r = cb(name, (uint8_t)name_len, cluster, is_dir, e->filesize, ud);
                if (r != 0)
                    return r;
            }
        }
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * VFS ops
 * ---------------------------------------------------------------------- */

/* find_entry_cb — used by fat32_lookup to match one path component */
struct find_ctx {
    const char *target;
    uint32_t    found_cluster;
    uint32_t    found_size;
    uint8_t     found_is_dir;
};

static int find_entry_cb(const char *name, uint8_t name_len, uint32_t cluster,
                          uint8_t is_dir, uint32_t filesize, void *ud)
{
    struct find_ctx *ctx = (struct find_ctx *)ud;
    uint32_t tlen = (uint32_t)strlen(ctx->target);
    if (tlen != name_len)
        return 0;
    for (uint32_t i = 0; i < tlen; i++) {
        char a = name[i], b = ctx->target[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    ctx->found_cluster = cluster;
    ctx->found_size    = filesize;
    ctx->found_is_dir  = is_dir;
    return 1; /* stop scanning */
}

static int fat32_lookup(const char *path, vfs_inode_info_t *out)
{
    if (!path || !out)
        return -1;

    while (*path == '/')
        path++;

    if (*path == '\0') {
        out->inode     = g_root_cluster;
        out->file_type = VFS_FILE_TYPE_DIR;
        out->size      = 0;
        out->dev       = 0;
        out->mode      = 0555;
        return 0;
    }

    uint32_t cur_cluster = g_root_cluster;

    while (*path != '\0') {
        char component[256];
        uint32_t clen = 0;
        while (path[clen] != '\0' && path[clen] != '/' && clen < 255) {
            component[clen] = path[clen];
            clen++;
        }
        component[clen] = '\0';
        path += clen;
        while (*path == '/')
            path++;

        struct find_ctx ctx = { .target = component, .found_cluster = 0 };
        int r = fat32_scan_dir(cur_cluster, NULL, find_entry_cb, &ctx);
        if (r < 0 || ctx.found_cluster == 0)
            return -1;

        if (*path != '\0' && !ctx.found_is_dir)
            return -1;

        cur_cluster = ctx.found_cluster;

        if (*path == '\0') {
            out->inode     = cur_cluster;
            out->file_type = ctx.found_is_dir ? VFS_FILE_TYPE_DIR : VFS_FILE_TYPE_REG;
            out->size      = ctx.found_size;
            out->dev       = 0;
            out->mode      = ctx.found_is_dir ? 0555 : 0444;
            return 0;
        }
    }

    return -1;
}

/* readdir scan adapter — translates fat32_scan_cb_t to vfs_dirent_cb_t */
struct readdir_ctx {
    vfs_dirent_cb_t vfs_cb;
    void           *ud;
};

static int readdir_adapter(const char *name, uint8_t name_len, uint32_t cluster,
                            uint8_t is_dir, uint32_t filesize, void *ud)
{
    (void)filesize;
    struct readdir_ctx *ctx = (struct readdir_ctx *)ud;
    uint8_t ftype = is_dir ? VFS_FILE_TYPE_DIR : VFS_FILE_TYPE_REG;
    return ctx->vfs_cb(name, name_len, cluster, ftype, ctx->ud);
}

static int fat32_readdir(uint32_t ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud)
{
    if (!offset) return -1;
    struct readdir_ctx ctx = { .vfs_cb = cb, .ud = ud };
    return fat32_scan_dir(ino, offset, readdir_adapter, &ctx);
}

static int fat32_read(uint32_t ino, uint64_t off, void *buf, uint32_t len)
{
    if (len == 0) return 0;

    uint32_t cluster_bytes = g_sectors_per_cluster * g_bytes_per_sector;

    /* Advance to the cluster containing byte offset `off`. */
    uint32_t skip_clusters = (uint32_t)(off / cluster_bytes);
    uint32_t cur = ino;
    for (uint32_t i = 0; i < skip_clusters; i++) {
        cur = fat_entry(cur);
        if (cur >= 0x0FFFFFF8) return 0;
    }

    uint32_t in_cluster = (uint32_t)(off % cluster_bytes);
    uint32_t bytes_done = 0;

    while (bytes_done < len && cur >= 2 && cur < 0x0FFFFFF8) {
        uint32_t base_sector = cluster_to_part_sector(cur);
        uint32_t sector_off  = in_cluster / g_bytes_per_sector;
        uint32_t in_sector   = in_cluster % g_bytes_per_sector;

        while (bytes_done < len && sector_off < g_sectors_per_cluster) {
            if (read_part_sector(base_sector + sector_off, g_dir_buf) != 0)
                return -1;
            uint32_t avail = g_bytes_per_sector - in_sector;
            uint32_t want  = len - bytes_done;
            uint32_t copy  = avail < want ? avail : want;
            for (uint32_t i = 0; i < copy; i++)
                ((uint8_t *)buf)[bytes_done + i] = g_dir_buf[in_sector + i];
            bytes_done += copy;
            in_sector   = 0;
            sector_off++;
        }

        in_cluster = 0;
        if (bytes_done < len)
            cur = fat_entry(cur);
    }

    return (int)bytes_done;
}

/* -----------------------------------------------------------------------
 * Mount
 * ---------------------------------------------------------------------- */

static int fat32_statfs(vfs_statfs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->f_type    = 0x4d44; /* MSDOS_SUPER_MAGIC */
    out->f_bsize   = g_sectors_per_cluster * g_bytes_per_sector;
    if (out->f_bsize == 0) out->f_bsize = 512;
    uint64_t total_sectors = g_part_end - g_part_start;
    out->f_blocks  = total_sectors / g_sectors_per_cluster;
    out->f_bfree   = 0; /* read-only mount */
    out->f_bavail  = 0;
    out->f_namelen = 255;
    return 0;
}

static vfs_ops_t g_fat32_ops = {
    .lookup  = fat32_lookup,
    .read    = fat32_read,
    .readdir = fat32_readdir,
    .statfs  = fat32_statfs,
};

int fat32_mount(uint64_t part_lba_start, uint64_t part_lba_end)
{
    g_part_start = part_lba_start;
    g_part_end   = part_lba_end;

    /* Read BPB from sector 0 of the partition. */
    if (block_read(part_lba_start, 1, g_fat_buf) != 0) {
        printk("FAT32: failed to read BPB\n");
        return -1;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)g_fat_buf;
    fat32_ebr_t *ebr = &bpb->ebr;

    g_bytes_per_sector   = bpb->bytes_per_sector;
    g_sectors_per_cluster = bpb->sectors_per_cluster;

    if (g_bytes_per_sector != 512) {
        printk("FAT32: unsupported sector size %u\n", g_bytes_per_sector);
        return -1;
    }

    uint32_t sectors_per_fat = bpb->sectors_per_fat != 0
        ? bpb->sectors_per_fat : ebr->sectors_per_fat;
    uint32_t root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32
        + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;

    g_fat_start_sector  = bpb->reserved_sector_count;
    g_first_data_sector = bpb->reserved_sector_count
                          + (uint32_t)bpb->fat_count * sectors_per_fat
                          + root_dir_sectors;
    g_root_cluster      = ebr->root_directory_cluster;

    printk("FAT32: mounted — cluster=%u B, root_cluster=%u, first_data=%u\n",
           g_sectors_per_cluster * g_bytes_per_sector,
           g_root_cluster, g_first_data_sector);
    return 0;
}

static int fat32_fs_mount(const char *source, const char *target, const void *data)
{
    (void)source;
    if (!data) {
        printk("FAT32: mount called without vfs_mount_data_t\n");
        return -22;
    }
    const vfs_mount_data_t *opts = (const vfs_mount_data_t *)data;
    const block_device_t *blkdev = blkdev_find(opts->major, opts->minor);
    if (!blkdev) {
        printk("FAT32: block device %u:%u not found\n", opts->major, opts->minor);
        return -22;
    }
    if (fat32_mount(blkdev->lba_start, blkdev->lba_end) != 0)
        return -1;
    return vfs_register_mount(target, &g_fat32_ops, 0);
}

void fat32_init(void)
{
    register_filesystem("fat32", fat32_fs_mount);
}
