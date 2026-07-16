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

/* tmpfs.c — In-memory temporary filesystem.
 *
 * Storage model:
 *   - Inodes: static array g_tmpfs_inodes[TMPFS_MAX_INODES] indexed by inode number.
 *             Inode 0 is invalid (reserved/null sentinel).
 *             Inode 1 is the tmpfs root directory (created by tmpfs_init()).
 *   - Data:   Each inode has i_block[] — an array of void* pointers to
 *             heap-allocated TMPFS_BLOCK_SIZE (4096) byte buffers.
 *             Blocks are allocated on first write and freed on unlink.
 *   - Dirs:   Directory inodes store ext2_dirent_t-format records in their data blocks.
 *
 * Path resolution:
 *   The lookup() function receives a RELATIVE path (mount point already stripped by VFS router).
 *   An empty string ("") means the tmpfs root itself.
 *   Path components are split on '/'; each component is looked up via tmpfs_dir_find().
 */

#include <miniOS/fs/tmpfs.h>
#include <miniOS/fs/ext2.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/mm/heap.h>
#include <miniOS/io.h>
#include <string.h>

#define TMPFS_MAX_INODES  64
#define TMPFS_BLOCK_SIZE  4096
#define TMPFS_MAX_BLOCKS  16    /* max direct blocks per inode = 64 KiB max file size */
#define TMPFS_ROOT_INO    1
#define TMPFS_NAME_MAX    255

/* -----------------------------------------------------------------------
 * Inode structure
 * --------------------------------------------------------------------- */
typedef struct {
    uint32_t ino;              /* 0 = free slot */
    uint16_t i_mode;           /* EXT2_S_IFREG | perms  or  EXT2_S_IFDIR | perms */
    uint8_t  device_type;      /* VFS_DEVICE_* for synthetic char devices */
    uint32_t i_rdev;           /* device number (major<<8)|minor for device nodes */
    uint32_t i_size;           /* current byte size */
    uint32_t i_links_count;    /* 1 for files, 2+ for dirs (self + parent + subdirs) */
    void    *i_block[TMPFS_MAX_BLOCKS]; /* heap-allocated data blocks */
    uint32_t i_block_count;    /* number of blocks allocated */
    char     symlink_target[256]; /* non-empty iff i_mode & EXT2_S_IFLNK; null-terminated */
} tmpfs_inode_t;

static tmpfs_inode_t g_tmpfs_inodes[TMPFS_MAX_INODES];
static uint32_t      g_tmpfs_next_ino = TMPFS_ROOT_INO; /* next inode to allocate */
static int           g_tmpfs_initialized = 0;
static uint32_t      g_tmpfs_active_root = TMPFS_ROOT_INO; /* set per-mount before lookup */

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

static tmpfs_inode_t *tmpfs_inode_get(uint32_t ino) {
    if (ino == 0 || ino >= TMPFS_MAX_INODES) return NULL;
    if (g_tmpfs_inodes[ino].ino != ino) return NULL;
    return &g_tmpfs_inodes[ino];
}

static tmpfs_inode_t *tmpfs_alloc_inode(uint16_t mode) {
    /* Linear scan for next free slot starting from g_tmpfs_next_ino */
    for (uint32_t i = g_tmpfs_next_ino; i < TMPFS_MAX_INODES; i++) {
        if (g_tmpfs_inodes[i].ino == 0) {
            tmpfs_inode_t *n = &g_tmpfs_inodes[i];
            n->ino           = i;
            n->i_mode        = mode;
            n->device_type   = VFS_DEVICE_NONE;
            n->i_rdev        = 0;
            n->i_size        = 0;
            n->i_links_count = 1;
            n->i_block_count = 0;
            n->symlink_target[0] = '\0';
            for (int b = 0; b < TMPFS_MAX_BLOCKS; b++) n->i_block[b] = NULL;
            if (i == g_tmpfs_next_ino) g_tmpfs_next_ino++;
            return n;
        }
    }
    return NULL;  /* inode table full */
}

static void tmpfs_free_inode(tmpfs_inode_t *ino) {
    for (int b = 0; b < TMPFS_MAX_BLOCKS; b++) {
        if (ino->i_block[b]) { kfree(ino->i_block[b]); ino->i_block[b] = NULL; }
    }
    ino->ino = 0;  /* mark slot as free */
}

