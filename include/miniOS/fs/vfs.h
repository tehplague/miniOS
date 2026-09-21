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

#ifndef _MINIOS_FS_VFS_H_
#define _MINIOS_FS_VFS_H_

#include <miniOS/types.h> 

#include <miniOS/fs/ext2.h>
#include <miniOS/fs/pipe.h>

#define VFS_MAX_FDS     64    /* max open files per task */
#define VFS_PATH_MAX   256    /* max path length */
#define VFS_FD_STDIN    0
#define VFS_FD_STDOUT   1
#define VFS_FD_STDERR   2
#define VFS_FIRST_OPEN_FD 3

/* File type flags stored in vfs_file */
#define VFS_FILE_TYPE_REG     1
#define VFS_FILE_TYPE_DIR     2
#define VFS_FILE_TYPE_PIPE    3  /* anonymous pipe backed by ring buffer */
#define VFS_FILE_TYPE_SYMLINK 4  /* symbolic link; used by lstat to detect symlink targets */
#define VFS_FILE_TYPE_CHAR    5  /* character device node */
#define VFS_FILE_TYPE_BLK     6  /* block device node */
#define VFS_FILE_TYPE_SOCKET  7  /* BSD-style socket backed by lwIP PCB */
#define VFS_FILE_TYPE_UNIX_SOCKET 8   /* AF_UNIX socket backed by unix_sock_t */

#define VFS_DEVICE_NONE 0
#define VFS_DEVICE_TTY  1
#define VFS_DEVICE_NULL 2

typedef struct vfs_inode_info {
    uint32_t inode;
    uint8_t  file_type;
    uint8_t  device_type;
    uint32_t dev;         /* encoded (major<<8)|minor for CHR/BLK nodes; 0 otherwise */
    uint64_t size;
    uint16_t mode;        /* lower 9 bits: rwxrwxrwx permission bits; 0 = unknown */
} vfs_inode_info_t;

/* Filesystem statistics — filled by the statfs vfs_ops callback */
typedef struct vfs_statfs {
    uint64_t f_type;    /* filesystem magic (e.g. 0xEF53 for ext2) */
    uint64_t f_bsize;   /* block size in bytes */
    uint64_t f_blocks;  /* total data blocks */
    uint64_t f_bfree;   /* free blocks */
    uint64_t f_bavail;  /* free blocks available to unprivileged user */
    uint64_t f_files;   /* total inodes */
    uint64_t f_ffree;   /* free inodes */
    uint64_t f_namelen; /* max filename length */
    uint64_t f_fsid;    /* unique per-mount id (stamped by vfs_statfs) */
} vfs_statfs_t;

/* A VFS ops table — one per mounted filesystem */
typedef struct vfs_ops {
    /* Look up path metadata, return 0 on success or -1 on not found */
    int (*lookup)(const char *path, vfs_inode_info_t *out);
    /* Read bytes from inode; returns bytes read or -1 */
    int (*read)(uint32_t ino, uint64_t off, void *buf, uint32_t len);
    /* Iterate directory entries at inode; calls cb per entry */
    int (*readdir)(uint32_t ino, uint64_t *offset, vfs_dirent_cb_t cb, void *ud);
    /* Write bytes to inode; returns bytes written or -1 */
    int (*write)(uint32_t ino, uint64_t off, const void *buf, uint32_t len);
    /* Flush inode metadata to disk; returns 0 on success or -1 */
    int (*flush)(uint32_t ino);
    /* Filesystem-specific file/directory creation and deletion.
     * NULL if the filesystem is read-only or does not support this operation. */
    int (*create)(uint32_t parent_ino, const char *name, uint16_t mode, uint32_t *new_ino_out);
    int (*unlink)(uint32_t parent_ino, const char *name);  /* file only; use rmdir for dirs */
    int (*rename)(uint32_t old_parent_ino, const char *old_name,
                  uint32_t new_parent_ino, const char *new_name);
    int (*mkdir)(uint32_t parent_ino, const char *name, uint32_t *new_ino_out);  /* dir creation; NULL if unsupported */
    int (*rmdir)(uint32_t parent_ino, const char *name);   /* empty dir removal; NULL if unsupported */
    /* Set the active root inode before path resolution; NULL if unused (e.g. ext2) */
    void (*set_root)(uint32_t root_ino);
    /* Truncate a regular file to zero bytes; returns 0 on success, -1 on error.
     * NULL if the filesystem does not support truncation. */
    int (*truncate)(uint32_t ino, uint64_t new_size);
    /* Read the target of a symlink inode into buf (max len bytes, NOT null-terminated
     * per POSIX readlink(2)). Returns bytes written (>= 0) or -1 on error.
     * NULL if the filesystem does not support symlinks. */
    int (*readlink)(uint32_t ino, char *buf, uint32_t len);
    /* Create a symlink named `name` in directory `parent_ino` pointing to `target`.
     * Returns 0 on success, -1 on error.
     * NULL if the filesystem does not support symlink creation. */
    int (*symlink)(uint32_t parent_ino, const char *name, const char *target);
    /* Create a device special file (char or block) named `name` in `parent_ino`.
     * @mode: EXT2_S_IFCHR|perms or EXT2_S_IFBLK|perms.
     * @dev:  Encoded device number (major<<8)|minor.
     * Returns 0 on success, -1 on error.
     * NULL if the filesystem does not support device node creation. */
    int (*mknod)(uint32_t parent_ino, const char *name, uint16_t mode,
                 uint32_t dev, uint32_t *new_ino_out);
    /* Change permission bits of an inode; @mode contains only the low 12 bits
     * (permissions + setuid/setgid/sticky). Returns 0 on success, -1 on error.
     * NULL if the filesystem does not support chmod. */
    int (*chmod)(uint32_t ino, uint16_t mode);
    /* Fill filesystem statistics; returns 0 on success, -1 on error.
     * NULL if the filesystem does not expose usage statistics. */
    int (*statfs)(vfs_statfs_t *out);
    /* Release any single-instance global state held by this filesystem so it
     * can be mounted again. Called by vfs_unregister_mount(). NULL if the
     * filesystem has no such state to release (e.g. tmpfs, sysfs). */
    void (*unmount)(void);
} vfs_ops_t;

