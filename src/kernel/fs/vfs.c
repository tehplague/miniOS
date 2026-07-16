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

#include <miniOS/fs/vfs.h>
#include <miniOS/fs/ext2.h>
#include <miniOS/fs/pipe.h>
#include <miniOS/drivers/tty.h>
#include <miniOS/drivers/ata.h>
#include <miniOS/sched/sched.h>
#include <miniOS/io.h>
#include <string.h>

static vfs_mount_t g_mounts[VFS_MAX_MOUNTS];
static int g_num_mounts = 0;

static filesystem_type_t g_fs_types[VFS_MAX_FS_TYPES];
static int g_num_fs_types = 0;

int register_filesystem(const char *name,
                        int (*mount)(const char *source, const char *target,
                                     const void *data)) {
    if (!name || !mount || g_num_fs_types >= VFS_MAX_FS_TYPES) return -1;
    filesystem_type_t *ft = &g_fs_types[g_num_fs_types++];
    strncpy(ft->name, name, sizeof(ft->name) - 1);
    ft->name[sizeof(ft->name) - 1] = '\0';
    ft->mount = mount;
    return 0;
}

int vfs_mount_fstype(const char *fstype, const char *source,
                     const char *target, const void *data) {
    for (int i = 0; i < g_num_fs_types; i++) {
        if (strncmp(g_fs_types[i].name, fstype, sizeof(g_fs_types[i].name)) == 0) {
            int old = g_num_mounts;
            int ret = g_fs_types[i].mount(source, target, data);
            if (ret == 0 && g_num_mounts > old) {
                vfs_mount_t *m = &g_mounts[g_num_mounts - 1];
                strncpy(m->fstype, fstype, sizeof(m->fstype) - 1);
                const char *dev = (source && source[0]) ? source : fstype;
                strncpy(m->device, dev, sizeof(m->device) - 1);
            }
            return ret;
        }
    }
    return -22;  /* EINVAL: unknown filesystem type */
}

int vfs_mount_count(void) { return g_num_mounts; }

const vfs_mount_t *vfs_get_mount(int idx) {
    if (idx < 0 || idx >= g_num_mounts) return NULL;
    return &g_mounts[idx];
}

/**
 * vfs_abs_path() - Expand a potentially-relative path to absolute using thread CWD.
 * If @path starts with '/', copies it verbatim to @buf.
 * Otherwise, prepends the calling thread's cwd (with '/' separator as needed).
 * @path: Input path (absolute or relative).
 * @buf:  Output buffer of VFS_PATH_MAX bytes.
 * @return: 0 on success, -36 (ENAMETOOLONG) if result exceeds VFS_PATH_MAX-1.
 */
static int vfs_abs_path(const char *path, char *buf) {
    if (path[0] == '/') {
        int n = 0;
        while (path[n] && n < VFS_PATH_MAX - 1) { buf[n] = path[n]; n++; }
        buf[n] = '\0';
        return 0;
    }
    struct thread *t = sched_current();
    size_t cwd_len = strlen(t->cwd);
    int sep = (cwd_len > 0 && t->cwd[cwd_len - 1] != '/') ? 1 : 0;
    int r = snprintf(buf, VFS_PATH_MAX, "%s%s%s", t->cwd, sep ? "/" : "", path);
    if (r < 0 || r >= (int)VFS_PATH_MAX) return -36;
    return 0;
}

/**
 * vfs_find_mount() - Find the deepest mount point that is a prefix of path.
 * Uses longest-prefix-match. Boundary check: next char after mount point must
 * be '/' or '\0' to prevent "/tmp" matching "/tmpfoo".
 * @path:         Absolute path to resolve.
 * @relative_out: Set to the subpath after the mount point (leading '/' stripped).
 * @return: Pointer to matching vfs_mount_t, or NULL if no match.
 */
static vfs_mount_t *vfs_find_mount(const char *path, const char **relative_out) {
    vfs_mount_t *best = NULL;
    int best_len = 0;
    for (int i = 0; i < g_num_mounts; i++) {
        const char *mp = g_mounts[i].mount_point;
        int mp_len = (int)strlen(mp);
        if (strncmp(path, mp, (size_t)mp_len) == 0) {
            /* Boundary check: the character after the mount prefix must be
             * '/', '\0', or the mount point must be "/" (root always matches).
             * This prevents "/tmp" from matching "/tmpfoo". */
            char next = path[mp_len];
            int is_root = (mp_len == 1 && mp[0] == '/');
            if (is_root || next == '/' || next == '\0') {
                if (mp_len > best_len) {
                    best = &g_mounts[i];
                    best_len = mp_len;
                }
            }
        }
    }
    if (!best) return NULL;
    *relative_out = path + best_len;
    if (**relative_out == '/') (*relative_out)++;
    return best;
}

