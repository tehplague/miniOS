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

/* stub_vfs.c — fake VFS for host-native unit tests.
 * Used by test_syscall.c which includes syscall.c (which calls vfs functions).
 * NOT used by test_vfs.c (which includes vfs.c directly) or test_elf.c
 * (which provides its own inline vfs stubs). */

#include <miniOS/fs/vfs.h>
#include <string.h>

/* vfs_open: returns a fake fd=3 for non-NULL path, -1 for NULL */
int vfs_open(const char *path, int flags, int mode) {
    (void)flags; (void)mode;
    if (!path) return -1;
    return 3;   /* fake fd */
}

/* vfs_read: writes "test" into buf, returns 4 */
int vfs_read(int fd, void *buf, uint32_t len) {
    (void)fd;
    const char *data = "test";
    uint32_t n = (len < 4) ? len : 4;
    for (uint32_t i = 0; i < n; i++) ((char *)buf)[i] = data[i];
    return (int)n;
}

/* vfs_readdir: no-op */
int vfs_readdir(int fd, vfs_dirent_cb_t cb, void *ud) {
    (void)fd; (void)cb; (void)ud;
    return 0;
}

/* vfs_close: always succeeds */
int vfs_close(int fd) {
    (void)fd;
    return 0;
}

/* vfs_mount: no-op */
void vfs_mount(vfs_ops_t *ops) {
    (void)ops;
}

/* vfs_write: no-op stub */
int vfs_write(int fd, const void *buf, uint32_t len) {
    (void)fd; (void)buf;
    return (int)len;
}

/* vfs_unlink: always succeeds */
int vfs_unlink(const char *path) {
    if (!path)
        return -22;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && path[4] == '\0')
        return -21; /* EISDIR */
    if (path[0] == '/' && path[1] == 'g' && path[2] == 'h' && path[3] == 'o' && path[4] == 's' &&
        path[5] == 't' && path[6] == '\0')
        return -2; /* ENOENT */
    return 0;
}

/* vfs_mkdir: always succeeds */
int vfs_mkdir(const char *path, int mode) {
    (void)path; (void)mode;
    return 0;
}

int vfs_rmdir(const char *path) {
    (void)path;
    return 0;
}

/* vfs_stat: returns VFS_FILE_TYPE_DIR for "/" and "/tmp"; -2 (ENOENT) otherwise */
int vfs_stat(const char *path, vfs_inode_info_t *out) {
    if (path && out) {
        /* "/" is always a directory */
        if (path[0] == '/' && path[1] == '\0') {
            out->inode = 1;
            out->file_type = VFS_FILE_TYPE_DIR;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 64;
            return 0;
        }
        /* "/tmp" is a directory (used by SYS_chdir tests) */
        if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && path[4] == '\0') {
            out->inode = 2;
            out->file_type = VFS_FILE_TYPE_DIR;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 64;
            return 0;
        }
        /* "/test" is a regular file (used to test ENOTDIR) */
        if (path[0] == '/' && path[1] == 't' && path[2] == 'e' && path[3] == 's' && path[4] == 't' && path[5] == '\0') {
            out->inode = 3;
            out->file_type = VFS_FILE_TYPE_REG;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 5;
            return 0;
        }
    }
    return -2;
}

/* vfs_fstat: returns -9 (EBADF) stub */
int vfs_fstat(int fd, vfs_inode_info_t *out) {
    if (!out || fd < 0)
        return -9;
    out->inode = (uint32_t)fd;
    out->file_type = VFS_FILE_TYPE_REG;
    out->device_type = VFS_DEVICE_NONE;
    out->size = 5;
    return 0;
}

/* vfs_register_mount: always succeeds */
int vfs_register_mount(const char *point, vfs_ops_t *ops, uint32_t root_ino) {
    (void)point; (void)ops; (void)root_ino;
    return 0;
}

/* vfs_unregister_mount: always succeeds */
int vfs_unregister_mount(const char *point) {
    (void)point;
    return 0;
}

/* vfs_lstat: same as vfs_stat for stub purposes — no symlinks in stub world */
int vfs_lstat(const char *path, vfs_inode_info_t *out) {
    if (path && out) {
        if (path[0] == '/' && path[1] == '\0') {
            out->inode = 1;
            out->file_type = VFS_FILE_TYPE_DIR;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 64;
            return 0;
        }
        if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && path[4] == '\0') {
            out->inode = 2;
            out->file_type = VFS_FILE_TYPE_DIR;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 64;
            return 0;
        }
        if (path[0] == '/' && path[1] == 't' && path[2] == 'e' && path[3] == 's' && path[4] == 't' && path[5] == '\0') {
            out->inode = 3;
            out->file_type = VFS_FILE_TYPE_REG;
            out->device_type = VFS_DEVICE_NONE;
            out->size = 5;
            return 0;
        }
    }
    return -2;
}

/* vfs_readlink: "/test" is REG → -22 (EINVAL); unknown → -2 (ENOENT) */
int vfs_readlink(const char *path, char *buf, uint32_t bufsiz) {
    (void)buf; (void)bufsiz;
    if (!path)
        return -22;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'e' && path[3] == 's' && path[4] == 't' && path[5] == '\0')
        return -22; /* EINVAL — not a symlink */
    return -2; /* ENOENT */
}

/* vfs_symlink: no-op stub — no test invokes SYS_symlink directly */
int vfs_symlink(const char *path, const char *target) {
    (void)path; (void)target;
    return 0;
}

/* vfs_chmod: no-op stub */
int vfs_chmod(const char *path, uint16_t mode) {
    (void)path; (void)mode;
    return 0;
}

/* vfs_mknod: no-op stub */
int vfs_mknod(const char *path, uint16_t mode, uint32_t dev) {
    (void)path; (void)mode; (void)dev;
    return 0;
}

/* vfs_mount_fstype: no-op stub */
int vfs_mount_fstype(const char *fstype, const char *source,
                     const char *target, const void *data) {
    (void)fstype; (void)source; (void)target; (void)data;
    return 0;
}

/* tmpfs stubs: no-op implementations for test builds */
void tmpfs_init(void) { /* no-op */ }

uint32_t tmpfs_alloc_root(void) { return 1; /* fake inode */ }

vfs_ops_t *tmpfs_get_ops(void) { return NULL; }

int vfs_rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -38; /* ENOSYS */
}

/* vfs_path_devno / vfs_fd_devno: no mounts registered on the test host. */
int vfs_path_devno(const char *path) {
    (void)path;
    return 0;
}

int vfs_fd_devno(int fd) {
    (void)fd;
    return 0;
}

/* vfs_statfs / vfs_fstatfs: no-op stubs — zero-fill the caller's struct. */
int vfs_statfs(const char *path, vfs_statfs_t *out) {
    (void)path;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}

int vfs_fstatfs(int fd, vfs_statfs_t *out) {
    (void)fd;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