/* Filesystem type registry — maps fstype names to mount callbacks */
#define VFS_MAX_FS_TYPES 16

typedef struct filesystem_type {
    char name[16];  /* e.g. "ext2", "sysfs", "tmpfs" */
    /* mount() — called by vfs_mount_fstype() to attach the filesystem.
     * @source: device or source identifier (may be NULL for pseudo-filesystems)
     * @target: absolute mount point path (e.g. "/", "/sys")
     * @data:   filesystem-specific options struct (may be NULL); cast to the
     *          appropriate type inside the callback (e.g. ext2_mount_data_t *)
     * The callback is responsible for calling vfs_register_mount() internally.
     * Returns 0 on success, negative errno on failure. */
    int (*mount)(const char *source, const char *target, const void *data);
} filesystem_type_t;

int register_filesystem(const char *name,
                        int (*mount)(const char *source, const char *target,
                                     const void *data));
/**
 * vfs_mount_fstype() - Look up a registered filesystem type and mount it.
 * @fstype: Filesystem type name (e.g. "ext2", "sysfs").
 * @source: Device/source string forwarded to the mount callback (may be NULL).
 * @target: Absolute mount point path (e.g. "/", "/sys").
 * @data:   Filesystem-specific options pointer forwarded to the mount callback
 *          (may be NULL for pseudo-filesystems that need no extra parameters).
 * @return: 0 on success, -22 (EINVAL) if @fstype is not registered, or the
 *          negative errno returned by the filesystem's mount callback.
 */
int vfs_mount_fstype(const char *fstype, const char *source,
                     const char *target, const void *data);

/* Mount table entry — maps a mount point path to a filesystem ops table */
#define VFS_MAX_MOUNTS 16

typedef struct vfs_mount {
    char       mount_point[VFS_PATH_MAX]; /* e.g. "/" or "/tmp" */
    vfs_ops_t *ops;                        /* filesystem operations */
    uint32_t   root_ino;                   /* per-mount root inode (0 = fs default) */
    char       fstype[16];                 /* e.g. "ext2", "tmpfs", "proc" */
    char       device[32];                 /* e.g. "/dev/sda1" or fstype name for pseudo-fs */
} vfs_mount_t;

/**
 * vfs_register_mount() - Register a filesystem at a mount point.
 * @point: Absolute mount point path (e.g. "/", "/tmp"). Copied into mount table.
 * @ops:   Pointer to vfs_ops_t with at least lookup, read, readdir set.
 *
 * Stores entry in g_mounts[]. Up to VFS_MAX_MOUNTS=8 mounts supported.
 * @root_ino: Starting inode for path resolution (pass 0 for fs-default).
 * @return: 0 on success, -1 if mount table is full.
 */
int vfs_register_mount(const char *point, vfs_ops_t *ops, uint32_t root_ino);

/**
 * vfs_unregister_mount() - Remove a mount table entry by exact mount point match.
 * Calls ops->unmount() (if set) before removing, so single-instance filesystems
 * (e.g. ext2) can release their global state and allow being mounted again.
 * @point: Exact mount point path as passed to vfs_register_mount() (e.g. "/tmp").
 * @return: 0 on success, -1 if no mount is registered at @point.
 */
int vfs_unregister_mount(const char *point);