int vfs_path_devno(const char *path) {
    const char *rel;
    vfs_mount_t *mnt = vfs_find_mount(path, &rel);
    if (!mnt) return 0;
    return (int)(mnt - g_mounts) + 1;
}

int vfs_fd_devno(int fd) {
    struct thread *t = sched_current();
    if (fd < 0 || fd >= VFS_MAX_FDS || !THREAD_FDT(t)[fd].in_use) return 0;
    vfs_ops_t *ops = THREAD_FDT(t)[fd].ops;
    for (int i = 0; i < g_num_mounts; i++)
        if (g_mounts[i].ops == ops) return i + 1;
    return 0;
}

int vfs_statfs(const char *path, vfs_statfs_t *out)
{
    if (!path || !out) return -22;
    const char *relative;
    vfs_mount_t *mnt = vfs_find_mount(path, &relative);
    if (!mnt) return -2;
    if (!mnt->ops || !mnt->ops->statfs) return -38;
    int r = mnt->ops->statfs(out);
    if (r == 0)
        out->f_fsid = (uint64_t)(mnt - g_mounts);
    return r;
}

int vfs_fstatfs(int fd, vfs_statfs_t *out)
{
    if (!out) return -22;
    struct thread *t = sched_current();
    if (fd < 0 || fd >= VFS_MAX_FDS || !THREAD_FDT(t)[fd].in_use) return -9;
    vfs_ops_t *ops = THREAD_FDT(t)[fd].ops;
    if (!ops || !ops->statfs) return -38; /* ENOSYS */
    int r = ops->statfs(out);
    if (r == 0) {
        for (int i = 0; i < g_num_mounts; i++) {
            if (g_mounts[i].ops == ops) { out->f_fsid = (uint64_t)i; break; }
        }
    }
    return r;
}

/**
 * vfs_register_mount() - Register a filesystem at a mount point.
 * @point: Absolute mount point path (e.g. "/", "/tmp"). Copied into mount table.
 * @ops:   Pointer to vfs_ops_t with at least lookup, read, readdir set.
 *
 * Stores entry in g_mounts[]. Up to VFS_MAX_MOUNTS=8 mounts supported.
 * @return: 0 on success, -1 if mount table is full.
 */
int vfs_register_mount(const char *point, vfs_ops_t *ops, uint32_t root_ino) {
    if (!point || !ops || g_num_mounts >= VFS_MAX_MOUNTS) return -1;
    vfs_mount_t *m = &g_mounts[g_num_mounts++];
    strncpy(m->mount_point, point, VFS_PATH_MAX - 1);
    m->mount_point[VFS_PATH_MAX - 1] = '\0';
    m->ops = ops;
    m->root_ino = root_ino;
    printk("VFS: mounted %s\n", point);
    return 0;
}

/**
 * vfs_mount() - Store the filesystem ops table at root "/" (backwards-compat shim).
 * @ops: Pointer to filesystem ops (must have lookup, read, readdir set).
 *
 * Calls vfs_register_mount("/", ops). Kept for backwards compatibility.
 */
void vfs_mount(vfs_ops_t *ops) {
    vfs_register_mount("/", ops, 0);
}

/* Activate the per-mount root before any ops->lookup call */
static inline void vfs_activate_root(const vfs_mount_t *mnt) {
    if (mnt->ops->set_root) mnt->ops->set_root(mnt->root_ino);
}

/* Maximum symlink hops before returning -ELOOP */
#define VFS_ELOOP 40
#define VFS_MAX_SYMLINK_DEPTH 8

/**
 * vfs_resolve_path() - Resolve a path, following symlinks up to VFS_MAX_SYMLINK_DEPTH hops.
 * @path:     Absolute input path to resolve.
 * @out_path: Output buffer (VFS_PATH_MAX bytes) filled with the final resolved path.
 * @out_info: Populated with final inode metadata on success.
 * @depth:    Current hop depth (caller passes 0).
 * @return: 0 on success, -2 (ENOENT) if path not found, -40 (ELOOP) on cycle.
 */
