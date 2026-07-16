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

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <termios.h>
#include <signal.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <miniOS/syscall.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* x86_64 SYSCALL instruction wrapper.
 * RAX=nr, RDI=arg1, RSI=arg2, RDX=arg3.
 * SYSCALL clobbers RCX and R11. */
static inline long inline_syscall(long nr, long arg1, long arg2, long arg3) {
    long ret;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );

    return ret;
}

/* 6-argument SYSCALL wrapper for mmap.
 * Linux x86-64 mmap ABI: rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=offset.
 * Note: arg4 goes in r10 (not rcx) per Linux syscall convention. */
static inline long inline_syscall6(long nr, long arg1, long arg2, long arg3,
                                    long arg4, long arg5, long arg6) {
    long ret;
    register long r10 __asm__("r10") = arg4;
    register long r8  __asm__("r8")  = arg5;
    register long r9  __asm__("r9")  = arg6;

    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );

    return ret;
}

static char *_brk_ptr;
int h_errno;

void *_sbrk(ptrdiff_t incr) {
    char *old_brk;
    long new_brk;
    long res;

    if (_brk_ptr == NULL) {
        res = inline_syscall(79, 0, 0, 0);
        if (res <= 0) {
            errno = (res < 0) ? (int)(-res) : ENOMEM;
            return (void *)-1;
        }
        _brk_ptr = (char *)res;
    }

    old_brk = _brk_ptr;
    new_brk = (long)(_brk_ptr + incr);
    res = inline_syscall(79, new_brk, 0, 0);
    if (res < 0) {
        errno = (int)(-res);
        return (void *)-1;
    }

    _brk_ptr = (char *)res;
    return (void *)old_brk;
}

void *sbrk(ptrdiff_t incr) {
    return _sbrk(incr);
}

ssize_t _write(int fd, const void *buf, size_t count) {
    long ret = inline_syscall(1, fd, (long)buf, (long)count);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (ssize_t)ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    return _write(fd, buf, count);
}

ssize_t _read(int fd, void *buf, size_t count) {
    long ret = inline_syscall(0, fd, (long)buf, (long)count);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (ssize_t)ret;
}

ssize_t read(int fd, void *buf, size_t count) {
    return _read(fd, buf, count);
}