/* Ensure block b exists in inode, allocating if needed. Returns pointer or NULL. */
static void *tmpfs_get_or_alloc_block(tmpfs_inode_t *ino, uint32_t bidx) {
    if (bidx >= TMPFS_MAX_BLOCKS) return NULL;
    if (!ino->i_block[bidx]) {
        ino->i_block[bidx] = kmalloc(TMPFS_BLOCK_SIZE);
        if (!ino->i_block[bidx]) return NULL;
        memset(ino->i_block[bidx], 0, TMPFS_BLOCK_SIZE);
        if (bidx >= ino->i_block_count) ino->i_block_count = bidx + 1;
    }
    return ino->i_block[bidx];
}

/* -----------------------------------------------------------------------
 * Directory helpers
 * --------------------------------------------------------------------- */

/* Append a directory entry to dir_ino's data. Returns 0 on success. */
static int tmpfs_dir_append(tmpfs_inode_t *dir, uint32_t child_ino,
                             const char *name, uint8_t file_type) {
    size_t raw_len = strlen(name);
    if (raw_len == 0 || raw_len > TMPFS_NAME_MAX) return -1;
    uint8_t name_len = (uint8_t)raw_len;

    /* dirent size: header (8 bytes) + name, rounded up to 4-byte boundary */
    uint16_t need = (uint16_t)((8 + name_len + 3) & ~3u);

    /* Find position: scan existing entries to find end of used space */
    uint64_t pos = dir->i_size;
    uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
    uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);

    if (boff + need > TMPFS_BLOCK_SIZE) {
        /* Spill to next block */
        bidx++;
        boff = 0;
    }

    void *blk = tmpfs_get_or_alloc_block(dir, bidx);
    if (!blk) return -1;

    ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)blk + boff);
    de->inode    = child_ino;
    de->rec_len  = need;
    de->name_len = name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);

    /* Update directory size */
    uint64_t new_end = (uint64_t)bidx * TMPFS_BLOCK_SIZE + boff + need;
    if (new_end > dir->i_size) dir->i_size = new_end;
    return 0;
}

/* Find a named entry in a directory. Returns child inode number or 0. */
static uint32_t tmpfs_dir_find(tmpfs_inode_t *dir, const char *name, uint8_t *ftype_out) {
    uint32_t name_len = (uint32_t)strlen(name);
    uint64_t pos = 0;
    while (pos < dir->i_size) {
        uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);
        if (bidx >= TMPFS_MAX_BLOCKS || !dir->i_block[bidx]) break;
        ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)dir->i_block[bidx] + boff);
        if (de->rec_len == 0) break;  /* corrupt/end */
        if (de->inode != 0 && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            if (ftype_out) *ftype_out = de->file_type;
            return de->inode;
        }
        pos += de->rec_len;
    }
    return 0;
}

/* Remove a named entry from directory (zero out inode field). Returns 0 if found. */
static int tmpfs_dir_remove(tmpfs_inode_t *dir, const char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    uint64_t pos = 0;
    while (pos < dir->i_size) {
        uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);
        if (bidx >= TMPFS_MAX_BLOCKS || !dir->i_block[bidx]) break;
        ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)dir->i_block[bidx] + boff);
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            de->inode = 0;  /* mark deleted (standard ext2 tombstone) */
            return 0;
        }
        pos += de->rec_len;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Path resolution
 * --------------------------------------------------------------------- */

/* Walk a relative path from the active mount root. Returns inode number or 0. */
static uint32_t tmpfs_walk_path(const char *rel_path, uint8_t *ftype_out) {
    uint32_t cur_ino = g_tmpfs_active_root;

    /* Empty path = root directory itself */
    if (!rel_path || rel_path[0] == '\0') {
        if (ftype_out) *ftype_out = 2;  /* dir */
        return cur_ino;
    }

    char component[TMPFS_NAME_MAX + 1];
    const char *p = rel_path;

    while (*p) {
        /* Extract next component up to '/' or end */
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len == 0 || len > TMPFS_NAME_MAX) return 0;
        memcpy(component, p, (size_t)len);
        component[len] = '\0';
        p += len;
        if (*p == '/') p++;

        tmpfs_inode_t *dir = tmpfs_inode_get(cur_ino);
        if (!dir || !(dir->i_mode & EXT2_S_IFDIR)) return 0;

        uint8_t ftype = 0;
        uint32_t child = tmpfs_dir_find(dir, component, &ftype);
        if (!child) return 0;
        cur_ino = child;
        if (!*p && ftype_out) *ftype_out = ftype;
    }
    return cur_ino;
}

