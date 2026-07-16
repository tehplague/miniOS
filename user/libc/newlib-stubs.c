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

/* newlib-stubs.c — Retargetable syscall stubs required by Newlib's libc.a
 * that are not yet present in syscalls.c or tty.c.  Newlib calls these
 * _-prefixed entry points internally; without them the final link fails with
 * undefined-reference errors.
 *
 * Stubs already covered by syscalls.c:  _exit  _read  _write  _open  _close
 *   _lseek  _fstat  _stat  _sbrk  _getpid
 * Stubs already covered by tty.c via isatty(): isatty (non-_ form)
 */

#include <sys/times.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/cpuset.h>
#include <sys/types.h>
#include <signal.h>
#include <sched.h>
#include <stddef.h>
#include <glob.h>
#include <fnmatch.h>
#include <pwd.h>
#include <errno.h>

/* Forward declarations for functions defined in syscalls.c / tty.c that
 * the stubs below delegate to. */
int kill(int pid, int sig);
int isatty(int fd);
int unlink(const char *path);

/* signal_trampoline — defined in signal_trampoline.c.
 * Must be registered with the kernel (SYS_register_sigtrampoline = 454)
 * before any signal handler could be invoked. */
extern void signal_trampoline(void);

/* _init / _fini — called by newlib's __libc_init_array() for legacy .init/.fini
 * section support. miniOS has no .init section content; empty stubs satisfy the linker. */
void _init(void) {}
void _fini(void) {}

/**
 * __register_sigtrampoline() - Register the userspace signal trampoline.
 *
 * Called by __libc_init_array (via __attribute__((constructor))) before main().
 * Informs the kernel of the trampoline VA so ring3_invoke_handler() can push it
 * as the return address when invoking signal handlers. This enables SYS_sigreturn
 * to make the SA_RESTART decision when the handler returns.
 *
 * SYS_register_sigtrampoline = 454 (miniOS-specific).
 */
static __attribute__((constructor)) void __register_sigtrampoline(void)
{
    register uint64_t nr __asm__("rax") = 454;   /* SYS_register_sigtrampoline */
    register uint64_t a1 __asm__("rdi") = (uint64_t)(uintptr_t)signal_trampoline;
    __asm__ volatile("syscall" : "+r"(nr) : "r"(a1) : "rcx", "r11", "memory");
}

/* sched_getaffinity — miniOS has no CPU affinity; return ENOSYS. */
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    (void)pid; (void)cpusetsize;
    if (mask) CPU_ZERO(mask);
    errno = ENOSYS;
    return -1;
}

/* sched_setaffinity — likewise. */
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    (void)pid; (void)cpusetsize; (void)mask;
    errno = ENOSYS;
    return -1;
}

/* getuid/geteuid/getgid/getegid — SYS 102/107/104/108 (Linux x86-64) */
uid_t getuid(void)  { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(102L), "D"(0L) : "rcx","r11"); return (uid_t)r; }
uid_t geteuid(void) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(107L), "D"(0L) : "rcx","r11"); return (uid_t)r; }
gid_t getgid(void)  { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(104L), "D"(0L) : "rcx","r11"); return (gid_t)r; }
gid_t getegid(void) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(108L), "D"(0L) : "rcx","r11"); return (gid_t)r; }

/* umask — SYS_umask = 95 */
mode_t umask(mode_t mask) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(95L), "D"((long)mask) : "rcx","r11");
    return (mode_t)r;
}

/* gettimeofday — SYS_gettimeofday = 96 */
int gettimeofday(struct timeval *tv, void *tz) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(96L), "D"((long)tv), "S"((long)tz), "d"(0L)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* sigprocmask — SYS_rt_sigprocmask = 14; sigsetsize = sizeof(sigset_t) = 8 */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    long ret;
    register long r10 __asm__("r10") = 8L;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(14L), "D"((long)how), "S"((long)set), "d"((long)oldset), "r"(r10)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* sigsuspend — SYS_rt_sigsuspend = 130 */
int sigsuspend(const sigset_t *mask) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(130L), "D"((long)mask), "S"(8L), "d"(0L)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* sigfillset / sigemptyset / sigaddset / sigdelset — pure bit manipulation */
int sigfillset(sigset_t *set)  { *set = ~(sigset_t)0; return 0; }
int sigemptyset(sigset_t *set) { *set = (sigset_t)0;  return 0; }
int sigaddset(sigset_t *set, int sig) {
    if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    *set |= (sigset_t)1 << (sig - 1); return 0;
}
int sigdelset(sigset_t *set, int sig) {
    if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    *set &= ~((sigset_t)1 << (sig - 1)); return 0;
}
int sigismember(const sigset_t *set, int sig) {
    if (sig < 1 || sig > 64) { errno = EINVAL; return -1; }
    return !!(*set & ((sigset_t)1 << (sig - 1)));
}