static int vfs_resolve_path(const char *path, char *out_path,
                            vfs_inode_info_t *out_info, int depth)
{
    if (depth > VFS_MAX_SYMLINK_DEPTH) return -VFS_ELOOP;

    const char *relative;
    vfs_mount_t *mnt = vfs_find_mount(path, &relative);
    if (!mnt) return -2;

    vfs_inode_info_t info = {0};
    vfs_activate_root(mnt);
    if (mnt->ops->lookup(relative, &info) != 0) return -2;

    if (info.file_type != VFS_FILE_TYPE_SYMLINK) {
        /* Not a symlink: copy path to out_path and return info */
        int n = 0;
        while (path[n] && n < VFS_PATH_MAX - 1) { out_path[n] = path[n]; n++; }
        out_path[n] = '\0';
        *out_info = info;
        return 0;
    }

    /* Symlink: read the target string */
    if (!mnt->ops->readlink) return -2;
    char target_buf[VFS_PATH_MAX];
    int tlen = mnt->ops->readlink(info.inode, target_buf, VFS_PATH_MAX - 1);
    if (tlen < 0) return -2;
    target_buf[tlen] = '\0';

    /* Build resolved path from target */
    char resolved[VFS_PATH_MAX];
    if (target_buf[0] == '/') {
        /* Absolute target: use directly */
        int n = 0;
        while (target_buf[n] && n < VFS_PATH_MAX - 1) { resolved[n] = target_buf[n]; n++; }
        resolved[n] = '\0';
    } else {
        /* Relative target: prepend symlink parent directory */
        int last_slash = -1;
        for (int i = 0; path[i]; i++) if (path[i] == '/') last_slash = i;

        int ri = 0;
        if (last_slash <= 0) {
            resolved[ri++] = '/';
        } else {
            for (int i = 0; i < last_slash && ri < VFS_PATH_MAX - 1; i++)
                resolved[ri++] = path[i];
        }
        if (ri < VFS_PATH_MAX - 1) resolved[ri++] = '/';
        for (int i = 0; target_buf[i] && ri < VFS_PATH_MAX - 1; i++)
            resolved[ri++] = target_buf[i];
        resolved[ri] = '\0';
    }

    return vfs_resolve_path(resolved, out_path, out_info, depth + 1);
}

/* Allocate a free fd slot in the calling task's fd_table */
static int alloc_fd(void) {
    struct thread *t = sched_current();
    /* Scan from 0 so that after close(0) an open() can reclaim fd=0 as
     * stdin (required by POSIX shells like ash for background job
     * stdin-redirect: open("/dev/null") must return 0). */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!t->fd_table[i].in_use) return i;
    }
    return -1;
}

/**
 * vfs_open() - Open or create a file by absolute path.
 * @path: Absolute path.
 * @flags: O_RDONLY (0), O_WRONLY (1), O_CREAT (0x0200). O_CREAT creates file if missing.
 *         O_TRUNC (0x0400): Truncate existing regular file to zero on open.
 * @mode: File permission bits for O_CREAT (lower 9 bits used; 0 → 0644 default).
 *        Write permission on the parent directory is enforced before creation.
 *
 * Resolves path through the mount table using longest-prefix match.
 * If O_CREAT is set and lookup fails: parses the parent path, looks up parent
 * inode, and calls the FS create op to allocate a new inode and directory entry.
 *
 * @return: fd index (VFS_FIRST_OPEN_FD to VFS_MAX_FDS-1), or -1 on error.
 */