/* -----------------------------------------------------------------------
 * vfs_ops_t implementations
 * --------------------------------------------------------------------- */

static int tmpfs_lookup(const char *path, vfs_inode_info_t *out) {
    uint8_t ftype = 0;
    uint32_t ino = tmpfs_walk_path(path, &ftype);
    if (!ino) return -1;
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n) return -1;
    out->inode       = ino;
    out->size        = n->i_size;
    out->device_type = n->device_type;
    out->dev         = n->i_rdev;
    out->mode        = n->i_mode & 0x1FF;
    {
        uint16_t type_bits = n->i_mode & 0xF000;
        if (type_bits == EXT2_S_IFDIR)
            out->file_type = VFS_FILE_TYPE_DIR;
        else if (type_bits == EXT2_S_IFLNK)
            out->file_type = VFS_FILE_TYPE_SYMLINK;
        else if (type_bits == EXT2_S_IFCHR || n->device_type != VFS_DEVICE_NONE)
            out->file_type = VFS_FILE_TYPE_CHAR;
        else if (type_bits == EXT2_S_IFBLK)
            out->file_type = VFS_FILE_TYPE_BLK;
        else
            out->file_type = VFS_FILE_TYPE_REG;
    }
    return 0;
}

static int tmpfs_read(uint32_t ino, uint64_t off, void *buf, uint32_t len) {
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n || !(n->i_mode & EXT2_S_IFREG)) return -1;
    if (off >= n->i_size) return 0;  /* EOF */
    if (off + len > n->i_size) len = (uint32_t)(n->i_size - off);

    uint32_t bytes_read = 0;
    while (bytes_read < len) {
        uint32_t bidx = (uint32_t)((off + bytes_read) / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)((off + bytes_read) % TMPFS_BLOCK_SIZE);
        uint32_t can  = TMPFS_BLOCK_SIZE - boff;
        if (can > len - bytes_read) can = len - bytes_read;
        if (bidx >= TMPFS_MAX_BLOCKS || !n->i_block[bidx]) {
            /* Sparse block — return zeros */
            memset((uint8_t *)buf + bytes_read, 0, can);
        } else {
            memcpy((uint8_t *)buf + bytes_read,
                   (uint8_t *)n->i_block[bidx] + boff, can);
        }
        bytes_read += can;
    }
    return (int)bytes_read;
}

static int tmpfs_write(uint32_t ino, uint64_t off, const void *buf, uint32_t len) {
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n || !(n->i_mode & EXT2_S_IFREG)) return -1;

    uint32_t bytes_written = 0;
    while (bytes_written < len) {
        uint32_t bidx = (uint32_t)((off + bytes_written) / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)((off + bytes_written) % TMPFS_BLOCK_SIZE);
        uint32_t can  = TMPFS_BLOCK_SIZE - boff;
        if (can > len - bytes_written) can = len - bytes_written;
        void *blk = tmpfs_get_or_alloc_block(n, bidx);
        if (!blk) return (bytes_written > 0) ? (int)bytes_written : -1;
        memcpy((uint8_t *)blk + boff, (const uint8_t *)buf + bytes_written, can);
        bytes_written += can;
    }
    uint64_t new_end = off + bytes_written;
    if (new_end > n->i_size) n->i_size = new_end;
    return (int)bytes_written;
}

static int tmpfs_readdir(uint32_t ino, uint64_t *offset,
                          vfs_dirent_cb_t cb, void *ud) {
    tmpfs_inode_t *dir = tmpfs_inode_get(ino);
    if (!dir || !(dir->i_mode & EXT2_S_IFDIR)) return -1;

    uint64_t pos = *offset;
    while (pos < dir->i_size) {
        uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);
        if (bidx >= TMPFS_MAX_BLOCKS || !dir->i_block[bidx]) break;
        ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)dir->i_block[bidx] + boff);
        if (de->rec_len == 0) break;
        if (de->inode != 0) {
            /* Emit entry: file_type 1=reg, 2=dir (matches VFS_FILE_TYPE_REG/DIR) */
            int rc = cb(de->name, de->name_len, de->inode, de->file_type, ud);
            if (rc != 0) { *offset = pos + de->rec_len; return rc; }
        }
        pos += de->rec_len;
    }
    *offset = pos;
    return 0;
}

