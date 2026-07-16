/* miniOS-specific functions not provided by mlibc */

#include <errno.h>
#include <sched.h>
#include <sys/sysinfo.h>

static long _syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "0"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data) {
    long ret;
    register long r10 __asm__("r10") = (long)flags;
    register long r8  __asm__("r8")  = (long)data;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(165L), "D"(source), "S"(target), "d"(fstype), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return (int)ret;
}

int umount(const char *target) {
    return (int)_syscall3(166L, (long)target, 0, 0);
}

int umount2(const char *target, int flags) {
    /* SYS_umount (166) ignores the flags argument — the kernel's umount is a
     * no-op stub (accepts and returns 0 without touching the mount table), so
     * there's no MNT_FORCE/MNT_DETACH behavior to differentiate yet. */
    return (int)_syscall3(166L, (long)target, (long)flags, 0);
}

long ptrace(long request, int pid, void *addr, void *data) {
    long ret;
    register long r10 __asm__("r10") = (long)data;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(101L), "D"(request), "S"((long)pid), "d"((long)addr), "r"(r10)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)-ret; return -1; }
    return ret;
}

int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(204L), "D"((long)pid), "S"(cpusetsize), "d"(mask)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(203L), "D"((long)pid), "S"(cpusetsize), "d"(mask)
        : "rcx", "r11", "memory");
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int sysinfo(struct sysinfo *info) {
    long ret = _syscall3(99L, (long)info, 0, 0);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5) {
    (void)option; (void)arg2; (void)arg3; (void)arg4; (void)arg5;
    errno = ENOSYS;
    return -1;
}