int vfs_open(const char *path, int flags, int mode)
{
    uint16_t creat_mode = (uint16_t)(mode & 0777);  /* lower 9 bits; 0 → FS default (0644) */

    if (!path) return -1;

    /* Relative path resolution: if path does not start with '/', prepend cwd */
    const char *resolved_path;
    char full_path[VFS_PATH_MAX];

    if (path[0] != '/') {
        struct thread *t_cwd = sched_current();
        /* cwd is always absolute; if it ends with '/', avoid double slash */
        size_t cwd_len = strlen(t_cwd->cwd);
        int sep_needed = (cwd_len > 0 && t_cwd->cwd[cwd_len - 1] != '/') ? 1 : 0;
        int ret = snprintf(full_path, VFS_PATH_MAX, "%s%s%s",
                           t_cwd->cwd,
                           sep_needed ? "/" : "",
                           path);
        if (ret < 0 || ret >= (int)VFS_PATH_MAX) return -36;  /* ENAMETOOLONG */
        resolved_path = full_path;
    } else {
        resolved_path = path;
    }

    const char *relative_path;
    vfs_mount_t *mnt = vfs_find_mount(resolved_path, &relative_path);
    if (!mnt) { printk("VFS: no mount for: %s\n", resolved_path); return -1; }

    vfs_inode_info_t info = {0};
    vfs_activate_root(mnt);
    int lookup_ret = mnt->ops->lookup(relative_path, &info);
    int found;
    if (lookup_ret != 0) {
        found = 0;
    } else if (info.file_type == VFS_FILE_TYPE_SYMLINK) {
        char sym_resolved[VFS_PATH_MAX];
        vfs_inode_info_t sym_info = {0};
        int sr = vfs_resolve_path(resolved_path, sym_resolved, &sym_info, 0);
        if (sr == -(VFS_ELOOP)) return -(VFS_ELOOP);
        if (sr != 0) { found = 0; }
        else {
            found = 1;
            info = sym_info;
            /* Re-bind mnt to the resolved path's mount */
            const char *sym_rel;
            vfs_mount_t *sym_mnt = vfs_find_mount(sym_resolved, &sym_rel);
            if (sym_mnt) mnt = sym_mnt;
        }
    } else {
        found = 1;
    }

    if (!found) {
        /* File not found: only proceed if O_CREAT is set */
        if (!(flags & 0x0200)) {  /* O_CREAT = 0x0200 in Newlib (not Linux's 0x40) */
            printk("VFS: not found: %s\n", resolved_path);
            return -1;
        }

        /* Parse parent path and filename from resolved_path */
        char parent_path[VFS_PATH_MAX];
        const char *filename = resolved_path;
        int last_slash = -1;

        /* Find last '/' */
        for (int i = 0; resolved_path[i] != '\0' && i < VFS_PATH_MAX - 1; i++) {
            if (resolved_path[i] == '/')
                last_slash = i;
        }

        if (last_slash <= 0) {
            /* Parent is root "/" */
            parent_path[0] = '/';
            parent_path[1] = '\0';
            filename = (last_slash == 0) ? resolved_path + 1 : resolved_path;
        } else {
            for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
                parent_path[i] = resolved_path[i];
            parent_path[last_slash] = '\0';
            filename = resolved_path + last_slash + 1;
        }

        /* Find mount for parent */
        const char *parent_relative;
        vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
        if (!parent_mnt) { printk("VFS: no mount for parent: %s\n", parent_path); return -1; }

        vfs_inode_info_t parent_info = {0};
        vfs_activate_root(parent_mnt);
        if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0) {
            printk("VFS: parent not found: %s\n", parent_path);
            return -1;
        }

        /* Enforce directory write permission before creating a file inside it */
        if (parent_info.mode != 0 && !(parent_info.mode & 0222)) {
            printk("VFS: create denied, dir not writable (mode %04o): %s\n",
                   parent_info.mode, parent_path);
            return -13;  /* EACCES */
        }

        uint32_t new_ino = 0;
        if (!parent_mnt->ops->create) {
            printk("VFS: filesystem at %s has no create op\n", parent_path);
            return -1;
        }
        if (parent_mnt->ops->create(parent_info.inode, filename, creat_mode, &new_ino) != 0) {
            printk("VFS: create failed for %s\n", resolved_path);
            return -1;
        }
        info.inode = new_ino;
        info.file_type = VFS_FILE_TYPE_REG;
        info.device_type = VFS_DEVICE_NONE;
        info.size = 0;

    } else {
        /* File exists. Check O_TRUNC: truncate to zero if requested.
         * O_TRUNC = 0x0400 (Newlib _FTRUNC value, verified in
         * sysroot/usr/x86_64-elf/include/sys/_default_fcntl.h line 19).
         * Only truncate regular files — skip silently for dirs.
         * (O_EXCL is not implemented; opening existing file with O_CREAT is fine) */
        if ((flags & 0x0400) && info.file_type == VFS_FILE_TYPE_REG) {
            if (!mnt->ops->truncate || mnt->ops->truncate(info.inode, 0) != 0) {
                printk("VFS: O_TRUNC failed for %s\n", resolved_path);
                return -1;
            }
            info.size = 0;  /* update cached size so fd_table.size = 0 */
        }
    }

    /* Permission check: enforce mode bits when the FS populated them (mode != 0).
     * Write access (O_WRONLY=1 or O_RDWR=2) requires at least one write bit.
     * All processes are treated as root — root is still blocked from writing
     * a file with no write bits (e.g. mode 0444). */
    if (info.mode != 0 && (flags & 3) != 0) {
        if (!(info.mode & 0222)) {
            printk("VFS: write denied (mode %04o): %s\n", info.mode, resolved_path);
            return -13;  /* EACCES */
        }
    }

    int fd = alloc_fd();
    if (fd < 0) {
        printk("VFS: fd table full\n");
        return -1;
    }

    struct thread *t = sched_current();
    vfs_file_t *f = &THREAD_FDT(t)[fd];
    f->in_use = 1; f->inode = info.inode; f->offset = 0;
    f->size = info.size; f->ops = mnt->ops; f->ftype = info.file_type; f->flags = flags;
    f->device_type = info.device_type;
    f->dev         = info.dev;
    return fd;
}