static int tmpfs_flush(uint32_t ino) {
    (void)ino;
    return 0;  /* no persistence; no-op */
}

static void tmpfs_set_active_root(uint32_t ino) {
    g_tmpfs_active_root = ino ? ino : TMPFS_ROOT_INO;
}

static int tmpfs_truncate(uint32_t ino, uint64_t new_size) {
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n || !(n->i_mode & EXT2_S_IFREG)) return -1;
    if (new_size > (uint64_t)TMPFS_MAX_BLOCKS * TMPFS_BLOCK_SIZE)
        return -27; /* EFBIG */
    if (new_size < (uint64_t)n->i_size) {
        uint32_t keep = (uint32_t)((new_size + TMPFS_BLOCK_SIZE - 1) / TMPFS_BLOCK_SIZE);
        for (int b = (int)keep; b < TMPFS_MAX_BLOCKS; b++) {
            if (n->i_block[b]) { kfree(n->i_block[b]); n->i_block[b] = NULL; }
        }
        if (new_size > 0 && (new_size % TMPFS_BLOCK_SIZE) != 0 && keep > 0) {
            uint32_t last = keep - 1;
            if (n->i_block[last]) {
                uint32_t tail = (uint32_t)(new_size % TMPFS_BLOCK_SIZE);
                uint8_t *blk  = (uint8_t *)n->i_block[last];
                for (uint32_t j = tail; j < TMPFS_BLOCK_SIZE; j++) blk[j] = 0;
            }
        }
        n->i_block_count = keep;
    }
    /* Extend: sparse — unallocated blocks read as zero on demand */
    n->i_size = (uint32_t)new_size;
    return 0;
}

/**
 * tmpfs_readlink() - Read the stored symlink_target of a tmpfs symlink inode.
 * @ino: Inode number (must have i_mode & EXT2_S_IFLNK set).
 * @buf: Output buffer. NOT null-terminated (POSIX readlink semantics).
 * @len: Maximum bytes to copy.
 * @return: Number of bytes copied (>= 0), or -1 if not a symlink or error.
 */
static int tmpfs_readlink(uint32_t ino, char *buf, uint32_t len)
{
    if (!buf || len == 0) return -1;
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n) return -1;
    if ((n->i_mode & 0xF000) != EXT2_S_IFLNK) return -1;

    uint32_t tlen = 0;
    while (n->symlink_target[tlen] && tlen < 255) tlen++;
    if (tlen == 0) return 0;

    uint32_t copy_len = (tlen < len) ? tlen : len;
    for (uint32_t i = 0; i < copy_len; i++)
        buf[i] = n->symlink_target[i];
    return (int)copy_len;
}

/**
 * tmpfs_symlink() - Create a symlink in a tmpfs directory.
 * @parent_ino: Parent directory inode number.
 * @name: Symlink filename (null-terminated; max TMPFS_NAME_MAX chars).
 * @target: Target string (null-terminated; max 255 chars stored).
 * @return: 0 on success, -1 on error (name too long, inode table full, etc.).
 */
static int tmpfs_symlink(uint32_t parent_ino, const char *name, const char *target)
{
    if (!name || !target) return -1;

    uint32_t tlen = 0;
    while (target[tlen] && tlen < 255) tlen++;
    if (tlen == 0) return -1;

    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent) return -1;

    tmpfs_inode_t *n = tmpfs_alloc_inode((uint16_t)(EXT2_S_IFLNK | 0777));
    if (!n) return -1;

    /* Store target string */
    for (uint32_t i = 0; i < tlen; i++)
        n->symlink_target[i] = target[i];
    n->symlink_target[tlen] = '\0';
    n->i_size = tlen;

    /* Add directory entry in parent (file_type 7 = EXT2_FT_SYMLINK) */
    if (tmpfs_dir_append(parent, n->ino, name, 7) != 0) {
        tmpfs_free_inode(n);
        return -1;
    }
    return 0;
}