/* sysconf — return sensible constants; ENOSYS for unknowns */
long sysconf(int name) {
    switch (name) {
    case 2:   return 100;       /* _SC_CLK_TCK */
    case 4:   return 4096;      /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 84:  return 4096;      /* _SC_PAGESIZE (glibc value) */
    case 8:   return 256;       /* _SC_OPEN_MAX */
    case 30:  return 255;       /* _SC_NAME_MAX */
    case 73:  return 1;         /* _SC_NPROCESSORS_CONF */
    case 84+1: return 1;        /* _SC_NPROCESSORS_ONLN */
    default:  errno = EINVAL; return -1;
    }
}

/* getpwnam — no user database in miniOS */
struct passwd *getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *getpwuid(uid_t uid)        { (void)uid;  return NULL; }

/* glob / globfree — no filesystem glob in miniOS stub layer */
int glob(const char *pattern, int flags,
         int (*errfunc)(const char *, int), glob_t *pglob) {
    (void)pattern; (void)flags; (void)errfunc;
    pglob->gl_pathc = 0;
    pglob->gl_pathv = NULL;
    return GLOB_NOMATCH;
}
void globfree(glob_t *pglob) { (void)pglob; }

/* fnmatch — simple pattern matching (FNM_PATHNAME not supported) */
int fnmatch(const char *pattern, const char *string, int flags) {
    (void)flags;
    /* Delegate to the recursive matcher below */
    const char *p = pattern, *s = string;
    while (*p && *s) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 0;
            for (; *s; s++)
                if (fnmatch(p, s, 0) == 0) return 0;
            return FNM_NOMATCH;
        } else if (*p == '?' || *p == *s) {
            p++; s++;
        } else {
            return FNM_NOMATCH;
        }
    }
    while (*p == '*') p++;
    return (*p || *s) ? FNM_NOMATCH : 0;
}

/* getrlimit / setrlimit — SYS_getrlimit=97, SYS_setrlimit=160 (Linux x86-64) */
int getrlimit(int resource, struct rlimit *rlim) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(97L), "D"((long)resource), "S"((long)rlim), "d"(0L)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(160L), "D"((long)resource), "S"((long)rlim), "d"(0L)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* clearenv — clear all environment variables. */
extern char **environ;
int clearenv(void) {
    if (environ)
        environ[0] = NULL;
    return 0;
}

/* hstrerror — map h_errno values to strings; used by DNS error reporting. */
const char *hstrerror(int err) {
    switch (err) {
    case 1: return "Unknown host";
    case 2: return "Host name lookup failure";
    case 3: return "Unknown server error";
    case 4: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}

/* _kill — called by newlib abort() and raise() to send a signal to self. */
int _kill(int pid, int sig) {
    return kill(pid, sig);
}

/* _isatty — called by newlib stdio to decide buffering strategy. */
int _isatty(int fd) {
    return isatty(fd);
}

/* _unlink — called by newlib tmpfile() and remove() for unlinking files. */
int _unlink(const char *path) {
    return unlink(path);
}

/* _link — hard links are not implemented in miniOS; return ENOSYS. */
int _link(const char *old, const char *new_path) {
    (void)old;
    (void)new_path;
    errno = ENOSYS;
    return -1;
}

/* times — SYS_times = 100; miniOS doesn't track CPU time, return 0 ticks. */
clock_t times(struct tms *buf) {
    if (buf) {
        buf->tms_utime  = 0;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return 0;
}

/* _times — process CPU time accounting; not tracked by miniOS.
 * Return zeroed tms and indicate ENOSYS so callers can fall back. */
clock_t _times(struct tms *buf) {
    if (buf) {
        buf->tms_utime  = 0;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    errno = ENOSYS;
    return (clock_t)-1;
}

/* lstat — query symlink metadata without following it (SYS_lstat = 6).
 * Duplicated here from syscalls.c to ensure it is always present in
 * libminiOS.a regardless of archive link-order or --gc-sections behaviour. */
int lstat(const char *path, struct stat *st) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(6L), "D"((long)path), "S"((long)st), "d"(0L)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int chmod(const char *path, mode_t mode) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(90ULL), "D"(path), "S"((uint64_t)(uint32_t)mode)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int access(const char *path, int mode) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(21ULL), "D"(path), "S"((uint64_t)(uint32_t)mode)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int rmdir(const char *path) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(84ULL), "D"(path)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int mknod(const char *path, mode_t mode, dev_t dev) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "0"(133ULL), "D"(path), "S"((uint64_t)(uint32_t)mode), "d"((uint64_t)(uint32_t)dev)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* _gettimeofday — newlib internal hook; forward to the real syscall so
 * time(), difftime(), and similar newlib wrappers see monotonic values. */
int _gettimeofday(struct timeval *tv, void *tz) {
    return gettimeofday(tv, tz);
}