/**
 * vfs_read() - Dispatch a read to the mounted filesystem.
 * @fd: Open file descriptor.
 * @buf: Destination buffer.
 * @len: Byte count requested.
 *
 * Validates fd in range and in_use. Calls fd_table[fd].ops->read() with
 * the stored inode and current offset. Advances offset by bytes_read.
 *
 * @return: Bytes read, 0 at EOF, -1 on bad fd or read error.
 */
int vfs_read(int fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    struct thread *t = sched_current();
    vfs_file_t *f = &THREAD_FDT(t)[fd];
    if (!f->in_use) return -1;
    /* Char device dispatch before ops check: fds 0-2 are TTY with ops=NULL */
    if (f->ftype == VFS_FILE_TYPE_CHAR) {
        if (f->device_type == VFS_DEVICE_TTY)
            return tty_read(buf, len, (f->status_flags & 0x800) != 0);
        if (f->device_type == VFS_DEVICE_NULL)
            return 0;
        return -1;
    }
    if (!f->ops) return -1;
    /* Pipe dispatch: pass pipe_t pointer as ino (no offset for pipes) */
    if (f->ftype == VFS_FILE_TYPE_PIPE && f->pipe) {
        return f->ops->read((uint32_t)(uintptr_t)f->pipe, 0, buf, len);
    }
    if (f->ftype != VFS_FILE_TYPE_REG) return -1;
    int n = f->ops->read(f->inode, f->offset, buf, len);
    if (n > 0) f->offset += (uint64_t)n;
    return n;
}

/**
 * vfs_write() - Dispatch a write to the mounted filesystem.
 * @fd: Open file descriptor with O_WRONLY flag.
 * @buf: Source buffer.
 * @len: Byte count to write.
 *
 * Validates fd. Calls fd_table[fd].ops->write() with the stored inode and
 * current offset. Advances offset and updates size if write extends file.
 *
 * @return: Bytes written, or -1 on bad fd or write error.
 */
int vfs_write(int fd, const void *buf, uint32_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    struct thread *t = sched_current();
    vfs_file_t *f = &THREAD_FDT(t)[fd];
    if (!f->in_use) return -1;
    /* Char device dispatch before ops check: fds 0-2 are TTY with ops=NULL */
    if (f->ftype == VFS_FILE_TYPE_CHAR) {
        if (f->device_type == VFS_DEVICE_TTY)
            return tty_write(buf, len);
        if (f->device_type == VFS_DEVICE_NULL)
            return (int)len;
        return -1;
    }
    if (!f->ops) return -1;
    /* Pipe dispatch: pass pipe_t pointer as ino (no offset for pipes) */
    if (f->ftype == VFS_FILE_TYPE_PIPE && f->pipe) {
        return f->ops->write((uint32_t)(uintptr_t)f->pipe, 0, buf, len);
    }
    if (!f->ops->write) return -1;
    if (f->ftype != VFS_FILE_TYPE_REG) return -1;
    int n = f->ops->write(f->inode, f->offset, buf, len);
    if (n > 0) {
        f->offset += (uint64_t)n;
        if (f->offset > f->size)
            f->size = f->offset;
    }
    return n;
}

/**
 * vfs_readdir() - Dispatch directory iteration to the mounted filesystem.
 * @fd: Open file descriptor (must be a directory; ftype == VFS_FILE_TYPE_DIR).
 * @cb: Entry callback.
 * @ud: Opaque user data for @cb.
 *
 * Validates fd and ftype. Calls fd_table[fd].ops->readdir() with the stored
 * inode, forwarding @cb and @ud.
 *
 * @return: 0 when all entries visited, non-zero from @cb, or -1 on bad fd.
 */
int vfs_readdir(int fd, vfs_dirent_cb_t cb, void *ud) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    struct thread *t = sched_current();
    vfs_file_t *f = &THREAD_FDT(t)[fd];
    if (!f->in_use || !f->ops || f->ftype != VFS_FILE_TYPE_DIR) {
        return -1;
    }
    return f->ops->readdir(f->inode, &f->offset, cb, ud);
}

/**
 * vfs_close() - Release an open file descriptor, flushing write data to disk.
 * @fd: File descriptor to close.
 *
 * If the file was opened with write flags and ops->flush is set, calls
 * ops->flush(inode) to write back the final inode state, then calls ata_flush()
 * to push the ATA write-back cache to stable storage (EXT2W-07).
 *
 * @return: 0 on success, -1 on invalid or already-closed fd.
 */