static int tmpfs_chmod(uint32_t ino, uint16_t mode) {
    tmpfs_inode_t *n = tmpfs_inode_get(ino);
    if (!n) return -1;
    /* Preserve file type bits; replace only the low 12 permission bits */
    n->i_mode = (n->i_mode & 0xF000) | (mode & 0x0FFF);
    return 0;
}

static int tmpfs_rename(uint32_t old_parent_ino, const char *old_name,
                        uint32_t new_parent_ino, const char *new_name)
{
    if (!old_name || !old_name[0] || !new_name || !new_name[0])
        return -22;

    tmpfs_inode_t *old_parent = tmpfs_inode_get(old_parent_ino);
    tmpfs_inode_t *new_parent = tmpfs_inode_get(new_parent_ino);
    if (!old_parent || !new_parent) return -22;
    if (!(old_parent->i_mode & EXT2_S_IFDIR) || !(new_parent->i_mode & EXT2_S_IFDIR))
        return -20;

    uint8_t old_ftype = 0;
    uint32_t moved_ino = tmpfs_dir_find(old_parent, old_name, &old_ftype);
    if (!moved_ino) return -2;  /* ENOENT */

    uint8_t new_ftype = 0;
    uint32_t existing_ino = tmpfs_dir_find(new_parent, new_name, &new_ftype);
    if (existing_ino) {
        if (existing_ino == moved_ino) return 0;  /* rename to self */
        int old_is_dir = (old_ftype == 2);
        int new_is_dir = (new_ftype == 2);
        if (old_is_dir && !new_is_dir) return -20;  /* ENOTDIR */
        if (!old_is_dir && new_is_dir) return -21;  /* EISDIR */
        if (new_is_dir) return -39;                 /* ENOTEMPTY: refuse dir overwrite */
        tmpfs_inode_t *existing = tmpfs_inode_get(existing_ino);
        tmpfs_dir_remove(new_parent, new_name);
        if (existing) tmpfs_free_inode(existing);
    }

    tmpfs_dir_remove(old_parent, old_name);
    if (tmpfs_dir_append(new_parent, moved_ino, new_name, old_ftype) != 0)
        return -1;

    /* Update ".." in moved directory when crossing directories */
    if (old_ftype == 2 && old_parent_ino != new_parent_ino) {
        tmpfs_inode_t *moved = tmpfs_inode_get(moved_ino);
        if (moved) {
            uint64_t pos = 0;
            while (pos < moved->i_size) {
                uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
                uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);
                if (bidx >= TMPFS_MAX_BLOCKS || !moved->i_block[bidx]) break;
                ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)moved->i_block[bidx] + boff);
                if (de->rec_len == 0) break;
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                    de->inode = new_parent_ino;
                    break;
                }
                pos += de->rec_len;
            }
        }
    }

    return 0;
}

static int tmpfs_statfs(vfs_statfs_t *out)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < TMPFS_MAX_INODES; i++)
        if (g_tmpfs_inodes[i].ino != 0) used++;
    /* Synthetic: report 256 MiB total RAM-disk, 4 KiB blocks */
    out->f_type    = 0x01021994UL; /* TMPFS_MAGIC */
    out->f_bsize   = 4096;
    out->f_blocks  = (256UL * 1024 * 1024) / 4096;
    out->f_bfree   = out->f_blocks;  /* tmpfs never runs out in this impl */
    out->f_bavail  = out->f_blocks;
    out->f_files   = TMPFS_MAX_INODES;
    out->f_ffree   = TMPFS_MAX_INODES - used;
    out->f_namelen = 255;
    return 0;
}

static vfs_ops_t g_tmpfs_ops = {
    .lookup   = tmpfs_lookup,
    .read     = tmpfs_read,
    .readdir  = tmpfs_readdir,
    .write    = tmpfs_write,
    .flush    = tmpfs_flush,
    .create   = tmpfs_create,
    .unlink   = tmpfs_unlink,
    .rename   = tmpfs_rename,
    .mkdir    = tmpfs_mkdir,
    .rmdir    = tmpfs_rmdir,
    .set_root  = tmpfs_set_active_root,
    .truncate  = tmpfs_truncate,
    .readlink  = tmpfs_readlink,
    .symlink   = tmpfs_symlink,
    .mknod     = tmpfs_mknod,
    .chmod     = tmpfs_chmod,
    .statfs    = tmpfs_statfs,
};

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