int vfs_mount_count(void);
const vfs_mount_t *vfs_get_mount(int idx);
int vfs_path_devno(const char *path);  /* 1-based mount index, 0 if not found */
int vfs_fd_devno(int fd);              /* 1-based mount index for open fd */

/* Forward declarations for socket types — avoids circular includes */
struct net_sock;
struct unix_sock;

/* An open file handle */
typedef struct vfs_file {
    uint8_t     in_use;
    uint32_t    inode;
    uint64_t    offset;     /* current read/write position */
    uint64_t    size;
    uint8_t     ftype;      /* VFS_FILE_TYPE_REG, VFS_FILE_TYPE_DIR, VFS_FILE_TYPE_PIPE, VFS_FILE_TYPE_CHAR */
    uint8_t     device_type; /* VFS_DEVICE_* for character devices */
    uint32_t    dev;         /* encoded (major<<8)|minor for CHR/BLK nodes; 0 otherwise */
    int         flags;         /* O_RDONLY (0) or O_WRONLY (1); set by vfs_open */
    int         status_flags;  /* O_NONBLOCK (0x800), O_APPEND (0x400), O_ASYNC (0x2000); mutable via fcntl F_SETFL */
    vfs_ops_t  *ops;
    uint32_t    ref_count;  /* number of fd slots pointing at this file description */
    pipe_t     *pipe;       /* non-NULL iff ftype == VFS_FILE_TYPE_PIPE */
    /* sock and usock share a slot: an fd is either a lwIP socket or AF_UNIX socket,
     * never both. Using a union keeps sizeof(vfs_file_t) exactly the same as
     * the original two-pointer layout. */
    union {
        struct net_sock  *sock;  /* non-NULL iff ftype == VFS_FILE_TYPE_SOCKET */
        struct unix_sock *usock; /* non-NULL iff ftype == VFS_FILE_TYPE_UNIX_SOCKET */
    };
} vfs_file_t;

/**
 * vfs_mount() - Register a filesystem's operations table (backwards-compat shim).
 * @ops: Pointer to a vfs_ops_t struct with lookup, read, and readdir function
 *       pointers. Must point to static storage valid for the kernel lifetime.
 *
 * Calls vfs_register_mount("/", ops) to mount the filesystem at root.
 * Kept for backwards compatibility — prefer vfs_register_mount() for new code.
 */
void vfs_mount(vfs_ops_t *ops);

/**
 * vfs_open() - Open or create a file by absolute path.
 * @path: Absolute path to the file or directory (e.g., "/etc/passwd").
 * @flags: O_RDONLY (0), O_WRONLY (1), O_CREAT (0x0200). O_CREAT creates file if missing.
 * @mode: File permission bits (accepted but ignored; all files created with 0644).
 *
 * Calls g_mounted_ops->lookup() to resolve the path to typed inode metadata.
 * If O_CREAT is set and file not found, creates a new file via ext2_create().
 * Allocates a file descriptor in the calling task's fd_table (per-thread;
 * up to VFS_MAX_FDS=16 concurrent open files). Initialises the vfs_file_t
 * entry with offset=0 and the resolved inode metadata.
 *
 * @return: File descriptor (VFS_FIRST_OPEN_FD to VFS_MAX_FDS-1) on success,
 *          -1 if the path is not found, creation fails, or all fds are in use.
 */
int vfs_open(const char *path, int flags, int mode);

/**
 * vfs_read() - Read bytes from an open file descriptor.
 * @fd: File descriptor returned by vfs_open.
 * @buf: Destination buffer.
 * @len: Maximum number of bytes to read.
 *
 * Dispatches to g_mounted_ops->read() with the fd's inode number and
 * current offset. Advances the offset by the number of bytes read.
 *
 * @return: Number of bytes read (may be less than @len at EOF),
 *          0 at EOF, or -1 on error (invalid fd or read failure).
 */
int vfs_read(int fd, void *buf, uint32_t len);

/**
 * vfs_readdir() - Iterate directory entries from an open directory fd.
 * @fd: File descriptor of an open directory (must have ftype == VFS_FILE_TYPE_DIR).
 * @cb: Callback invoked per entry (name, name_len, inode, file_type, ud).
 * @ud: Opaque user data forwarded to @cb.
 *
 * Dispatches to g_mounted_ops->readdir() with the fd's inode number and
 * current directory offset.
 *
 * @return: 0 when all entries visited, non-zero value from @cb if stopped early,
 *          or -1 on invalid fd or if fd is not a directory.
 */
int vfs_readdir(int fd, vfs_dirent_cb_t cb, void *ud);

/**
 * vfs_write() - Write bytes to an open file descriptor.
 * @fd: File descriptor returned by vfs_open with O_WRONLY.
 * @buf: Source buffer.
 * @len: Number of bytes to write.
 *
 * Dispatches to g_mounted_ops->write() with the fd's inode and current offset.
 * Advances offset by the number of bytes written.
 *
 * @return: Number of bytes written, or -1 on error (invalid fd, write failure).
 */