int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return -1;
    struct thread *t = sched_current();
    vfs_file_t *vf = &THREAD_FDT(t)[fd];
    if (!vf->in_use) return -1;

    /* ref_count > 1: this is a shared reference (dup/dup2); decrement and clear slot */
    if (vf->ref_count > 1) {
        vf->ref_count--;
        vf->in_use = 0;
        return 0;
    }

    /* Last reference (ref_count == 0 means legacy pre-Phase-24 fd, treat as 1) */
    if (vf->ftype == VFS_FILE_TYPE_PIPE && vf->pipe) {
        /* Mark write end closed so readers see EOF after buffer drains */
        if (vf->flags == 1) vf->pipe->write_closed = 1;  /* O_WRONLY = 1 */
        pipe_free(vf->pipe);
        vf->pipe = NULL;
    } else if ((vf->flags & 0x01) && vf->ops && vf->ops->flush) {
        /* Regular writable file: flush to disk */
        vf->ops->flush(vf->inode);
        ata_flush();
    }

    vf->in_use    = 0;
    vf->ops       = NULL;
    vf->inode     = 0;
    vf->offset    = 0;
    vf->size      = 0;
    vf->ftype     = 0;
    vf->device_type = VFS_DEVICE_NONE;
    vf->flags     = 0;
    vf->ref_count = 0;
    vf->pipe      = NULL;
    return 0;
}

/**
 * vfs_stat() - Look up file metadata by absolute path.
 * @path:    Absolute path to query.
 * @out:     Populated with inode, file_type, size on success.
 * @return:  0 on success, -1 if path not found or no mount.
 */
int vfs_stat(const char *path, vfs_inode_info_t *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;
    char resolved[VFS_PATH_MAX];
    int r = vfs_resolve_path(path, resolved, out, 0);
    if (r == -(VFS_ELOOP)) return -(VFS_ELOOP);
    return (r == 0) ? 0 : -1;
}

int vfs_lstat(const char *path, vfs_inode_info_t *out) {
    if (!path || !out) return -1;
    memset(out, 0, sizeof(*out));
    /* Resolve relative paths against CWD (same as vfs_open) */
    const char *abs_path = path;
    char full_path[VFS_PATH_MAX];
    if (path[0] != '/') {
        struct thread *t = sched_current();
        size_t cwd_len = strlen(t->cwd);
        int sep = (cwd_len > 0 && t->cwd[cwd_len - 1] != '/') ? 1 : 0;
        int r = snprintf(full_path, VFS_PATH_MAX, "%s%s%s", t->cwd, sep ? "/" : "", path);
        if (r < 0 || r >= (int)VFS_PATH_MAX) return -1;
        abs_path = full_path;
    }
    const char *relative;
    vfs_mount_t *mnt = vfs_find_mount(abs_path, &relative);
    if (!mnt) { printk("VFS: lstat: no mount for %s\n", abs_path); return -1; }
    vfs_activate_root(mnt);
    return mnt->ops->lookup(relative, out);
}

int vfs_readlink(const char *path, char *buf, uint32_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -1;
    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;
    const char *relative;
    vfs_mount_t *mnt = vfs_find_mount(path, &relative);
    if (!mnt) return -1;
    vfs_inode_info_t info = {0};
    vfs_activate_root(mnt);
    if (mnt->ops->lookup(relative, &info) != 0) return -2;
    if (info.file_type != VFS_FILE_TYPE_SYMLINK) return -22;
    if (!mnt->ops->readlink) return -22;
    return mnt->ops->readlink(info.inode, buf, bufsiz);
}

int vfs_symlink(const char *path, const char *target) {
    if (!path || !target) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;

    char parent_path[VFS_PATH_MAX];
    const char *linkname = path;
    int last_slash = -1;
    for (int i = 0; path[i] && i < VFS_PATH_MAX - 1; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        linkname = (last_slash == 0) ? path + 1 : path;
    } else {
        for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        linkname = path + last_slash + 1;
    }

    const char *parent_relative;
    vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
    if (!parent_mnt) return -1;

    vfs_inode_info_t parent_info = {0};
    vfs_activate_root(parent_mnt);
    if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0) return -1;
    if (!parent_mnt->ops->symlink) return -1;
    return parent_mnt->ops->symlink(parent_info.inode, linkname, target);
}

/**
 * vfs_fstat() - Get file metadata from an open file descriptor.
 * @fd:   Open fd (must be VFS_FIRST_OPEN_FD <= fd < VFS_MAX_FDS, in_use).
 * @out:  Populated with inode, ftype, size on success.
 * @return: 0 on success, -1 on bad fd.
 */
int vfs_fstat(int fd, vfs_inode_info_t *out) {
    if (!out || fd < VFS_FIRST_OPEN_FD || fd >= VFS_MAX_FDS) return -1;
    struct thread *t = sched_current();
    vfs_file_t *f = &THREAD_FDT(t)[fd];
    if (!f->in_use) return -1;
    out->inode     = f->inode;
    out->file_type   = f->ftype;
    out->device_type = f->device_type;
    out->dev         = f->dev;
    out->size        = f->size;
    return 0;
}