static int tmpfs_fs_mount(const char *source, const char *target, const void *data) {
    (void)source; (void)data;
    tmpfs_init();
    uint32_t root = tmpfs_alloc_root();
    if (!root)
        return -12;  /* ENOMEM */
    if (vfs_register_mount(target, &g_tmpfs_ops, root) < 0)
        return -28;  /* ENOSPC — mount table full */
    return 0;
}

void tmpfs_init(void) {
    if (g_tmpfs_initialized) return;
    memset(g_tmpfs_inodes, 0, sizeof(g_tmpfs_inodes));
    g_tmpfs_next_ino = TMPFS_ROOT_INO;

    /* Allocate root directory (inode 1) */
    tmpfs_inode_t *root = &g_tmpfs_inodes[TMPFS_ROOT_INO];
    root->ino           = TMPFS_ROOT_INO;
    root->i_mode        = (uint16_t)(EXT2_S_IFDIR | 0755);
    root->device_type   = VFS_DEVICE_NONE;
    root->i_size        = 0;
    root->i_links_count = 2;  /* "." + parent ref */
    root->i_block_count = 0;
    for (int b = 0; b < TMPFS_MAX_BLOCKS; b++) root->i_block[b] = NULL;
    g_tmpfs_next_ino = TMPFS_ROOT_INO + 1;

    /* Add "." and ".." to root */
    tmpfs_dir_append(root, TMPFS_ROOT_INO, ".", 2);
    tmpfs_dir_append(root, TMPFS_ROOT_INO, "..", 2);

    g_tmpfs_initialized = 1;
    register_filesystem("tmpfs", tmpfs_fs_mount);
    printk("tmpfs: initialized, root inode=%u\n", TMPFS_ROOT_INO);
}

vfs_ops_t *tmpfs_get_ops(void) {
    return &g_tmpfs_ops;
}

uint32_t tmpfs_alloc_root(void) {
    tmpfs_inode_t *n = tmpfs_alloc_inode((uint16_t)(EXT2_S_IFDIR | 0755));
    if (!n) return 0;
    n->i_links_count = 2;
    tmpfs_dir_append(n, n->ino, ".", 2);
    tmpfs_dir_append(n, n->ino, "..", 2);
    return n->ino;
}

int tmpfs_create(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino_out) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -1;
    if (!name || strlen(name) == 0 || strlen(name) > TMPFS_NAME_MAX) return -1;

    /* Check no duplicate */
    if (tmpfs_dir_find(parent, name, NULL) != 0) return -1;

    tmpfs_inode_t *n = tmpfs_alloc_inode((uint16_t)(EXT2_S_IFREG | (mode ? (mode & 0777) : 0644)));
    if (!n) return -1;
    if (tmpfs_dir_append(parent, n->ino, name, 1) != 0) {
        tmpfs_free_inode(n); return -1;
    }
    *new_ino_out = n->ino;
    return 0;
}

int tmpfs_create_device(uint32_t parent_ino, const char *name, uint8_t device_type,
                        uint32_t *new_ino_out) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -1;
    if (!name || strlen(name) == 0 || strlen(name) > TMPFS_NAME_MAX) return -1;
    if (device_type == VFS_DEVICE_NONE) return -1;
    if (tmpfs_dir_find(parent, name, NULL) != 0) return -1;

    tmpfs_inode_t *n = tmpfs_alloc_inode((uint16_t)(EXT2_S_IFREG | 0644));
    if (!n) return -1;
    n->device_type = device_type;
    if (tmpfs_dir_append(parent, n->ino, name, 1) != 0) {
        tmpfs_free_inode(n);
        return -1;
    }
    *new_ino_out = n->ino;
    return 0;
}