int _open(const char *path, int flags, ...) {
    long ret = inline_syscall(2, (long)path, flags, 0);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

int open(const char *path, int flags, ...) {
    return _open(path, flags);
}

int _close(int fd) {
    long ret = inline_syscall(3, fd, 0, 0);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

int close(int fd) {
    return _close(fd);
}

off_t _lseek(int fd, off_t offset, int whence) {
    long ret = inline_syscall(8, (long)fd, (long)offset, (long)whence);
    if (ret < 0) {
        errno = (int)(-ret);
        return (off_t)-1;
    }
    return (off_t)ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    return _lseek(fd, offset, whence);
}

void _exit(int code) {
    inline_syscall(60, code, 0, 0);
    for (;;) {
    }
}

void exit(int code) {
    _exit(code);
}



int _fstat(int fd, struct stat *st) {
    long ret = inline_syscall(5, (long)fd, (long)st, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int fstat(int fd, struct stat *st) {
    return _fstat(fd, st);
}

int _stat(const char *path, struct stat *st) {
    long ret = inline_syscall(4, (long)path, (long)st, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int stat(const char *path, struct stat *st) {
    return _stat(path, st);
}

pid_t _getpid(void) {
    return (pid_t)inline_syscall(39, 0, 0, 0);
}

pid_t getpid(void) {
    return _getpid();
}

pid_t getppid(void) {
    return (pid_t)inline_syscall(110, 0, 0, 0);
}

pid_t setsid(void) {
    long ret = inline_syscall(112, 0, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return (pid_t)-1;
    }
    return (pid_t)ret;
}

int setpgid(pid_t pid, pid_t pgid) {
    long ret = inline_syscall(109, (long)pid, (long)pgid, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

int kill(pid_t pid, int sig) {
    return (int)inline_syscall(62, (long)pid, (long)sig, 0);
}

pid_t fork(void) {
    return (pid_t)inline_syscall(57, 0, 0, 0);
}

int exec(const char *path) {
    long ret = inline_syscall(59, (long)path, 0, 0);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

int execve(const char *path, char *const *argv, char *const *envp) {
    long ret = inline_syscall(59, (long)path, (long)argv, (long)envp);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

extern char **environ;

int execv(const char *path, char *const *argv) {
    return execve(path, argv, environ);
}

/* Local helper — avoids dependence on shadowed string.h */
static const char *_find_colon(const char *s) {
    while (*s && *s != ':') s++;
    return *s == ':' ? s : NULL;
}
static const char *_find_slash(const char *s) {
    while (*s && *s != '/') s++;
    return *s == '/' ? s : NULL;
}

int execvp(const char *file, char *const *argv) {
    /* If file contains a slash, exec directly. */
    if (file && _find_slash(file))
        return execve(file, argv, environ);

    /* Search PATH */
    const char *path_env = getenv("PATH");
    if (!path_env)
        path_env = "/bin:/usr/bin:/sbin:/usr/sbin";

    char buf[4096];
    const char *p = path_env;
    while (p && *p) {
        const char *end = _find_colon(p);
        size_t dirlen = end ? (size_t)(end - p) : strlen(p);
        if (dirlen + 1 + strlen(file) + 1 > sizeof(buf)) {
            p = end ? end + 1 : NULL;
            continue;
        }
        memcpy(buf, p, dirlen);
        buf[dirlen] = '/';
        strcpy(buf + dirlen + 1, file);
        execve(buf, argv, environ);
        if (errno != ENOENT)
            return -1;
        p = end ? end + 1 : NULL;
    }
    errno = ENOENT;
    return -1;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    long ret;

    (void)options;   /* WNOHANG not yet supported — blocking wait only */
    ret = inline_syscall(7, (long)pid, (long)status, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (pid_t)ret;
}

pid_t _libc_wait(int *status) {
    return waitpid(0, status, 0);
}

/* pipe() — SYS_pipe (nr=22); fds[0]=read end, fds[1]=write end */
int pipe(int fds[2]) {
    return (int)inline_syscall(22, (long)fds, 0, 0);
}

/* dup() — SYS_dup (nr=32); returns new fd or -1 on error */
int dup(int oldfd) {
    return (int)inline_syscall(32, oldfd, 0, 0);
}

int dup2(int oldfd, int newfd) {
    long ret = inline_syscall(33, (long)oldfd, (long)newfd, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int _ioctl(int fd, unsigned long request, void *arg) {
    long ret = inline_syscall(16, (long)fd, (long)request, (long)arg);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    return _ioctl(fd, request, arg);
}



long getdents(int fd, void *buf, unsigned int count) {
    long ret = inline_syscall(78, (long)fd, (long)buf, (long)count);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return ret;
}

/* SYS_mmap = 9 (Linux x86-64 ABI) */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long offset) {
    long ret = inline_syscall6(9, (long)addr, (long)len, (long)prot,
                               (long)flags, (long)fd, offset);

    if (ret < 0) {
        errno = (int)(-ret);
        return MAP_FAILED;
    }

    return (void *)ret;
}

/* SYS_munmap = 11 (Linux x86-64 ABI) */
int munmap(void *addr, size_t len) {
    long ret = inline_syscall(11, (long)addr, (long)len, 0);

    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }

    return (int)ret;
}

/* SYS_mount = 165, SYS_umount = 166 (Linux x86-64 ABI) */
int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    long ret = inline_syscall6(165, (long)source, (long)target, (long)fstype,
                               (long)flags, (long)data, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int umount(const char *target) {
    long ret = inline_syscall(166, (long)target, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* SYS_mkdir = 83 (Linux x86-64 ABI) */
int mkdir(const char *path, mode_t mode) {
    long ret = inline_syscall(83, (long)path, (long)mode, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int rmdir(const char *path) {
    long ret = inline_syscall(84, (long)path, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* SYS_unlink = 87 (Linux x86-64 ABI) */
int unlink(const char *path) {
    long ret = inline_syscall(87, (long)path, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

int access(const char *path, int mode) {
    long ret = inline_syscall(21, (long)path, (long)mode, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* SYS_signal = 13 (Linux x86-64 ABI: legacy signal handler registration)
 * Use function-pointer typedef directly to avoid sighandler_t visibility
 * differences between Newlib sysroot and host includes. */
typedef void (*_miniOS_sighandler_t)(int);

_miniOS_sighandler_t signal(int sig, _miniOS_sighandler_t handler) {
    return (_miniOS_sighandler_t)(uintptr_t)inline_syscall(13, (long)sig, (long)(uintptr_t)handler, 0);
}

/* SYS_sigaction = 174 (Linux x86-64 ABI: full signal handler registration) */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    return (int)inline_syscall(174, (long)sig, (long)(uintptr_t)act, (long)(uintptr_t)oact);
}

/* chdir() — change current working directory via SYS_chdir (nr=81).
 * Returns 0 on success, -1 on error with errno set. */
int chdir(const char *path) {
    long ret = inline_syscall(81, (long)path, 0, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

/* getcwd() — copy current working directory to buf via SYS_getcwd (nr=80).
 * Returns buf on success, NULL on error with errno set.
 * buf must be at least size bytes; size must be > 0. */
char *getcwd(char *buf, size_t size) {
    long ret = inline_syscall(80, (long)buf, (long)size, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return NULL;
    }
    return buf;
}

/* poll() — wait for events on file descriptors via SYS_poll (nr=23).
 * fds: array of pollfd structs; nfds: array length; timeout: milliseconds (-1=infinite).
 * Returns number of fds with events, 0 on timeout, -1 on error with errno set. */
int poll(struct pollfd *fds, unsigned int nfds, int timeout) {
    long ret = inline_syscall(23, (long)(uintptr_t)fds, (long)nfds, (long)timeout);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* select() — I/O multiplexing via SYS_select (nr=24).
 * nfds: highest fd + 1; readfds/writefds: fd_set bitmasks (NULL = ignore).
 * timeout: NULL=infinite; {0,0}=non-blocking; else millisecond-equivalent wait.
 * Returns count of ready fds, 0 on timeout, -1 on error with errno set. */
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    (void)exceptfds;  /* not implemented; caller must pass NULL or tolerate it being ignored */
    long ret = inline_syscall6(24, (long)nfds,
                               (long)(uintptr_t)readfds,
                               (long)(uintptr_t)writefds,
                               (long)(uintptr_t)exceptfds,
                               (long)(uintptr_t)timeout, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* fcntl() — file descriptor control via SYS_fcntl (nr=72).
 * cmd=3 (F_GETFL): returns access mode + status flags; cmd=4 (F_SETFL): sets status flags.
 * Returns 0 or flags value on success, -1 on error with errno set. */
int fcntl(int fd, int cmd, ...) {
    /* Variadic: extract third arg as long for F_SETFL; 0 if not provided */
    long arg = 0;
    if (cmd == 4) {  /* F_SETFL */
        __builtin_va_list ap;
        __builtin_va_start(ap, cmd);
        arg = __builtin_va_arg(ap, long);
        __builtin_va_end(ap);
    }
    long ret = inline_syscall(72, (long)fd, (long)cmd, arg);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (int)ret;
}

/* lstat() — query file metadata without following symlinks via SYS_lstat (nr=6).
 * Returns 0 on success, -1 on error with errno set. */
int lstat(const char *path, struct stat *st) {
    long ret = inline_syscall(6, (long)path, (long)(uintptr_t)st, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

/* readlink() — read target of a symlink via SYS_readlink (nr=89).
 * Returns number of bytes written to buf on success, -1 on error with errno set.
 * Phase 27: always returns -1/EINVAL for non-symlink paths. */
ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    long ret = inline_syscall(89, (long)path, (long)(uintptr_t)buf, (long)bufsiz);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return (ssize_t)ret;
}

/* sleep() — suspend execution for @seconds via SYS_nanosleep (nr=35).
 * Returns 0 on completion. */
unsigned int sleep(unsigned int seconds) {
    struct { unsigned long tv_sec; unsigned long tv_nsec; } req = { seconds, 0 };
    inline_syscall(35, (long)(unsigned long)&req, 0, 0);
    return 0;
}

/* nanosleep() — high-resolution sleep via SYS_nanosleep (nr=35).
 * Returns 0 on completion, -1 with errno=EINTR if interrupted by a signal. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
    long ret = inline_syscall(35, (long)(unsigned long)req, (long)(unsigned long)rem, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

unsigned alarm(unsigned seconds) {
    long ret = inline_syscall(SYS_alarm_miniOS, (long)seconds, 0, 0);
    if (ret < 0)
        return 0;
    return (unsigned)ret;
}

int setitimer(int which, const struct itimerval *new_value, struct itimerval *old_value) {
    long ret = inline_syscall(SYS_setitimer_miniOS, (long)which,
                              (long)(uintptr_t)new_value, (long)(uintptr_t)old_value);
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return 0;
}

/* tcsetpgrp(fd, pgrp) — set the foreground process group of tty @fd.
 * Uses TIOCSPGRP ioctl (0x5410). Returns 0 on success, -1 on error. */
int tcsetpgrp(int fd, pid_t pgrp) {
    int pgid = (int)pgrp;
    long ret = inline_syscall(SYS_ioctl, (long)fd, 0x5410L, (long)(uintptr_t)&pgid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* tcgetpgrp(fd) — get the foreground process group of tty @fd.
 * Uses TIOCGPGRP ioctl (0x540F). Returns pgid on success, -1 on error. */
pid_t tcgetpgrp(int fd) {
    int pgid = 0;
    long ret = inline_syscall(SYS_ioctl, (long)fd, 0x540FL, (long)(uintptr_t)&pgid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)pgid;
}

unsigned int if_nametoindex(const char *ifname)
{
    if (!ifname)
        return 0;
    if (ifname[0] == 'e' && ifname[1] == 't' && ifname[2] == 'h' && ifname[3] == '0' && ifname[4] == 0)
        return 1;
    if (ifname[0] == 'l' && ifname[1] == 'o' && ifname[2] == 0)
        return 2;
    return 0;
}

char *if_indextoname(unsigned int ifindex, char *ifname)
{
    const char *name = NULL;
    if (!ifname) {
        errno = EINVAL;
        return NULL;
    }
    if (ifindex == 1)
        name = "eth0";
    else if (ifindex == 2)
        name = "lo";
    else {
        errno = ENXIO;
        return NULL;
    }
    strcpy(ifname, name);
    return ifname;
}

struct if_nameindex *if_nameindex(void)
{
    struct if_nameindex *list = calloc(3, sizeof(*list));
    if (!list)
        return NULL;
    list[0].if_index = 1;
    list[0].if_name = malloc(5);
    list[1].if_index = 2;
    list[1].if_name = malloc(3);
    if (!list[0].if_name || !list[1].if_name) {
        free(list[0].if_name);
        free(list[1].if_name);
        free(list);
        return NULL;
    }
    memcpy(list[0].if_name, "eth0", 5);
    memcpy(list[1].if_name, "lo", 3);
    return list;
}

void if_freenameindex(struct if_nameindex *ptr)
{
    if (!ptr)
        return;
    for (struct if_nameindex *p = ptr; p->if_index != 0; p++)
        free(p->if_name);
    free(ptr);
}

/* clock_gettime() — SYS_clock_gettime = 228.
 * Per D-06: reads CLOCK_MONOTONIC backed by TSC for sub-ms RTT measurement.
 * Returns 0 on success, -1 with errno set on failure. */
int clock_gettime(clockid_t clockid, struct timespec *tp) {
    long ret = inline_syscall(228, (long)clockid, (long)(unsigned long)tp, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* sched_getcpu() — return current CPU ID via SYS_sched_getcpu (nr=318).
 * Returns CPU index (0 = BSP, 1..N-1 = APs). No arguments.
 * Mirrors Linux sched_getcpu(3) semantics. */
int sched_getcpu(void) {
    return (int)inline_syscall(318, 0, 0, 0);
}

/* fchdir — SYS_fchdir = 81 */
int fchdir(int fd) {
    long ret = inline_syscall(81, fd, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* chroot — SYS_chroot = 161 */
int chroot(const char *path) {
    long ret = inline_syscall(161, (long)path, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* settimeofday — SYS_settimeofday = 164 */
struct timezone;
int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    long ret = inline_syscall(164, (long)tv, (long)tz, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* setuid/setgid — SYS_setuid=105, SYS_setgid=106 */
int setuid(unsigned int uid) {
    long ret = inline_syscall(105, uid, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
int setgid(unsigned int gid) {
    long ret = inline_syscall(106, gid, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* seteuid/setegid — via SYS_setresuid=117 / SYS_setresgid=119; keep ruid/rgid unchanged (-1) */
int seteuid(unsigned int euid) {
    long ret = inline_syscall(117, (long)(unsigned int)-1, (long)euid, (long)(unsigned int)-1);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}
int setegid(unsigned int egid) {
    long ret = inline_syscall(119, (long)(unsigned int)-1, (long)egid, (long)(unsigned int)-1);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* setgroups — SYS_setgroups = 116 */
int setgroups(int size, const unsigned int *list) {
    long ret = inline_syscall(116, (long)size, (long)list, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* vfork — miniOS doesn't implement vfork; fall back to fork (SYS_fork=57) */
int vfork(void) {
    long ret = inline_syscall(57, 0, 0, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* link — SYS_link = 86 */
int link(const char *oldpath, const char *newpath) {
    long ret = inline_syscall(86, (long)oldpath, (long)newpath, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* socket — SYS_socket = 41 */
int socket(int domain, int type, int protocol) {
    long ret = inline_syscall(41, domain, type, protocol);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* bind — SYS_bind = 49 */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    long ret = inline_syscall(49, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* listen — SYS_listen = 50 */
int listen(int sockfd, int backlog) {
    long ret = inline_syscall(50, sockfd, backlog, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* sendto — SYS_sendto = 44 */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    register long r10 __asm__("r10") = (long)dest_addr;
    register long r8  __asm__("r8")  = (long)addrlen;
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(44L), "D"((long)sockfd), "S"((long)buf), "d"((long)len),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

/* ttyname_r — miniOS has no /dev filesystem; return ENOTTY for non-tty fds. */
int ttyname_r(int fd, char *buf, size_t buflen) {
    (void)fd; (void)buf; (void)buflen;
    errno = ENOTTY;
    return ENOTTY;
}

/* realpath — resolve canonical path; miniOS has no symlinks, so copy the path. */
char *realpath(const char *path, char *resolved) {
    if (!path) { errno = EINVAL; return NULL; }
    if (!resolved) {
        resolved = malloc(4096);
        if (!resolved) { errno = ENOMEM; return NULL; }
    }
    /* Simple copy without resolving .. or symlinks */
    size_t n = strlen(path);
    if (n >= 4096) { errno = ENAMETOOLONG; return NULL; }
    memcpy(resolved, path, n + 1);
    return resolved;
}

/* -------------------------------------------------------------------------
 * opendir / readdir / closedir — implement via SYS_getdents (nr=78).
 * The kernel fills linux_dirent64-compatible records (packed):
 *   uint64_t d_ino, int64_t d_off, uint16_t d_reclen, uint8_t d_type,
 *   char d_name[...] — exactly matching our struct dirent layout.
 * ------------------------------------------------------------------------- */
#include <dirent.h>
#include <fcntl.h>

#define DIR_BUF_SIZE 4096

struct __dirstream {
    int fd;
    char buf[DIR_BUF_SIZE];
    int buf_pos;
    int buf_end;
    struct dirent cur;  /* storage for the dirent we return */
};

DIR *opendir(const char *path) {
    int fd = open(path, 0200000 /* O_DIRECTORY */ | 0 /* O_RDONLY */);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof(*d));
    if (!d) { close(fd); errno = ENOMEM; return NULL; }
    d->fd      = fd;
    d->buf_pos = 0;
    d->buf_end = 0;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return NULL; }
    for (;;) {
        if (dirp->buf_pos >= dirp->buf_end) {
            long n = inline_syscall(78, dirp->fd, (long)(unsigned long)dirp->buf, DIR_BUF_SIZE);
            if (n <= 0) return NULL;   /* end of directory or error */
            dirp->buf_pos = 0;
            dirp->buf_end = (int)n;
        }
        /* linux_dirent64 (packed) at buf_pos */
        typedef struct {
            unsigned long long d_ino;
            long long          d_off;
            unsigned short     d_reclen;
            unsigned char      d_type;
            char               d_name[];
        } __attribute__((packed)) kdirent_t;
        kdirent_t *kd = (kdirent_t *)(dirp->buf + dirp->buf_pos);
        dirp->buf_pos += kd->d_reclen;
        /* Copy into stable storage and return pointer to it */
        dirp->cur.d_ino    = kd->d_ino;
        dirp->cur.d_off    = kd->d_off;
        dirp->cur.d_reclen = kd->d_reclen;
        dirp->cur.d_type   = kd->d_type;
        /* Safe copy of name — kd->d_name is null-terminated */
        size_t nlen = 0;
        while (kd->d_name[nlen] && nlen < sizeof(dirp->cur.d_name) - 1) nlen++;
        __builtin_memcpy(dirp->cur.d_name, kd->d_name, nlen);
        dirp->cur.d_name[nlen] = '\0';
        return &dirp->cur;
    }
}

int closedir(DIR *dirp) {
    if (!dirp) { errno = EBADF; return -1; }
    int r = close(dirp->fd);
    free(dirp);
    return r;
}

void rewinddir(DIR *dirp) {
    if (!dirp) return;
    inline_syscall(8 /* SYS_lseek */, dirp->fd, 0, 0 /* SEEK_SET */);
    dirp->buf_pos = 0;
    dirp->buf_end = 0;
}

/* connect — SYS_connect = 42 */
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    long ret = inline_syscall(42, fd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* recvfrom — SYS_recvfrom = 45 */
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    register long r10 __asm__("r10") = (long)flags;
    register long r8  __asm__("r8")  = (long)src_addr;
    register long r9  __asm__("r9")  = (long)addrlen;
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(45L), "D"((long)sockfd), "S"((long)buf), "d"((long)len),
          "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

/* setsockopt — SYS_setsockopt = 54 (Linux x86-64 ABI) */
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    long ret = inline_syscall6(54, (long)fd, (long)level, (long)optname,
                                (long)(uintptr_t)optval, (long)optlen, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* getsockopt — SYS_getsockopt = 55 (Linux x86-64 ABI); kernel returns ENOPROTOOPT (D-07 stub) */
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{
    long ret = inline_syscall6(55, (long)fd, (long)level, (long)optname,
                                (long)(uintptr_t)optval, (long)(uintptr_t)optlen, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd;
    if (!addr || !addrlen || *addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_in *sin = (struct sockaddr_in *)(void *)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    *addrlen = (socklen_t)sizeof(*sin);
    return 0;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    return getsockname(fd, addr, addrlen);
}

/* shutdown — SYS_shutdown = 48 */
int shutdown(int sockfd, int how)
{
    long ret = inline_syscall(48, sockfd, how, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* accept — SYS_accept = 43 */
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    long ret = inline_syscall(43, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static char *dup_cstr(const char *src)
{
    size_t len = strlen(src) + 1;
    char *copy = malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, src, len);
    return copy;
}

static int parse_ipv4_octet(const char **cpp, unsigned *out)
{
    const char *cp = *cpp;
    unsigned value = 0;
    int digits = 0;
    while (*cp >= '0' && *cp <= '9') {
        value = value * 10u + (unsigned)(*cp - '0');
        if (value > 255u)
            return -1;
        cp++;
        digits++;
    }
    if (digits == 0)
        return -1;
    *cpp = cp;
    *out = value;
    return 0;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr addr;
    if (inet_aton(cp, &addr) == 0)
        return INADDR_NONE;
    return addr.s_addr;
}

int inet_aton(const char *cp, struct in_addr *inp)
{
    if (!cp || !inp) {
        errno = EINVAL;
        return 0;
    }

    unsigned octets[4];
    for (int i = 0; i < 4; i++) {
        if (parse_ipv4_octet(&cp, &octets[i]) != 0)
            return 0;
        if (i != 3) {
            if (*cp != '.')
                return 0;
            cp++;
        }
    }
    if (*cp != '\0')
        return 0;

    inp->s_addr = htonl((octets[0] << 24) | (octets[1] << 16) |
                        (octets[2] << 8) | octets[3]);
    return 1;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[16];
    uint32_t host = ntohl(in.s_addr);
    unsigned a = (host >> 24) & 0xffu;
    unsigned b = (host >> 16) & 0xffu;
    unsigned c = (host >> 8) & 0xffu;
    unsigned d = host & 0xffu;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a, b, c, d);
    return buf;
}

static int parse_numeric_service(const char *service, int socktype, int *out_port)
{
    (void)socktype;
    if (!service || !out_port)
        return -1;
    char *end = NULL;
    long port = strtol(service, &end, 10);
    if (!end || *end != '\0' || port < 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    *out_port = (int)port;
    return 0;
}

static struct servent g_service;
static char *g_service_aliases[] = { NULL };
static struct hostent g_hostent;
static char *g_hostent_aliases[] = { NULL };
static char *g_hostent_addr_list[] = { NULL, NULL };
static struct in_addr g_hostent_addr;
static char g_hostent_name[256];

struct servent *getservbyname(const char *name, const char *proto)
{
    (void)proto;
    if (!name)
        return NULL;

    int port = 0;
    if (streq(name, "echo")) {
        port = 7;
    } else if (streq(name, "http")) {
        port = 80;
    } else if (streq(name, "https")) {
        port = 443;
    } else {
        return NULL;
    }

    g_service.s_name = (char *)name;
    g_service.s_aliases = g_service_aliases;
    g_service.s_port = htons((uint16_t)port);
    g_service.s_proto = (char *)(proto ? proto : "tcp");
    return &g_service;
}

/* ── Userspace POSIX DNS resolver (RFC 1035, A + CNAME) ─────────────────── */

#define DNS_PORT            53
#define DNS_TIMEOUT_MS      5000
#define DNS_BUF_SIZE        512
#define DNS_NAME_MAX        256
#define DNS_MAX_CHAIN       8

static uint32_t dns_read_nameserver(void)
{
    int fd = open("/etc/resolv.conf", 0 /* O_RDONLY */, 0);
    if (fd < 0)
        return 0;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    char *p = buf;
    while (*p) {
        if (strncmp(p, "nameserver", 10) == 0) {
            p += 10;
            while (*p == ' ' || *p == '\t') p++;
            char *end = p;
            while (*end && *end != '\n' && *end != '\r') end++;
            *end = '\0';
            struct in_addr a;
            if (inet_aton(p, &a))
                return a.s_addr;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

static int dns_encode_name(const char *name, uint8_t *out, int outlen)
{
    int pos = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int lablen = (int)(dot - p);
        if (lablen == 0 || lablen > 63 || pos + lablen + 1 >= outlen)
            return -1;
        out[pos++] = (uint8_t)lablen;
        memcpy(out + pos, p, (size_t)lablen);
        pos += lablen;
        p = (*dot == '.') ? dot + 1 : dot;
    }
    if (pos + 1 >= outlen)
        return -1;
    out[pos++] = 0;
    return pos;
}

/* Decode wire-format name at pkt[off] into out[] (NUL-terminated, lowercase).
 * Follows compression pointers. Returns offset of next field after the name
 * in the original packet stream, or -1 on error. */
static int dns_decode_name(const uint8_t *pkt, int pktlen, int off,
                           char *out, int outlen)
{
    int pos = 0;
    int ret_off = -1;
    int hops = 0;

    while (off < pktlen && hops < 128) {
        uint8_t b = pkt[off];
        if ((b & 0xC0) == 0xC0) {
            if (off + 2 > pktlen) return -1;
            if (ret_off < 0) ret_off = off + 2;
            off = ((int)(b & 0x3F) << 8) | pkt[off + 1];
            hops++;
            continue;
        }
        if (b == 0) {
            if (pos > 0 && out[pos - 1] == '.') pos--; /* strip trailing dot */
            if (pos >= outlen) return -1;
            out[pos] = '\0';
            return (ret_off >= 0) ? ret_off : off + 1;
        }
        int lablen = (int)b;
        off++;
        if (off + lablen > pktlen || pos + lablen + 1 >= outlen)
            return -1;
        for (int i = 0; i < lablen; i++) {
            char c = (char)pkt[off + i];
            out[pos++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        out[pos++] = '.';
        off += lablen;
    }
    return -1;
}

static int dns_nameeq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int resolve_ipv4_hostname(const char *node, struct in_addr *out_addr)
{
    if (!node || !out_addr) {
        errno = EINVAL;
        return -1;
    }

    if (inet_aton(node, out_addr))
        return 0;

    uint32_t ns = dns_read_nameserver();
    if (!ns) {
        h_errno = NO_RECOVERY;
        errno = EIO;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        h_errno = NO_RECOVERY;
        return -1;
    }

    uint8_t pkt[DNS_BUF_SIZE];
    /* Header */
    pkt[0] = 0x13; pkt[1] = 0x37;   /* txid */
    pkt[2] = 0x01; pkt[3] = 0x00;   /* flags: RD=1 */
    pkt[4] = 0x00; pkt[5] = 0x01;   /* QDCOUNT=1 */
    pkt[6] = 0x00; pkt[7] = 0x00;   /* ANCOUNT=0 */
    pkt[8] = 0x00; pkt[9] = 0x00;   /* NSCOUNT=0 */
    pkt[10] = 0x00; pkt[11] = 0x00; /* ARCOUNT=0 */

    int qname_len = dns_encode_name(node, pkt + 12, DNS_BUF_SIZE - 12 - 4);
    if (qname_len < 0) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    int qoff = 12 + qname_len;
    pkt[qoff++] = 0x00; pkt[qoff++] = 0x01; /* QTYPE=A */
    pkt[qoff++] = 0x00; pkt[qoff++] = 0x01; /* QCLASS=IN */

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(DNS_PORT);
    srv.sin_addr.s_addr = ns;

    if (sendto(fd, pkt, (size_t)qoff, 0,
               (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        close(fd);
        h_errno = NO_RECOVERY;
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, DNS_TIMEOUT_MS) <= 0) {
        close(fd);
        h_errno = TRY_AGAIN;
        errno = ETIMEDOUT;
        return -1;
    }

    uint8_t resp[DNS_BUF_SIZE];
    ssize_t rlen = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
    close(fd);
    if (rlen < 12) {
        h_errno = NO_RECOVERY;
        errno = EIO;
        return -1;
    }

    /* Validate txid and RCODE */
    if (resp[0] != 0x13 || resp[1] != 0x37) {
        h_errno = NO_RECOVERY;
        errno = EIO;
        return -1;
    }
    if ((resp[3] & 0x0F) != 0) {
        h_errno = HOST_NOT_FOUND;
        errno = ENOENT;
        return -1;
    }

    uint16_t qdcount = ((uint16_t)resp[4] << 8) | resp[5];
    uint16_t ancount = ((uint16_t)resp[6] << 8) | resp[7];
    if (ancount == 0) {
        h_errno = HOST_NOT_FOUND;
        errno = ENOENT;
        return -1;
    }

    /* Skip question section */
    int off = 12;
    for (uint16_t q = 0; q < qdcount; q++) {
        char discard[DNS_NAME_MAX];
        off = dns_decode_name(resp, (int)rlen, off, discard, sizeof(discard));
        if (off < 0 || off + 4 > (int)rlen) {
            h_errno = NO_RECOVERY;
            errno = EIO;
            return -1;
        }
        off += 4; /* QTYPE + QCLASS */
    }

    /* Collect CNAME and A records from answer section */
    struct { char from[DNS_NAME_MAX]; char to[DNS_NAME_MAX]; } cnames[DNS_MAX_CHAIN];
    struct { char name[DNS_NAME_MAX]; uint32_t addr; }          a_recs[DNS_MAX_CHAIN];
    int ncnames = 0, na_recs = 0;

    for (uint16_t a = 0; a < ancount; a++) {
        char rname[DNS_NAME_MAX];
        int noff = dns_decode_name(resp, (int)rlen, off, rname, sizeof(rname));
        if (noff < 0 || noff + 10 > (int)rlen)
            break;
        uint16_t type  = ((uint16_t)resp[noff]   << 8) | resp[noff + 1];
        uint16_t rdlen = ((uint16_t)resp[noff + 8] << 8) | resp[noff + 9];
        int rdata = noff + 10;
        if (rdata + rdlen > (int)rlen)
            break;

        if (type == 5 && ncnames < DNS_MAX_CHAIN) { /* CNAME */
            memcpy(cnames[ncnames].from, rname, sizeof(cnames[ncnames].from));
            if (dns_decode_name(resp, (int)rlen, rdata,
                                cnames[ncnames].to,
                                sizeof(cnames[ncnames].to)) >= 0)
                ncnames++;
        } else if (type == 1 && rdlen == 4 && na_recs < DNS_MAX_CHAIN) { /* A */
            memcpy(a_recs[na_recs].name, rname, sizeof(a_recs[na_recs].name));
            memcpy(&a_recs[na_recs].addr, resp + rdata, 4);
            na_recs++;
        }

        off = rdata + rdlen;
    }

    /* Follow CNAME chain from query name until an A record is found */
    char current[DNS_NAME_MAX];
    int clen = (int)strlen(node);
    if (clen >= DNS_NAME_MAX) clen = DNS_NAME_MAX - 1;
    for (int i = 0; i < clen; i++)
        current[i] = (node[i] >= 'A' && node[i] <= 'Z') ? node[i] + 32 : node[i];
    current[clen] = '\0';

    for (int depth = 0; depth <= DNS_MAX_CHAIN; depth++) {
        for (int i = 0; i < na_recs; i++) {
            if (dns_nameeq(a_recs[i].name, current)) {
                memcpy(&out_addr->s_addr, &a_recs[i].addr, 4);
                return 0;
            }
        }
        int followed = 0;
        for (int i = 0; i < ncnames; i++) {
            if (dns_nameeq(cnames[i].from, current)) {
                memcpy(current, cnames[i].to, DNS_NAME_MAX);
                followed = 1;
                break;
            }
        }
        if (!followed)
            break;
    }

    h_errno = HOST_NOT_FOUND;
    errno = ENOENT;
    return -1;
}

struct hostent *gethostbyname(const char *name)
{
    if (!name) {
        h_errno = HOST_NOT_FOUND;
        errno = EINVAL;
        return NULL;
    }

    if (strlen(name) >= sizeof(g_hostent_name)) {
        h_errno = NO_RECOVERY;
        errno = ENAMETOOLONG;
        return NULL;
    }

    if (resolve_ipv4_hostname(name, &g_hostent_addr) != 0)
        return NULL;

    memcpy(g_hostent_name, name, strlen(name) + 1);
    g_hostent.h_name = g_hostent_name;
    g_hostent.h_aliases = g_hostent_aliases;
    g_hostent.h_addrtype = AF_INET;
    g_hostent.h_length = sizeof(g_hostent_addr);
    g_hostent_addr_list[0] = (char *)&g_hostent_addr;
    g_hostent_addr_list[1] = NULL;
    g_hostent.h_addr_list = g_hostent_addr_list;
    return &g_hostent;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    if (!res)
        return EAI_FAIL;
    *res = NULL;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;

    int port = 0;
    if (service && parse_numeric_service(service, hints ? hints->ai_socktype : 0, &port) != 0) {
        struct servent *sv = getservbyname(service,
            (hints && hints->ai_socktype == SOCK_DGRAM) ? "udp" : "tcp");
        if (!sv)
            return EAI_SERVICE;
        port = ntohs((uint16_t)sv->s_port);
    }

    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sin = calloc(1, sizeof(*sin));
    if (!ai || !sin) {
        free(ai);
        free(sin);
        return EAI_MEMORY;
    }

    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = (unsigned int)sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)(void *)sin;

    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)port);

    if (!node) {
        sin->sin_addr.s_addr = (hints && (hints->ai_flags & AI_PASSIVE))
            ? htonl(INADDR_ANY)
            : htonl(INADDR_LOOPBACK);
    } else if (resolve_ipv4_hostname(node, &sin->sin_addr) != 0) {
        free(ai);
        free(sin);
        return (errno == ETIMEDOUT) ? EAI_AGAIN : EAI_NONAME;
    }

    if (hints && (hints->ai_flags & AI_CANONNAME)) {
        if (node) {
            ai->ai_canonname = dup_cstr(node);
            if (!ai->ai_canonname) {
                free(ai);
                free(sin);
                return EAI_MEMORY;
            }
        }
    }

    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

int getnameinfo(const struct sockaddr *addr, unsigned int addrlen,
                char *host, unsigned int hostlen,
                char *serv, unsigned int servlen, int flags)
{
    (void)flags;
    if (!addr || addrlen < sizeof(struct sockaddr_in))
        return EAI_FAIL;
    if (addr->sa_family != AF_INET)
        return EAI_FAMILY;

    const struct sockaddr_in *sin = (const struct sockaddr_in *)(const void *)addr;
    if (host && hostlen > 0) {
        char *name = inet_ntoa(sin->sin_addr);
        if (strlen(name) + 1 > hostlen)
            return EAI_OVERFLOW;
        strcpy(host, name);
    }
    if (serv && servlen > 0) {
        unsigned port = (unsigned)ntohs(sin->sin_port);
        int written = snprintf(serv, servlen, "%u", port);
        if (written < 0 || (unsigned)written >= servlen)
            return EAI_OVERFLOW;
    }
    return 0;
}

/* prctl(2) stub — returns ENOSYS; satisfies libbb references. */
int prctl(int option, ...) {
    (void)option;
    errno = ENOSYS;
    return -1;
}

/* sysinfo(2) — Linux-specific; mirrors struct sysinfo from <sys/sysinfo.h>. */
struct sysinfo {
    long           uptime;
    unsigned long  loads[3];
    unsigned long  totalram;
    unsigned long  freeram;
    unsigned long  sharedram;
    unsigned long  bufferram;
    unsigned long  totalswap;
    unsigned long  freeswap;
    unsigned short procs;
    char           _pad[6];
    unsigned long  totalhigh;
    unsigned long  freehigh;
    unsigned int   mem_unit;
    char           _f[0];
};

int sysinfo(struct sysinfo *info) {
    long r = inline_syscall(SYS_sysinfo, (long)info, 0, 0);
    if (r < 0) { errno = -(int)r; return -1; }
    return 0;
}