/**
 * vfs_unlink() - Delete a file by absolute path.
 * @path: Absolute path of the file to remove (e.g., "/tmp/foo.txt").
 *
 * Resolves the mount point for @path, looks up the parent directory inode,
 * then calls ops->unlink(parent_ino, filename). Immediate deletion.
 * @return: 0 on success, -1 if not found, ops->unlink is NULL, or delete fails.
 */
int vfs_unlink(const char *path) {
    if (!path) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;

    /* Parse parent directory and filename from path */
    char parent_path[VFS_PATH_MAX];
    const char *filename = path;
    int last_slash = -1;
    for (int i = 0; path[i] != '\0' && i < VFS_PATH_MAX - 1; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        filename = (last_slash == 0) ? path + 1 : path;
    } else {
        for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        filename = path + last_slash + 1;
    }

    const char *parent_relative;
    vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
    if (!parent_mnt) { printk("VFS: unlink: no mount for parent %s\n", parent_path); return -1; }

    vfs_inode_info_t parent_info;
    vfs_activate_root(parent_mnt);
    if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0) {
        printk("VFS: unlink: parent not found: %s\n", parent_path);
        return -1;
    }

    /* Follow symlinks in parent path */
    if (parent_info.file_type == VFS_FILE_TYPE_SYMLINK) {
        char sym_resolved[VFS_PATH_MAX];
        vfs_inode_info_t sym_info = {0};
        if (vfs_resolve_path(parent_path, sym_resolved, &sym_info, 0) != 0) return -1;
        parent_info = sym_info;
    }

    if (!parent_mnt->ops->unlink) {
        printk("VFS: unlink: filesystem has no unlink op\n");
        return -1;
    }
    return parent_mnt->ops->unlink(parent_info.inode, filename);
}

int vfs_rmdir(const char *path) {
    if (!path) return -22;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -36;
    path = abs;

    char parent_path[VFS_PATH_MAX];
    const char *filename = path;
    int last_slash = -1;
    for (int i = 0; path[i] != '\0' && i < VFS_PATH_MAX - 1; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        filename = (last_slash == 0) ? path + 1 : path;
    } else {
        for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        filename = path + last_slash + 1;
    }

    const char *parent_relative;
    vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
    if (!parent_mnt) return -2;

    vfs_inode_info_t parent_info;
    vfs_activate_root(parent_mnt);
    if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0)
        return -2;

    if (parent_info.file_type == VFS_FILE_TYPE_SYMLINK) {
        char sym_resolved[VFS_PATH_MAX];
        vfs_inode_info_t sym_info = {0};
        if (vfs_resolve_path(parent_path, sym_resolved, &sym_info, 0) != 0) return -2;
        parent_info = sym_info;
    }

    if (!parent_mnt->ops->rmdir)
        return -38;

    return parent_mnt->ops->rmdir(parent_info.inode, filename);
}

int vfs_mkdir(const char *path, int mode) {
    (void)mode;  /* accepted; tmpfs_mkdir uses hardcoded 0755 */
    if (!path) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;

    char parent_path[VFS_PATH_MAX];
    const char *filename = path;
    int last_slash = -1;
    for (int i = 0; path[i] != '\0' && i < VFS_PATH_MAX - 1; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        filename = (last_slash == 0) ? path + 1 : path;
    } else {
        for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        filename = path + last_slash + 1;
    }

    const char *parent_relative;
    vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
    if (!parent_mnt) { printk("VFS: mkdir: no mount for parent %s\n", parent_path); return -1; }

    vfs_inode_info_t parent_info;
    vfs_activate_root(parent_mnt);
    if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0) {
        printk("VFS: mkdir: parent not found: %s\n", parent_path);
        return -1;
    }

    /* Follow symlinks in parent path */
    if (parent_info.file_type == VFS_FILE_TYPE_SYMLINK) {
        char sym_resolved[VFS_PATH_MAX];
        vfs_inode_info_t sym_info = {0};
        if (vfs_resolve_path(parent_path, sym_resolved, &sym_info, 0) != 0) return -1;
        parent_info = sym_info;
    }

    if (!parent_mnt->ops->mkdir) {
        printk("VFS: mkdir: filesystem has no mkdir op\n");
        return -1;
    }
    uint32_t new_ino = 0;
    return parent_mnt->ops->mkdir(parent_info.inode, filename, &new_ino);
}