int tmpfs_mknod(uint32_t parent_ino, const char *name, uint16_t mode,
                uint32_t dev, uint32_t *new_ino_out) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -1;
    if (!name || strlen(name) == 0 || strlen(name) > TMPFS_NAME_MAX) return -1;
    if (tmpfs_dir_find(parent, name, NULL) != 0) return -1;

    uint16_t type_bits = mode & 0xF000;
    if (type_bits != EXT2_S_IFCHR && type_bits != EXT2_S_IFBLK) return -22;

    tmpfs_inode_t *n = tmpfs_alloc_inode(mode);
    if (!n) return -1;
    n->i_rdev = dev;

    /* Map well-known (major, minor) pairs to VFS_DEVICE_* for dispatch */
    if (type_bits == EXT2_S_IFCHR) {
        uint32_t major = (dev >> 8) & 0xFF;
        uint32_t minor = dev & 0xFF;
        if (major == 5 && minor == 0)
            n->device_type = VFS_DEVICE_TTY;    /* /dev/tty */
        else if (major == 1 && minor == 3)
            n->device_type = VFS_DEVICE_NULL;   /* /dev/null */
        /* else: device_type stays VFS_DEVICE_NONE; node exists but I/O returns ENXIO */
    }

    uint8_t ft = (type_bits == EXT2_S_IFCHR) ? EXT2_FT_CHRDEV : EXT2_FT_BLKDEV;
    if (tmpfs_dir_append(parent, n->ino, name, ft) != 0) {
        tmpfs_free_inode(n);
        return -1;
    }
    *new_ino_out = n->ino;
    return 0;
}

int tmpfs_mkdir(uint32_t parent_ino, const char *name, uint32_t *new_ino_out) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -1;
    if (!name || strlen(name) == 0 || strlen(name) > TMPFS_NAME_MAX) return -1;
    if (tmpfs_dir_find(parent, name, NULL) != 0) return -1;

    tmpfs_inode_t *n = tmpfs_alloc_inode((uint16_t)(EXT2_S_IFDIR | 0755));
    if (!n) return -1;
    n->i_links_count = 2;

    /* Add "." and ".." */
    tmpfs_dir_append(n, n->ino,    ".",  2);
    tmpfs_dir_append(n, parent_ino, "..", 2);

    if (tmpfs_dir_append(parent, n->ino, name, 2) != 0) {
        tmpfs_free_inode(n); return -1;
    }
    parent->i_links_count++;
    *new_ino_out = n->ino;
    return 0;
}

int tmpfs_unlink(uint32_t parent_ino, const char *name) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -20;

    uint8_t ftype = 0;
    uint32_t child_ino = tmpfs_dir_find(parent, name, &ftype);
    if (!child_ino) return -2;
    if (ftype == 2) return -21;  /* EISDIR: use rmdir for directories */

    tmpfs_inode_t *child = tmpfs_inode_get(child_ino);
    tmpfs_dir_remove(parent, name);
    if (child) tmpfs_free_inode(child);
    return 0;
}

int tmpfs_rmdir(uint32_t parent_ino, const char *name) {
    tmpfs_inode_t *parent = tmpfs_inode_get(parent_ino);
    if (!parent || !(parent->i_mode & EXT2_S_IFDIR)) return -20;

    uint8_t ftype = 0;
    uint32_t child_ino = tmpfs_dir_find(parent, name, &ftype);
    if (!child_ino) return -2;
    if (ftype != 2) return -20;

    tmpfs_inode_t *child = tmpfs_inode_get(child_ino);
    if (!child) return -2;

    /* Verify empty: only "." and ".." allowed */
    uint64_t pos = 0;
    int non_dot_count = 0;
    while (pos < child->i_size) {
        uint32_t bidx = (uint32_t)(pos / TMPFS_BLOCK_SIZE);
        uint32_t boff = (uint32_t)(pos % TMPFS_BLOCK_SIZE);
        if (bidx >= TMPFS_MAX_BLOCKS || !child->i_block[bidx]) break;
        ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)child->i_block[bidx] + boff);
        if (de->rec_len == 0) break;
        if (de->inode != 0) {
            /* Skip "." and ".." */
            if (!(de->name_len == 1 && de->name[0] == '.') &&
                !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))
                non_dot_count++;
        }
        pos += de->rec_len;
    }
    if (non_dot_count > 0) return -39;  /* ENOTEMPTY */

    tmpfs_dir_remove(parent, name);
    parent->i_links_count--;
    tmpfs_free_inode(child);
    return 0;
}
