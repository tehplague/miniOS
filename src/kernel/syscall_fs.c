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

#include <miniOS/syscall.h>
#include <miniOS/sched/sched.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/fs/pipe.h>
#include <miniOS/mm/heap.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/types.h>
#include <miniOS/drivers/tty.h>
#include <miniOS/net/net_socket.h>
#include <miniOS/net/unix_sock.h>
#include <string.h>
#include <stdint.h>
#include "syscall_internal.h"

/* TSC-based timing for poll/select deadlines (accurate regardless of LAPIC rate) */
extern uint64_t miniOS_tsc_hz;
extern uint64_t miniOS_tsc_boot;
static inline uint64_t fs_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* -----------------------------------------------------------------------
 * poll/select ABI structs and flag constants
 * ----------------------------------------------------------------------- */
struct mini_pollfd {
    int   fd;
    short events;
    short revents;
} __attribute__((packed));

typedef struct { uint64_t tv_sec;  uint64_t tv_usec; } mini_timeval_t;
typedef struct { uint64_t bits[8]; } mini_fd_set_t;

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed)) linux_dirent64_t;

typedef struct {
    uint8_t  *buf;
    uint32_t  buf_size;
    uint32_t  written;
    int       overflow;
} getdents_state_t;

static int getdents_cb(const char *name, uint8_t name_len,
                       uint32_t inode, uint8_t file_type, void *ud) {
    getdents_state_t *s = (getdents_state_t *)ud;
    uint16_t base_size = (uint16_t)(sizeof(linux_dirent64_t) + name_len + 1);
    uint16_t reclen    = (uint16_t)((base_size + 7u) & ~7u);
    if (s->written + reclen > s->buf_size) {
        s->overflow = 1;
        return 1;
    }

    linux_dirent64_t *d = (linux_dirent64_t *)(s->buf + s->written);
    d->d_ino    = inode;
    d->d_off    = (int64_t)(s->written + reclen);
    d->d_reclen = reclen;
    d->d_type   = file_type;
    for (uint8_t i = 0; i < name_len; i++)
        d->d_name[i] = name[i];
    d->d_name[name_len] = '\0';

    uint16_t name_field_len = (uint16_t)(reclen - (uint16_t)sizeof(linux_dirent64_t));
    for (uint16_t i = (uint16_t)(name_len + 1); i < name_field_len; i++)
        d->d_name[i] = 0;

    s->written += reclen;
    return 0;
}

static int socket_has_other_fd_ref(struct thread *thread, int closing_fd, net_sock_t *sock)
{
    if (!thread || !sock)
        return 0;

    vfs_file_t *fdt = THREAD_FDT(thread);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (i == closing_fd)
            continue;
        if (!fdt[i].in_use)
            continue;
        if (fdt[i].ftype != VFS_FILE_TYPE_SOCKET)
            continue;
        if (fdt[i].sock == sock)
            return 1;
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * kernel_stat_t - Linux x86-64 struct stat layout (matches mlibc ABI).
 * ----------------------------------------------------------------------- */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atim_tv_sec;
    int64_t  st_atim_tv_nsec;
    int64_t  st_mtim_tv_sec;
    int64_t  st_mtim_tv_nsec;
    int64_t  st_ctim_tv_sec;
    int64_t  st_ctim_tv_nsec;
    int64_t  __unused[3];
} kernel_stat_t;

#define KST_IFREG  0100000U
#define KST_IFDIR  0040000U
#define KST_IFCHR  0020000U
#define KST_IFBLK  0060000U

/* Linux x86-64 struct statfs layout (matches mlibc ABI, 120 bytes) */
typedef struct {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
} kernel_statfs_t;

static void populate_stat(const vfs_inode_info_t *info, kernel_stat_t *st) {
    for (size_t i = 0; i < sizeof(*st); i++)
        ((char *)st)[i] = 0;

    st->st_ino      = (uint64_t)info->inode;
    st->st_size     = (int64_t)info->size;
    st->st_nlink    = 1;
    st->st_blksize  = 4096;
    st->st_blocks   = (int64_t)((info->size + 511) / 512);

    if (info->file_type == VFS_FILE_TYPE_SYMLINK) {
        st->st_mode = 0120777;
    } else if (info->file_type == VFS_FILE_TYPE_DIR) {
        st->st_mode = KST_IFDIR | 0755U;
    } else if (info->file_type == VFS_FILE_TYPE_CHAR) {
        st->st_mode = KST_IFCHR | 0666U;
    } else if (info->file_type == VFS_FILE_TYPE_BLK) {
        st->st_mode = KST_IFBLK | 0660U;
    } else {
        st->st_mode = KST_IFREG | 0644U;
    }

    if (info->file_type == VFS_FILE_TYPE_CHAR || info->file_type == VFS_FILE_TYPE_BLK)
        st->st_rdev = (uint64_t)info->dev;
}

static uint32_t synthetic_access_bits(const vfs_inode_info_t *info) {
    kernel_stat_t st;
    populate_stat(info, &st);

    uint32_t access = 0;
    if (st.st_mode & 0400U) access |= 4U;
    if (st.st_mode & 0200U) access |= 2U;
    if (st.st_mode & 0100U) access |= 1U;
    return access;
}