int vfs_chmod(const char *path, uint16_t mode) {
    if (!path) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;

    vfs_inode_info_t info = {0};
    const char *rel;
    vfs_mount_t *mnt = vfs_find_mount(abs, &rel);
    if (!mnt) return -1;

    vfs_activate_root(mnt);
    if (mnt->ops->lookup(rel, &info) != 0) return -1;
    if (!mnt->ops->chmod) return -1;
    return mnt->ops->chmod(info.inode, mode);
}

int vfs_mknod(const char *path, uint16_t mode, uint32_t dev) {
    if (!path) return -1;

    uint16_t type_bits = mode & 0xF000;
    if (type_bits != EXT2_S_IFCHR && type_bits != EXT2_S_IFBLK) return -22;

    char abs[VFS_PATH_MAX];
    if (vfs_abs_path(path, abs) != 0) return -1;
    path = abs;

    char parent_path[VFS_PATH_MAX];
    const char *filename = path;
    int last_slash = -1;
    for (int i = 0; path[i] != '\0' && i < VFS_PATH_MAX - 1; i++)
        if (path[i] == '/') last_slash = i;

    if (last_slash <= 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        filename = (last_slash == 0) ? path + 1 : path;
    } else {
        for (int i = 0; i < last_slash && i < VFS_PATH_MAX - 1; i++)
            parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        filename = path + last_slash + 1;
    }

    const char *parent_relative;
    vfs_mount_t *parent_mnt = vfs_find_mount(parent_path, &parent_relative);
    if (!parent_mnt) return -1;

    vfs_inode_info_t parent_info = {0};
    vfs_activate_root(parent_mnt);
    if (parent_mnt->ops->lookup(parent_relative, &parent_info) != 0) return -1;
    if (!parent_mnt->ops->mknod) return -1;

    uint32_t new_ino = 0;
    return parent_mnt->ops->mknod(parent_info.inode, filename, mode, dev, &new_ino);
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -22;

    char old_abs[VFS_PATH_MAX], new_abs[VFS_PATH_MAX];
    if (vfs_abs_path(oldpath, old_abs) != 0) return -36;
    if (vfs_abs_path(newpath, new_abs) != 0) return -36;

    /* Split old path into parent dir + filename */
    char old_parent_path[VFS_PATH_MAX];
    const char *old_name = old_abs;
    int old_last_slash = -1;
    for (int i = 0; old_abs[i] && i < VFS_PATH_MAX - 1; i++)
        if (old_abs[i] == '/') old_last_slash = i;
    if (old_last_slash <= 0) {
        old_parent_path[0] = '/'; old_parent_path[1] = '\0';
        old_name = (old_last_slash == 0) ? old_abs + 1 : old_abs;
    } else {
        for (int i = 0; i < old_last_slash && i < VFS_PATH_MAX - 1; i++)
            old_parent_path[i] = old_abs[i];
        old_parent_path[old_last_slash] = '\0';
        old_name = old_abs + old_last_slash + 1;
    }

    /* Split new path into parent dir + filename */
    char new_parent_path[VFS_PATH_MAX];
    const char *new_name = new_abs;
    int new_last_slash = -1;
    for (int i = 0; new_abs[i] && i < VFS_PATH_MAX - 1; i++)
        if (new_abs[i] == '/') new_last_slash = i;
    if (new_last_slash <= 0) {
        new_parent_path[0] = '/'; new_parent_path[1] = '\0';
        new_name = (new_last_slash == 0) ? new_abs + 1 : new_abs;
    } else {
        for (int i = 0; i < new_last_slash && i < VFS_PATH_MAX - 1; i++)
            new_parent_path[i] = new_abs[i];
        new_parent_path[new_last_slash] = '\0';
        new_name = new_abs + new_last_slash + 1;
    }

    const char *old_parent_rel, *new_parent_rel;
    vfs_mount_t *old_mnt = vfs_find_mount(old_parent_path, &old_parent_rel);
    vfs_mount_t *new_mnt = vfs_find_mount(new_parent_path, &new_parent_rel);
    if (!old_mnt || !new_mnt) return -2;
    if (old_mnt != new_mnt) return -18;  /* EXDEV: cross-device rename */

    vfs_mount_t *mnt = old_mnt;
    if (!mnt->ops->rename) return -38;  /* ENOSYS */

    vfs_inode_info_t old_parent_info = {0}, new_parent_info = {0};
    vfs_activate_root(mnt);
    if (mnt->ops->lookup(old_parent_rel, &old_parent_info) != 0) return -2;
    if (mnt->ops->lookup(new_parent_rel, &new_parent_info) != 0) return -2;

    return mnt->ops->rename(old_parent_info.inode, old_name,
                            new_parent_info.inode, new_name);
}