int vfs_write(int fd, const void *buf, uint32_t len);

/**
 * vfs_close() - Release an open file descriptor, flushing write data to disk.
 * @fd: File descriptor to close (0 to VFS_MAX_FDS-1).
 *
 * If the file was opened with write flags and ops->flush is set, calls
 * ops->flush(inode) to write back the final inode state, then calls ata_flush()
 * to push the ATA write-back cache to stable storage.
 * Clears the in_use flag in the calling task's fd_table entry.
 *
 * @return: 0 on success, -1 if @fd is out of range or not open.
 */
int vfs_close(int fd);

/**
 * vfs_unlink() - Delete a file by absolute path.
 * @path: Absolute path of the file to remove (e.g., "/tmp/foo.txt").
 *
 * Resolves the mount point for @path, looks up the parent directory inode,
 * then calls ops->unlink(parent_ino, filename). Immediate deletion.
 * @return: 0 on success, -1 if not found, ops->unlink is NULL, or delete fails.
 */
int vfs_unlink(const char *path);

/**
 * vfs_rename() - Rename or move a file or directory.
 * @oldpath: Absolute path of the source.
 * @newpath: Absolute path of the destination.
 *
 * Both paths must be on the same mounted filesystem (EXDEV otherwise).
 * If @newpath already exists as a file it is atomically replaced.
 * Directories cannot be overwritten (returns ENOTEMPTY).
 * @return: 0 on success, or a negative errno-style value on failure.
 */
int vfs_rename(const char *oldpath, const char *newpath);

/**
 * vfs_rmdir() - Delete an empty directory by absolute path.
 * @path: Absolute path of the directory to remove.
 *
 * Resolves the mount point for @path, looks up the parent directory inode,
 * then calls ops->rmdir(parent_ino, dirname).
 * @return: 0 on success or a negative errno-style value on failure.
 */
int vfs_rmdir(const char *path);

/**
 * vfs_mkdir() - Create a directory by absolute path.
 * @path: Absolute path of the new directory (e.g., "/tmp/newdir").
 * @mode: Permission bits (accepted, passed through to ops->mkdir).
 * @return: 0 on success, -1 if parent not found, ops->mkdir is NULL, or creation fails.
 */
int vfs_mkdir(const char *path, int mode);

/**
 * vfs_stat() - Look up file metadata by absolute path.
 * @path: Absolute path.
 * @out:  Output struct populated on success.
 * @return: 0 on success, -1 on error.
 */
int vfs_stat(const char *path, vfs_inode_info_t *out);
int vfs_statfs(const char *path, vfs_statfs_t *out);
int vfs_fstatfs(int fd, vfs_statfs_t *out);

/**
 * vfs_fstat() - Get metadata from an open file descriptor.
 * @fd:  Open file descriptor.
 * @out: Output struct populated on success.
 * @return: 0 on success, -1 on bad fd.
 */
int vfs_fstat(int fd, vfs_inode_info_t *out);

/**
 * vfs_lstat() - Look up file metadata by absolute path WITHOUT following symlinks.
 * @path: Absolute path.
 * @out:  Output struct populated on success.
 * @return: 0 on success, -1 on error.
 */
int vfs_lstat(const char *path, vfs_inode_info_t *out);

/**
 * vfs_symlink() - Create a symbolic link at path pointing to target.
 * @path:   Absolute path of the new symlink.
 * @target: Symlink target string (stored as-is; may be relative or absolute).
 * @return: 0 on success, -1 on error.
 */
int vfs_symlink(const char *path, const char *target);

/**
 * vfs_chmod() - Change the permission bits of a file by absolute path.
 * @path: Absolute path to the file.
 * @mode: New permission bits (low 12 bits only; type bits are preserved).
 * @return: 0 on success, -1 on error.
 */
int vfs_chmod(const char *path, uint16_t mode);

/**
 * vfs_mknod() - Create a device special file by absolute path.
 * @path: Absolute path of the new device node.
 * @mode: EXT2_S_IFCHR|perms or EXT2_S_IFBLK|perms.
 * @dev:  Encoded device number (major<<8)|minor.
 * @return: 0 on success, -1 on error, -22 on bad mode.
 */
int vfs_mknod(const char *path, uint16_t mode, uint32_t dev);

/**
 * vfs_readlink() - Read symlink target into buf without following.
 * @path:   Absolute path to a symlink inode.
 * @buf:    Output buffer (NOT null-terminated per POSIX).
 * @bufsiz: Size of buf.
 * @return: Number of bytes written, or -1 if not a symlink or error.
 */
int vfs_readlink(const char *path, char *buf, uint32_t bufsiz);

#endif /* _MINIOS_FS_VFS_H_ */