int64_t syscall_dispatch_fs(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    (void)arg6;

    switch (nr) {
    case SYS_read: {
        int fd = (int)arg1;
        void *buf = (void *)arg2;
        uint32_t count = (uint32_t)arg3;
        if (buf == NULL || count == 0)
            return -22;
        if (fd >= 0 && fd < VFS_FIRST_OPEN_FD) {
            struct thread *tc_rd = sched_current();
            vfs_file_t *vf_stdio = &THREAD_FDT(tc_rd)[fd];
            if (vf_stdio->in_use) {
                if (vf_stdio->ftype == VFS_FILE_TYPE_PIPE && vf_stdio->pipe)
                    return pipe_read_buf(vf_stdio->pipe, buf, count);
                return (int64_t)vfs_read(fd, buf, count);
            }
            if (fd == VFS_FD_STDIN)
                return tty_read(buf, count, 0);
            return -22;
        }
        if (fd < VFS_FIRST_OPEN_FD || fd >= VFS_MAX_FDS)
            return -22;
        struct thread *tp_rd = sched_current();
        vfs_file_t *vf_rd = &THREAD_FDT(tp_rd)[fd];
        if (vf_rd->in_use && vf_rd->ftype == VFS_FILE_TYPE_UNIX_SOCKET && vf_rd->usock)
            return unix_sock_read(vf_rd->usock, buf, count);
        if (vf_rd->in_use && vf_rd->ftype == VFS_FILE_TYPE_SOCKET && vf_rd->sock) {
            {
                extern volatile uint64_t lapic_tick_count;
                /* D-08: SYS_read on socket uses the 10-second default deadline, not rcvtimeo_ticks */
                uint64_t read_deadline = lapic_tick_count + 1000; /* 10 seconds */
                while (!net_sock_has_data(vf_rd->sock) && lapic_tick_count < read_deadline) {
                    if (syscall_has_pending_signal()) {
                        /* SYS_read on socket gets SA_RESTART */
                        tp_rd->syscall_restart_pending = 1;
                        return -4;  /* EINTR */
                    }
                    /* Sleep until net_poll_thread wakes us via recv callback */
                    vf_rd->sock->poll_waiter_tid = tp_rd->tid;
                    tp_rd->state = THREAD_FUTEX_WAIT;
                    sched_yield();
                    vf_rd->sock->poll_waiter_tid = 0;
                }
            }
            return net_sock_recv(vf_rd->sock, buf, count, 0);
        }
        if (vf_rd->in_use && vf_rd->ftype == VFS_FILE_TYPE_PIPE && vf_rd->pipe)
            return pipe_read_buf(vf_rd->pipe, buf, count);
        return (int64_t)vfs_read(fd, buf, count);
    }

    case SYS_write: {
        int fd = (int)arg1;
        const char *buf = (const char *)arg2;
        uint64_t count = arg3;
        if (buf == NULL || count == 0)
            return -22;
        if (fd >= 0 && fd < VFS_FIRST_OPEN_FD) {
            struct thread *twt = sched_current();
            vfs_file_t *vf_stdio = &THREAD_FDT(twt)[fd];
            if (vf_stdio->in_use) {
                if (vf_stdio->ftype == VFS_FILE_TYPE_PIPE && vf_stdio->pipe)
                    return pipe_write_buf(vf_stdio->pipe, buf, (uint32_t)count);
                return (int64_t)vfs_write(fd, buf, (uint32_t)count);
            }
            if (fd == VFS_FD_STDOUT || fd == VFS_FD_STDERR)
                return (int64_t)tty_write(buf, (uint32_t)count);
            return -9;
        }

        if (fd >= VFS_FIRST_OPEN_FD && fd < VFS_MAX_FDS) {
            struct thread *tp_wr = sched_current();
            vfs_file_t *vf_wr = &THREAD_FDT(tp_wr)[fd];
            if (vf_wr->in_use && vf_wr->ftype == VFS_FILE_TYPE_UNIX_SOCKET && vf_wr->usock)
                return unix_sock_write(vf_wr->usock, buf, (size_t)count);
            if (vf_wr->in_use && vf_wr->ftype == VFS_FILE_TYPE_SOCKET && vf_wr->sock)
                return net_sock_send(vf_wr->sock, buf, (size_t)count, 0);
            if (vf_wr->in_use && vf_wr->ftype == VFS_FILE_TYPE_PIPE && vf_wr->pipe)
                return pipe_write_buf(vf_wr->pipe, buf, (uint32_t)count);
            return (int64_t)vfs_write(fd, buf, (uint32_t)count);
        }

        return -9;
    }

    case SYS_open: {
        const char *path = (const char *)arg1;
        int oflags = (int)arg2;
        int mode   = (int)arg3;
        if (path == NULL)
            return -22;
        return (int64_t)vfs_open(path, oflags, mode);
    }

    case SYS_close: {
        int fd = (int)arg1;
        struct thread *tc = sched_current();
        vfs_file_t *tc_fdt = THREAD_FDT(tc);
        /* fd 0-2: TTY char device entries — just clear in_use to release the slot */
        if (fd >= 0 && fd < VFS_FIRST_OPEN_FD && tc_fdt[fd].in_use) {
            tc_fdt[fd].in_use = 0;
            return 0;
        }
        if (fd >= VFS_FIRST_OPEN_FD && fd < VFS_MAX_FDS && tc_fdt[fd].in_use) {
            vfs_file_t *cvf = &tc_fdt[fd];
            if (cvf->ftype == VFS_FILE_TYPE_PIPE && cvf->pipe) {
                if (cvf->flags == 1)
                    cvf->pipe->write_closed = 1;
                cvf->ref_count--;
                if (cvf->ref_count == 0)
                    pipe_free(cvf->pipe);
                cvf->pipe    = NULL;
                cvf->in_use  = 0;
                cvf->ftype   = 0;
                cvf->flags   = 0;
                cvf->ops     = NULL;
                return 0;
            }
            if (cvf->ftype == VFS_FILE_TYPE_SOCKET && cvf->sock) {
                net_sock_t *csock = cvf->sock;
                int has_other_ref = socket_has_other_fd_ref(tc, fd, csock);
                /* WR-03: delegate all PCB teardown to net_sock_close/net_sock_free;
                 * net_sock_free owns the null-check and proto dispatch internally. */
                if (!has_other_ref)
                    net_sock_close(csock);
                cvf->sock   = NULL;
                cvf->in_use = 0;
                cvf->ftype  = 0;
                return 0;
            }
            if (cvf->ftype == VFS_FILE_TYPE_UNIX_SOCKET && cvf->usock) {
                unix_sock_close(cvf->usock);
                cvf->usock  = NULL;
                cvf->in_use = 0;
                cvf->ftype  = 0;
                return 0;
            }
        }
        return (int64_t)vfs_close(fd);
    }

    case SYS_ioctl: {
        int fd = (int)arg1;
        uint64_t cmd = arg2;
        uint64_t ptr = arg3;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        if (fd < VFS_FIRST_OPEN_FD) {
            struct thread *tio = sched_current();
            vfs_file_t *vf_stdio = &THREAD_FDT(tio)[fd];
            if (vf_stdio->in_use) {
                if (vf_stdio->ftype == VFS_FILE_TYPE_CHAR &&
                    vf_stdio->device_type == VFS_DEVICE_TTY)
                    return tty_ioctl(cmd, ptr);
                return -25;
            }
            return tty_ioctl(cmd, ptr);
        }
        struct thread *tio = sched_current();
        vfs_file_t *vf = &THREAD_FDT(tio)[fd];
        if (!vf->in_use)
            return -9;
        if (vf->ftype == VFS_FILE_TYPE_CHAR && vf->device_type == VFS_DEVICE_TTY)
            return tty_ioctl(cmd, ptr);
        return -25;
    }

    case SYS_lseek: {
        int fd = (int)arg1;
        int64_t offset = (int64_t)arg2;
        int whence = (int)arg3;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        struct thread *t = sched_current();
        vfs_file_t *f = &THREAD_FDT(t)[fd];
        if (!f->in_use)
            return -9;
        switch (whence) {
        case 0: f->offset = (uint64_t)offset; break;
        case 1: f->offset = (uint64_t)((int64_t)f->offset + offset); break;
        case 2: f->offset = (uint64_t)((int64_t)f->size  + offset); break;
        default: return -22;
        }
        return (int64_t)f->offset;
    }

    case SYS_getdents: {
        int fd = (int)arg1;
        void *buf = (void *)arg2;
        uint32_t buf_size = (uint32_t)arg3;
        if (fd < VFS_FIRST_OPEN_FD || fd >= VFS_MAX_FDS || buf == NULL || buf_size == 0)
            return -22;
        getdents_state_t state = {
            .buf      = (uint8_t *)buf,
            .buf_size = buf_size,
            .written  = 0,
            .overflow = 0,
        };
        int r = vfs_readdir(fd, getdents_cb, &state);
        if (r < 0)
            return -9;
        if (state.overflow && state.written == 0)
            return -34;
        return (int64_t)state.written;
    }

    case SYS_getcwd: {
        char *buf = (char *)(uintptr_t)arg1;
        size_t sz = (size_t)arg2;
        if (!buf || sz == 0)
            return -22;

        struct thread *t_gc = sched_current();
        size_t cwd_len = strlen(t_gc->cwd);
        if (cwd_len + 1 > sz)
            return -34;

        memcpy(buf, t_gc->cwd, cwd_len + 1);
        return (int64_t)(uintptr_t)buf;
    }

    case SYS_chdir: {
        const char *path_cd = (const char *)(uintptr_t)arg1;
        if (!path_cd)
            return -22;

        size_t plen = strlen(path_cd);
        if (plen >= VFS_PATH_MAX)
            return -36;

        char abs_cd[VFS_PATH_MAX];
        {
            struct thread *t_abs = sched_current();
            if (path_cd[0] == '/') {
                strncpy(abs_cd, path_cd, VFS_PATH_MAX - 1);
                abs_cd[VFS_PATH_MAX - 1] = '\0';
            } else {
                size_t cwd_len = strlen(t_abs->cwd);
                int sep = (cwd_len > 0 && t_abs->cwd[cwd_len - 1] != '/') ? 1 : 0;
                int r = snprintf(abs_cd, VFS_PATH_MAX, "%s%s%s",
                                 t_abs->cwd, sep ? "/" : "", path_cd);
                if (r < 0 || r >= (int)VFS_PATH_MAX)
                    return -36;
            }
        }

        vfs_inode_info_t info_cd;
        if (vfs_stat(abs_cd, &info_cd) < 0)
            return -2;
        if (info_cd.file_type != VFS_FILE_TYPE_DIR)
            return -20;

        struct thread *t_cd = sched_current();
        strncpy(t_cd->cwd, abs_cd, VFS_PATH_MAX - 1);
        t_cd->cwd[VFS_PATH_MAX - 1] = '\0';
        return 0;
    }

    case SYS_pipe: {
        uint64_t fds_ptr = arg1;
        if (!fds_ptr || fds_ptr >= KERNEL_VMA)
            return -14;

        pipe_t *p = pipe_create();
        if (!p)
            return -12;

        struct thread *tp = sched_current();
        vfs_file_t *tp_fdt = THREAD_FDT(tp);
        int rfd = -1, wfd = -1;
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
            if (!tp_fdt[i].in_use) {
                rfd = i;
                break;
            }
        }
        if (rfd < 0) {
            kfree(p);
            return -24;
        }
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
            if (i != rfd && !tp_fdt[i].in_use) {
                wfd = i;
                break;
            }
        }
        if (wfd < 0) {
            kfree(p);
            return -24;
        }

        p->ref_count = 2;

        vfs_file_t *rvf = &tp_fdt[rfd];
        memset(rvf, 0, sizeof(*rvf));
        rvf->in_use    = 1;
        rvf->ftype     = VFS_FILE_TYPE_PIPE;
        rvf->flags     = 0;
        rvf->pipe      = p;
        rvf->ref_count = 1;
        rvf->ops       = &pipe_ops;

        vfs_file_t *wvf = &tp_fdt[wfd];
        memset(wvf, 0, sizeof(*wvf));
        wvf->in_use    = 1;
        wvf->ftype     = VFS_FILE_TYPE_PIPE;
        wvf->flags     = 1;
        wvf->pipe      = p;
        wvf->ref_count = 1;
        wvf->ops       = &pipe_ops;

        int out[2] = { rfd, wfd };
        memcpy((void *)fds_ptr, out, sizeof(out));
        return 0;
    }

    case SYS_dup: {
        int oldfd = (int)arg1;
        struct thread *td = sched_current();
        vfs_file_t *td_fdt = THREAD_FDT(td);
        if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !td_fdt[oldfd].in_use)
            return -9;

        int newfd = -1;
        for (int i = VFS_FIRST_OPEN_FD; i < VFS_MAX_FDS; i++) {
            if (!td_fdt[i].in_use) {
                newfd = i;
                break;
            }
        }
        if (newfd < 0)
            return -24;

        vfs_file_t *old_vf = &td_fdt[oldfd];
        vfs_file_t *new_vf = &td_fdt[newfd];
        *new_vf = *old_vf;
        new_vf->ref_count++;
        return newfd;
    }

    case SYS_dup2: {
        int oldfd = (int)arg1;
        int newfd = (int)arg2;
        if (oldfd < 0 || oldfd >= VFS_MAX_FDS)
            return -9;
        if (newfd < 0 || newfd >= VFS_MAX_FDS)
            return -9;
        if (oldfd == newfd)
            return newfd;

        struct thread *tdup = sched_current();
        vfs_file_t *tdup_fdt = THREAD_FDT(tdup);
        vfs_file_t *oldf = &tdup_fdt[oldfd];
        if (!oldf->in_use)
            return -9;

        vfs_file_t *newf_existing = &tdup_fdt[newfd];
        if (newf_existing->in_use) {
            if (newf_existing->ftype == VFS_FILE_TYPE_PIPE && newf_existing->pipe) {
                if (newf_existing->flags == 1)
                    newf_existing->pipe->write_closed = 1;
                newf_existing->ref_count--;
                if (newf_existing->ref_count == 0) {
                    pipe_free(newf_existing->pipe);
                    newf_existing->pipe = NULL;
                }
                newf_existing->in_use = 0;
            } else {
                vfs_close(newfd);
            }
        }

        tdup_fdt[newfd] = *oldf;
        tdup_fdt[newfd].ref_count++;
        return newfd;
    }

    case SYS_chmod: {
        const char *path = (const char *)arg1;
        uint16_t mode = (uint16_t)arg2;
        if (!path)
            return -22;
        if ((uint64_t)path >= KERNEL_VMA)
            return -14;
        return (int64_t)vfs_chmod(path, mode);
    }

    case SYS_mknod: {
        const char *path = (const char *)arg1;
        uint16_t mode = (uint16_t)arg2;
        uint32_t dev = (uint32_t)arg3;
        if (!path)
            return -22;
        if ((uint64_t)path >= KERNEL_VMA)
            return -14;
        int r = vfs_mknod(path, mode, dev);
        if (r == -22)
            return -22;
        if (r < 0)
            return -17;
        return 0;
    }

    case SYS_mkdir: {
        const char *path = (const char *)arg1;
        int mode = (int)arg2;
        if (!path)
            return -22;
        if ((uint64_t)path >= KERNEL_VMA)
            return -14;
        if (vfs_mkdir(path, mode) < 0)
            return -17;
        return 0;
    }

    case SYS_rmdir: {
        const char *path = (const char *)arg1;
        if (!path)
            return -22;
        if ((uint64_t)path >= KERNEL_VMA)
            return -14;
        return (int64_t)vfs_rmdir(path);
    }

    case SYS_unlink: {
        const char *path = (const char *)arg1;
        return (int64_t)vfs_unlink(path);
    }

    case SYS_rename: {
        const char *oldpath = (const char *)arg1;
        const char *newpath = (const char *)arg2;
        if (!oldpath || !newpath) return -22;
        if ((uint64_t)oldpath >= KERNEL_VMA || (uint64_t)newpath >= KERNEL_VMA)
            return -14;
        return (int64_t)vfs_rename(oldpath, newpath);
    }

    case SYS_symlink: {
        const char *sym_target   = (const char *)(uintptr_t)arg1;
        const char *sym_linkpath = (const char *)(uintptr_t)arg2;
        if (!sym_target || !sym_linkpath)
            return -22;
        if ((uint64_t)sym_target >= KERNEL_VMA || (uint64_t)sym_linkpath >= KERNEL_VMA)
            return -14;
        return (int64_t)vfs_symlink(sym_linkpath, sym_target);
    }

    case SYS_lstat: {
        const char *pathname_l = (const char *)arg1;
        kernel_stat_t *statbuf_l = (kernel_stat_t *)arg2;
        if (!pathname_l || !statbuf_l)
            return -22;
        if ((uint64_t)pathname_l >= KERNEL_VMA || (uint64_t)statbuf_l >= KERNEL_VMA)
            return -14;
        vfs_inode_info_t info_l;
        if (vfs_lstat(pathname_l, &info_l) < 0)
            return -2;
        kernel_stat_t st_l;
        populate_stat(&info_l, &st_l);
        st_l.st_dev = (uint64_t)vfs_path_devno(pathname_l);
        memcpy(statbuf_l, &st_l, sizeof(kernel_stat_t));
        return 0;
    }

    case SYS_stat: {
        const char *pathname = (const char *)arg1;
        kernel_stat_t *statbuf = (kernel_stat_t *)arg2;
        if (!pathname || !statbuf)
            return -22;
        if ((uint64_t)pathname >= KERNEL_VMA || (uint64_t)statbuf >= KERNEL_VMA)
            return -14;
        vfs_inode_info_t info;
        if (vfs_stat(pathname, &info) < 0)
            return -2;
        kernel_stat_t st;
        populate_stat(&info, &st);
        st.st_dev = (uint64_t)vfs_path_devno(pathname);
        memcpy(statbuf, &st, sizeof(kernel_stat_t));
        return 0;
    }

    case SYS_access: {
        const char *pathname = (const char *)arg1;
        int mode = (int)arg2;
        if (!pathname)
            return -22;
        if ((uint64_t)pathname >= KERNEL_VMA)
            return -14;
        if ((mode & ~7) != 0)
            return -22;
        vfs_inode_info_t info;
        if (vfs_stat(pathname, &info) < 0)
            return -2;
        if (mode == 0)
            return 0;
        uint32_t allowed = synthetic_access_bits(&info);
        if (((uint32_t)mode & allowed) != (uint32_t)mode)
            return -13;
        return 0;
    }

    case SYS_fstat: {
        int fd = (int)arg1;
        kernel_stat_t *statbuf = (kernel_stat_t *)arg2;
        if (!statbuf)
            return -22;
        if ((uint64_t)statbuf >= KERNEL_VMA)
            return -14;
        vfs_inode_info_t info;
        if (vfs_fstat(fd, &info) < 0)
            return -9;
        kernel_stat_t st;
        populate_stat(&info, &st);
        st.st_dev = (uint64_t)vfs_fd_devno(fd);
        memcpy(statbuf, &st, sizeof(kernel_stat_t));
        return 0;
    }

    case SYS_statfs: {
        const char *path = (const char *)arg1;
        kernel_statfs_t *buf = (kernel_statfs_t *)arg2;
        if (!path || !buf) return -22;
        if ((uint64_t)path >= KERNEL_VMA || (uint64_t)buf >= KERNEL_VMA) return -14;
        vfs_statfs_t vfs_sf;
        int r = vfs_statfs(path, &vfs_sf);
        if (r < 0) return r;
        kernel_statfs_t ksf = {0};
        ksf.f_type    = vfs_sf.f_type;
        ksf.f_bsize   = vfs_sf.f_bsize;
        ksf.f_blocks  = vfs_sf.f_blocks;
        ksf.f_bfree   = vfs_sf.f_bfree;
        ksf.f_bavail  = vfs_sf.f_bavail;
        ksf.f_files   = vfs_sf.f_files;
        ksf.f_ffree   = vfs_sf.f_ffree;
        ksf.f_namelen = vfs_sf.f_namelen;
        ksf.f_frsize  = vfs_sf.f_bsize;
        ksf.f_fsid[0] = (uint32_t)vfs_sf.f_fsid;
        ksf.f_fsid[1] = 0;
        memcpy(buf, &ksf, sizeof(ksf));
        return 0;
    }

    case SYS_fstatfs: {
        int fd = (int)arg1;
        kernel_statfs_t *buf = (kernel_statfs_t *)arg2;
        if (!buf) return -22;
        if ((uint64_t)buf >= KERNEL_VMA) return -14;
        vfs_statfs_t vfs_sf;
        int r = vfs_fstatfs(fd, &vfs_sf);
        if (r < 0) return r;
        kernel_statfs_t ksf = {0};
        ksf.f_type    = vfs_sf.f_type;
        ksf.f_bsize   = vfs_sf.f_bsize;
        ksf.f_blocks  = vfs_sf.f_blocks;
        ksf.f_bfree   = vfs_sf.f_bfree;
        ksf.f_bavail  = vfs_sf.f_bavail;
        ksf.f_files   = vfs_sf.f_files;
        ksf.f_ffree   = vfs_sf.f_ffree;
        ksf.f_namelen = vfs_sf.f_namelen;
        ksf.f_frsize  = vfs_sf.f_bsize;
        ksf.f_fsid[0] = (uint32_t)vfs_sf.f_fsid;
        ksf.f_fsid[1] = 0;
        memcpy(buf, &ksf, sizeof(ksf));
        return 0;
    }

    case SYS_mount: {
        const char *source = (const char *)arg1;
        const char *target = (const char *)arg2;
        const char *fstype = (const char *)arg3;
        const void *data = (const void *)arg5;
        if (!target || !fstype)
            return -22;
        if ((uint64_t)target >= KERNEL_VMA || (uint64_t)fstype >= KERNEL_VMA)
            return -14;
        if (source && (uint64_t)source >= KERNEL_VMA)
            return -14;
        if (data && (uint64_t)data >= KERNEL_VMA)
            return -14;
        if (target[0] != '/')
            return -22;

        if (strncmp(fstype, "sysfs", 5) == 0 || strncmp(fstype, "tmpfs", 4) == 0)
        {
            // Pseudo filesystem
            return vfs_mount_fstype(fstype, source, target, data);
        }

        if (!source)
        {
            return -22;
        }

        vfs_inode_info_t src_info;
        if (vfs_lstat(source, &src_info) < 0 || src_info.file_type != VFS_FILE_TYPE_BLK)
            return -22;

        vfs_mount_data_t blk_data = {
            .major = (uint8_t)(src_info.dev >> 8),
            .minor = (uint8_t)(src_info.dev & 0xFF),
        };

        return vfs_mount_fstype(fstype, source, target, &blk_data);
    }

    case SYS_umount: {
        const char *target = (const char *)arg1;
        if (!target)
            return -22;
        if ((uint64_t)target >= KERNEL_VMA)
            return -14;
        if (strncmp(target, "/", 2) == 0)
            return -16;  /* EBUSY: refuse to unmount the root filesystem */
        if (vfs_unregister_mount(target) < 0)
            return -22;  /* EINVAL: not a mount point */
        return 0;
    }

    case SYS_fcntl: {
        int fd = (int)arg1;
        int cmd = (int)arg2;
        long fcntl_arg = (long)arg3;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -9;
        struct thread *fcntl_t = sched_current();
        vfs_file_t *f = &THREAD_FDT(fcntl_t)[fd];
        if (!f->in_use)
            return -9;
        switch (cmd) {
        case 0:    /* F_DUPFD */
        case 1030: { /* F_DUPFD_CLOEXEC (Linux/mlibc ABI) */
            int min_fd = (int)fcntl_arg;
            if (min_fd < 0) min_fd = 0;
            vfs_file_t *fcntl_fdt = THREAD_FDT(fcntl_t);
            for (int i = min_fd; i < VFS_MAX_FDS; i++) {
                if (!fcntl_fdt[i].in_use) {
                    fcntl_fdt[i] = *f;
                    return (int64_t)i;
                }
            }
            return -24; /* EMFILE */
        }
        case 1:  /* F_GETFD */
        case 2:  /* F_SETFD */
            return 0;
        case 3:
            return (int64_t)(f->flags | f->status_flags);
        case 4:
            f->status_flags = (int)fcntl_arg & (0x0800 | 0x0400 | 0x2000);
            return 0;
        case 5:  /* F_GETLK64 */
        case 6:  /* F_SETLK64 */
        case 7:  /* F_SETLKW64 */
        case 14: /* F_SETLKW64 (alternate) */
        case 8:  /* F_SETOWN */
        case 9:  /* F_GETOWN */
            return 0;
        default:
            return -22;
        }
    }

    case SYS_select: {
        int nfds = (int)arg1;
        mini_fd_set_t *readfds  = (mini_fd_set_t *)(uintptr_t)arg2;
        mini_fd_set_t *writefds = (mini_fd_set_t *)(uintptr_t)arg3;
        mini_timeval_t *tv = (mini_timeval_t *)(uintptr_t)arg5;
        if (nfds < 0 || nfds > 512)
            return -22;

        int timeout_ms;
        if (!tv) {
            timeout_ms = -1;
        } else {
            timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
        }

        struct mini_pollfd spfds[VFS_MAX_FDS];
        int fd_map[VFS_MAX_FDS];
        int npfds = 0;

        for (int fd = 0; fd < nfds && fd < VFS_MAX_FDS * 64 && npfds < VFS_MAX_FDS; fd++) {
            int bit_idx   = fd / 64;
            int bit_shift = fd % 64;
            short events  = 0;
            if (readfds  && (readfds->bits[bit_idx]  & (1ULL << bit_shift))) events |= POLLIN;
            if (writefds && (writefds->bits[bit_idx] & (1ULL << bit_shift))) events |= POLLOUT;
            if (events) {
                spfds[npfds].fd      = fd;
                spfds[npfds].events  = events;
                spfds[npfds].revents = 0;
                fd_map[npfds]        = fd;
                npfds++;
            }
        }

        if (npfds == 0)
            return 0;

        {
            int sel_infinite = (timeout_ms < 0);
            uint64_t sel_deadline_tsc = 0;
            if (!sel_infinite) {
                uint64_t timeout_tsc = (uint64_t)timeout_ms * miniOS_tsc_hz / 1000;
                sel_deadline_tsc = fs_rdtsc() + timeout_tsc;
            }

            for (;;) {
                int ready = 0;
                struct thread *sel_cur = sched_current();
                for (int i = 0; i < npfds; i++) {
                    int fd = spfds[i].fd;
                    spfds[i].revents = 0;
                    if (fd < 0 || fd >= VFS_MAX_FDS) {
                        spfds[i].revents = POLLNVAL;
                        ready++;
                        continue;
                    }
                    vfs_file_t *f = &THREAD_FDT(sel_cur)[fd];

                    if (!f->in_use) {
                        if (fd < VFS_FIRST_OPEN_FD) {
                            if ((spfds[i].events & POLLIN) && tty_has_data()) {
                                spfds[i].revents |= POLLIN;
                                ready++;
                            }
                            if (spfds[i].events & POLLOUT) {
                                spfds[i].revents |= POLLOUT;
                                ready++;
                            }
                        } else {
                            spfds[i].revents = POLLNVAL;
                            ready++;
                        }
                        continue;
                    }

                    if (f->ftype == VFS_FILE_TYPE_PIPE && f->pipe) {
                        pipe_t *p = f->pipe;
                        uint32_t avail = p->write_pos - p->read_pos;
                        if ((spfds[i].events & POLLIN) && avail > 0) {
                            spfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if ((spfds[i].events & POLLOUT) && avail < PIPE_SIZE) {
                            spfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else if (f->ftype == VFS_FILE_TYPE_SOCKET && f->sock) {
                        if ((spfds[i].events & POLLIN) && net_sock_has_data(f->sock)) {
                            spfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if (spfds[i].events & POLLOUT) {
                            spfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else if (f->ftype == VFS_FILE_TYPE_CHAR &&
                               f->device_type == VFS_DEVICE_TTY) {
                        if ((spfds[i].events & POLLIN) && tty_has_data()) {
                            spfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if (spfds[i].events & POLLOUT) {
                            spfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else {
                        spfds[i].revents = POLLERR;
                        ready++;
                    }
                }

                /* INTR-03: ready I/O wins over signal check */
                if (ready > 0) {
                    if (readfds)  { for (int b = 0; b < 8; b++) readfds->bits[b]  = 0; }
                    if (writefds) { for (int b = 0; b < 8; b++) writefds->bits[b] = 0; }
                    for (int i = 0; i < npfds; i++) {
                        int fd = fd_map[i];
                        if ((spfds[i].revents & POLLIN)  && readfds)
                            readfds->bits[fd / 64]  |= (1ULL << (fd % 64));
                        if ((spfds[i].revents & POLLOUT) && writefds)
                            writefds->bits[fd / 64] |= (1ULL << (fd % 64));
                    }
                    return ready;
                }
                if (syscall_has_pending_signal()) {
                    /* D-03: infinite select gets SA_RESTART; finite does not */
                    if (sel_infinite)
                        sel_cur->syscall_restart_pending = 1;
                    return -4;  /* EINTR */
                }
                if (!sel_infinite && fs_rdtsc() >= sel_deadline_tsc) {
                    if (readfds)  { for (int b = 0; b < 8; b++) readfds->bits[b]  = 0; }
                    if (writefds) { for (int b = 0; b < 8; b++) writefds->bits[b] = 0; }
                    return 0;
                }
                /* Sleep if all blocking fds are sockets; net_poll_thread wakes via
                 * sched_wake_tid() from the lwIP recv callback. Fall back to
                 * sched_yield when TTY/pipe fds are present. */
                int sel_nonsocket = 0;
                for (int i = 0; i < npfds; i++) {
                    int fd = spfds[i].fd;
                    if (fd < VFS_FIRST_OPEN_FD) { sel_nonsocket = 1; break; }
                    vfs_file_t *pf = &THREAD_FDT(sel_cur)[fd];
                    if (!pf->in_use || pf->ftype != VFS_FILE_TYPE_SOCKET)
                        { sel_nonsocket = 1; break; }
                }
                if (!sel_nonsocket) {
                    for (int i = 0; i < npfds; i++) {
                        int fd = spfds[i].fd;
                        if (fd >= 0 && fd < VFS_MAX_FDS) {
                            vfs_file_t *pf = &THREAD_FDT(sel_cur)[fd];
                            if (pf->in_use && pf->ftype == VFS_FILE_TYPE_SOCKET && pf->sock)
                                pf->sock->poll_waiter_tid = sel_cur->tid;
                        }
                    }
                    sel_cur->state = THREAD_FUTEX_WAIT;
                    sched_yield();
                    for (int i = 0; i < npfds; i++) {
                        int fd = spfds[i].fd;
                        if (fd >= 0 && fd < VFS_MAX_FDS) {
                            vfs_file_t *pf = &THREAD_FDT(sel_cur)[fd];
                            if (pf->in_use && pf->ftype == VFS_FILE_TYPE_SOCKET && pf->sock)
                                pf->sock->poll_waiter_tid = 0;
                        }
                    }
                } else {
                    sched_yield();
                }
            }
        }
        /* unreachable */
        return 0;
    }

    case SYS_poll: {
        struct mini_pollfd *user_fds = (struct mini_pollfd *)(uintptr_t)arg1;
        int nfds = (int)arg2;
        int timeout = (int)arg3;
        if (!user_fds && nfds > 0)
            return -14;
        if (nfds < 0)
            return -22;
        if (nfds == 0)
            return 0;

        int effective_nfds = (nfds > VFS_MAX_FDS) ? VFS_MAX_FDS : nfds;
        struct mini_pollfd pfds[VFS_MAX_FDS];
        for (int i = 0; i < effective_nfds; i++)
            pfds[i] = user_fds[i];

        {
            int poll_infinite = (timeout < 0);
            uint64_t poll_deadline_tsc = 0;
            if (!poll_infinite) {
                uint64_t timeout_tsc = (uint64_t)timeout * miniOS_tsc_hz / 1000;
                poll_deadline_tsc = fs_rdtsc() + timeout_tsc;
            }

            for (;;) {
                int ready = 0;
                struct thread *poll_cur = sched_current();
                for (int i = 0; i < effective_nfds; i++) {
                    int fd = pfds[i].fd;
                    pfds[i].revents = 0;
                    if (fd < 0 || fd >= VFS_MAX_FDS) {
                        pfds[i].revents = POLLNVAL;
                        ready++;
                        continue;
                    }
                    vfs_file_t *f = &THREAD_FDT(poll_cur)[fd];

                    if (!f->in_use) {
                        if (fd < VFS_FIRST_OPEN_FD) {
                            if ((pfds[i].events & POLLIN) && tty_has_data()) {
                                pfds[i].revents |= POLLIN;
                                ready++;
                            }
                            if (pfds[i].events & POLLOUT) {
                                pfds[i].revents |= POLLOUT;
                                ready++;
                            }
                        } else {
                            pfds[i].revents = POLLNVAL;
                            ready++;
                        }
                        continue;
                    }

                    if (f->ftype == VFS_FILE_TYPE_PIPE && f->pipe) {
                        pipe_t *p = f->pipe;
                        uint32_t avail = p->write_pos - p->read_pos;
                        if ((pfds[i].events & POLLIN) && avail > 0) {
                            pfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if ((pfds[i].events & POLLOUT) && avail < PIPE_SIZE) {
                            pfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else if (f->ftype == VFS_FILE_TYPE_SOCKET && f->sock) {
                        if ((pfds[i].events & POLLIN) && net_sock_has_data(f->sock)) {
                            pfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if (pfds[i].events & POLLOUT) {
                            pfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else if (f->ftype == VFS_FILE_TYPE_CHAR &&
                               f->device_type == VFS_DEVICE_TTY) {
                        if ((pfds[i].events & POLLIN) && tty_has_data()) {
                            pfds[i].revents |= POLLIN;
                            ready++;
                        }
                        if (pfds[i].events & POLLOUT) {
                            pfds[i].revents |= POLLOUT;
                            ready++;
                        }
                    } else {
                        pfds[i].revents = POLLERR;
                        ready++;
                    }
                }

                /* INTR-03: ready I/O wins over signal check */
                if (ready > 0) {
                    for (int i = 0; i < effective_nfds; i++)
                        user_fds[i].revents = pfds[i].revents;
                    return ready;
                }
                if (syscall_has_pending_signal()) {
                    /* D-03: infinite poll gets SA_RESTART; finite does not */
                    if (poll_infinite)
                        poll_cur->syscall_restart_pending = 1;
                    return -4;  /* EINTR */
                }
                if (!poll_infinite && fs_rdtsc() >= poll_deadline_tsc) {
                    for (int i = 0; i < effective_nfds; i++)
                        user_fds[i].revents = pfds[i].revents;
                    return 0;
                }
                /* Sleep if all blocking fds are sockets; net_poll_thread wakes us via
                 * sched_wake_tid() from the lwIP recv callback. Fall back to sched_yield
                 * when TTY or pipe fds are present (those have no wake mechanism yet). */
                int poll_nonsocket = 0;
                for (int i = 0; i < effective_nfds; i++) {
                    int fd = pfds[i].fd;
                    if (fd < VFS_FIRST_OPEN_FD) { poll_nonsocket = 1; break; }
                    vfs_file_t *pf = &THREAD_FDT(poll_cur)[fd];
                    if (!pf->in_use || pf->ftype != VFS_FILE_TYPE_SOCKET)
                        { poll_nonsocket = 1; break; }
                }
                if (!poll_nonsocket) {
                    for (int i = 0; i < effective_nfds; i++) {
                        int fd = pfds[i].fd;
                        if (fd >= 0 && fd < VFS_MAX_FDS) {
                            vfs_file_t *pf = &THREAD_FDT(poll_cur)[fd];
                            if (pf->in_use && pf->ftype == VFS_FILE_TYPE_SOCKET && pf->sock)
                                pf->sock->poll_waiter_tid = poll_cur->tid;
                        }
                    }
                    poll_cur->state = THREAD_FUTEX_WAIT;
                    sched_yield();
                    for (int i = 0; i < effective_nfds; i++) {
                        int fd = pfds[i].fd;
                        if (fd >= 0 && fd < VFS_MAX_FDS) {
                            vfs_file_t *pf = &THREAD_FDT(poll_cur)[fd];
                            if (pf->in_use && pf->ftype == VFS_FILE_TYPE_SOCKET && pf->sock)
                                pf->sock->poll_waiter_tid = 0;
                        }
                    }
                } else {
                    sched_yield();
                }
            }
        }
        /* unreachable */
        return 0;
    }

    case SYS_readlink: {
        const char *pathname_rl = (const char *)(uintptr_t)arg1;
        char *buf_rl = (char *)(uintptr_t)arg2;
        uint32_t bufsiz_rl = (uint32_t)arg3;
        if (!pathname_rl || !buf_rl || bufsiz_rl == 0)
            return -22;
        if ((uint64_t)pathname_rl >= KERNEL_VMA || (uint64_t)buf_rl >= KERNEL_VMA)
            return -14;
        int rl_ret = vfs_readlink(pathname_rl, buf_rl, bufsiz_rl);
        if (rl_ret == -2)  return -2;
        if (rl_ret == -22) return -22;
        if (rl_ret < 0)    return -1;
        return (int64_t)rl_ret;
    }

    case SYS_pread64: {
        int fd = (int)arg1;
        void *buf = (void *)arg2;
        uint32_t count = (uint32_t)arg3;
        uint64_t offset = arg4;
        if (fd < 0 || fd >= VFS_MAX_FDS || buf == NULL || count == 0)
            return -22;
        struct thread *t = sched_current();
        vfs_file_t *f = &THREAD_FDT(t)[fd];
        if (!f->in_use || f->ftype != VFS_FILE_TYPE_REG || !f->ops || !f->ops->read)
            return -22;
        return (int64_t)f->ops->read(f->inode, offset, buf, count);
    }

    case SYS_pwrite64: {
        int fd = (int)arg1;
        const void *buf = (const void *)arg2;
        uint32_t count = (uint32_t)arg3;
        uint64_t offset = arg4;
        if (fd < 0 || fd >= VFS_MAX_FDS || buf == NULL || count == 0)
            return -22;
        struct thread *t = sched_current();
        vfs_file_t *f = &THREAD_FDT(t)[fd];
        if (!f->in_use || f->ftype != VFS_FILE_TYPE_REG || !f->ops || !f->ops->write)
            return -22;
        int n = f->ops->write(f->inode, offset, buf, count);
        if (n > 0 && (offset + (uint64_t)n) > f->size)
            f->size = offset + (uint64_t)n;
        return (int64_t)n;
    }

    case SYS_ftruncate: {
        int fd = (int)arg1;
        uint64_t length = arg2;
        if (fd < 0 || fd >= VFS_MAX_FDS)
            return -22;
        struct thread *t = sched_current();
        vfs_file_t *f = &THREAD_FDT(t)[fd];
        if (!f->in_use || f->ftype != VFS_FILE_TYPE_REG || !f->ops)
            return -22;
        if (length == f->size)
            return 0;
        if (!f->ops->truncate)
            return -38;
        int r = f->ops->truncate(f->inode, length);
        if (r == 0) f->size = length;
        return (int64_t)r;
    }

    default:
        return SYSCALL_DISPATCH_UNHANDLED;
    }
}
