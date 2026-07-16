#include "mlibc/tcb.hpp"
#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/signal.h>
#include <abi-bits/stat.h>
#include <abi-bits/statvfs.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <bits/syscall.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/fsfd_target.hpp>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

// miniOS syscall numbers — see include/miniOS/syscall.h
#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_stat      4
#define SYS_fstat     5
#define SYS_lstat     6
#define SYS_lseek     8
#define SYS_mmap      9
#define SYS_mprotect 10
#define SYS_munmap   11
#define SYS_ioctl     16
#define SYS_access    21
#define SYS_pipe      22
#define SYS_poll      23
#define SYS_dup       32
#define SYS_dup2      33
#define SYS_nanosleep 35
#define SYS_getpid    39
#define SYS_socket    41
#define SYS_connect   42
#define SYS_accept    43
#define SYS_sendto    44
#define SYS_recvfrom  45
#define SYS_shutdown  48
#define SYS_bind      49
#define SYS_listen    50
#define SYS_socketpair 53
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_fork      57
#define SYS_execve    59
#define SYS_exit      60
#define SYS_wait4     61
#define SYS_kill      62
#define SYS_fcntl     72
#define SYS_getdents  78
#define SYS_getcwd    80
#define SYS_chdir     81
#define SYS_rename    82
#define SYS_mkdir     83
#define SYS_rmdir     84
#define SYS_unlink    87
#define SYS_symlink   88
#define SYS_readlink  89
#define SYS_chmod     90
#define SYS_setpgid   109
#define SYS_getppid   110
#define SYS_setsid    112
#define SYS_mknod     133
#define SYS_statfs    137
#define SYS_fstatfs   138
#define SYS_sigaction 174
#define SYS_clock_gettime 228

namespace mlibc {

// ── Core I/O ──────────────────────────────────────────────────────────────

void Sysdeps<LibcLog>::operator()(const char *msg) {
	ssize_t unused;
	size_t len = 0;
	while (msg[len]) len++;
	sysdep<Write>(2, msg, len, &unused);
}

void Sysdeps<LibcPanic>::operator()() {
	ssize_t unused;
	const char msg[] = "mlibc panic\n";
	sysdep<Write>(2, msg, sizeof(msg) - 1, &unused);
	sysdep<Exit>(-1);
	__builtin_trap();
}

int Sysdeps<Isatty>::operator()(int fd) {
	struct { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; } ws = {};
	long r = __syscall3(SYS_ioctl, fd, 0x5413UL /* TIOCGWINSZ */, (long)&ws);
	if (r == 0) return 0;
	return (r < 0) ? (int)(-r) : 1;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t size, ssize_t *ret) {
	long r = __syscall3(SYS_write, fd, (long)buf, (long)size);
	if (r < 0) return (int)(-r);
	*ret = (ssize_t)r;
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t size, ssize_t *ret) {
	long r = __syscall3(SYS_read, fd, (long)buf, (long)size);
	if (r < 0) return (int)(-r);
	*ret = (ssize_t)r;
	return 0;
}

int Sysdeps<Open>::operator()(const char *path, int flags, mode_t mode, int *fd) {
	long r = __syscall3(SYS_open, (long)path, flags, (long)mode);
	if (r < 0) return (int)(-r);
	*fd = (int)r;
	return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
	if (dirfd == AT_FDCWD) {
		long r = __syscall3(SYS_open, (long)path, flags, (long)mode);
		if (r < 0) return (int)(-r);
		*fd = (int)r;
		return 0;
	}
	return ENOSYS;
}

int Sysdeps<Close>::operator()(int fd) {
	long r = __syscall1(SYS_close, fd);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	long r = __syscall3(SYS_lseek, fd, (long)offset, whence);
	if (r < 0) return (int)(-r);
	*new_offset = (off_t)r;
	return 0;
}

int Sysdeps<Flock>::operator()(int, int) {
	return 0;
}

int Sysdeps<Readv>::operator()(int fd, const struct iovec *iovs, int iovc, ssize_t *bytes_read) {
	ssize_t total = 0;
	for (int i = 0; i < iovc; i++) {
		long r = __syscall3(SYS_read, fd, (long)iovs[i].iov_base, (long)iovs[i].iov_len);
		if (r < 0) {
			if (total > 0) break;
			return (int)(-r);
		}
		total += (ssize_t)r;
		if ((size_t)r < iovs[i].iov_len) break;
	}
	*bytes_read = total;
	return 0;
}

int Sysdeps<Writev>::operator()(int fd, const struct iovec *iovs, int iovc, ssize_t *bytes_written) {
	ssize_t total = 0;
	for (int i = 0; i < iovc; i++) {
		long r = __syscall3(SYS_write, fd, (long)iovs[i].iov_base, (long)iovs[i].iov_len);
		if (r < 0) {
			if (total > 0) break;
			return (int)(-r);
		}
		total += (ssize_t)r;
	}
	*bytes_written = total;
	return 0;
}

// ── Memory ────────────────────────────────────────────────────────────────

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	long r = __syscall6(SYS_mmap, 0, (long)size,
	                    PROT_READ | PROT_WRITE,
	                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((unsigned long)r > (unsigned long)-4096UL) return ENOMEM;
	*pointer = (void *)r;
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	long r = __syscall2(SYS_munmap, (long)pointer, (long)size);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags,
                                int fd, off_t offset, void **window) {
	long r = __syscall6(SYS_mmap, (long)hint, (long)size, prot, flags,
	                    fd, (long)offset);
	if ((unsigned long)r > (unsigned long)-4096UL) return ENOMEM;
	*window = (void *)r;
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	long r = __syscall2(SYS_munmap, (long)pointer, (long)size);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<VmProtect>::operator()(void *pointer, size_t size, int prot) {
	long r = __syscall3(SYS_mprotect, (long)pointer, (long)size, prot);
	if (r < 0) return (int)(-r);
	return 0;
}

// ── TLS / Futex ───────────────────────────────────────────────────────────

int Sysdeps<TcbSet>::operator()(void *pointer) {
	// FSGSBASE (CR4 bit 16) enabled by kernel; set FS.BASE directly.
	// x86-64 TLS variant 2: TP = &Tcb (NOT &Tcb + sizeof(Tcb))
	uintptr_t tp = (uintptr_t)pointer;
	asm volatile("wrfsbase %0" :: "r"(tp) : "memory");
	return 0;
}

pid_t Sysdeps<GetTid>::operator()() {
	return (pid_t)__syscall0(SYS_getpid);
}

int Sysdeps<FutexWake>::operator()(int *, bool) {
	return 0;
}

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *) {
	if (__atomic_load_n(pointer, __ATOMIC_SEQ_CST) == expected)
		return EAGAIN;
	return 0;
}

// ── Time / Sleep ──────────────────────────────────────────────────────────

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	struct { long tv_sec; long tv_nsec; } ts;
	long r = __syscall2(SYS_clock_gettime, clock, (long)&ts);
	if (r < 0) return (int)(-r);
	*secs  = (time_t)ts.tv_sec;
	*nanos = ts.tv_nsec;
	return 0;
}

int Sysdeps<Sleep>::operator()(time_t *secs, long *nanos) {
	struct { long tv_sec; long tv_nsec; } req = { *secs, *nanos }, rem = {};
	long r = __syscall2(SYS_nanosleep, (long)&req, (long)&rem);
	if (r < 0) {
		*secs  = rem.tv_sec;
		*nanos = rem.tv_nsec;
		return (int)(-r);
	}
	return 0;
}

// ── Process ───────────────────────────────────────────────────────────────

void Sysdeps<Exit>::operator()(int status) {
	__syscall1(SYS_exit, status);
	__builtin_unreachable();
}

void Sysdeps<ThreadExit>::operator()() {
	__syscall1(SYS_exit, 0);
	__builtin_unreachable();
}

int Sysdeps<Fork>::operator()(pid_t *child) {
	long r = __syscall0(SYS_fork);
	if (r < 0) return (int)(-r);
	*child = (pid_t)r;
	return 0;
}

int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
	long r = __syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
	return (int)(-r);
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags,
                                  struct rusage *, pid_t *ret_pid) {
	long r = __syscall4(SYS_wait4, (long)pid, (long)status, flags, 0);
	if (r < 0) return (int)(-r);
	*ret_pid = (pid_t)r;
	return 0;
}

pid_t Sysdeps<GetPid>::operator()() {
	return (pid_t)__syscall0(SYS_getpid);
}

pid_t Sysdeps<GetPpid>::operator()() {
	return (pid_t)__syscall1(SYS_getppid, 0);
}

int Sysdeps<GetPgid>::operator()(pid_t, pid_t *pgid) {
	*pgid = (pid_t)__syscall0(SYS_getpid);
	return 0;
}

int Sysdeps<SetPgid>::operator()(pid_t pid, pid_t pgid) {
	long r = __syscall2(SYS_setpgid, (long)pid, (long)pgid);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<SetSid>::operator()(pid_t *sid) {
	long r = __syscall0(SYS_setsid);
	if (r < 0) return (int)(-r);
	*sid = (pid_t)r;
	return 0;
}

uid_t Sysdeps<GetUid>::operator()()  { return 0; }
uid_t Sysdeps<GetEuid>::operator()() { return 0; }
gid_t Sysdeps<GetGid>::operator()()  { return 0; }
gid_t Sysdeps<GetEgid>::operator()() { return 0; }

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	long r = __syscall2(SYS_kill, (long)pid, sig);
	if (r < 0) return (int)(-r);
	return 0;
}

// miniOS kernel sigaction layout (SYS_sigaction = 174):
//   offset 0:  sa_handler (8 bytes)
//   offset 8:  sa_mask    (uint64_t)
//   offset 16: sa_flags   (int)
// Linux struct sigaction (abi-bits/signal.h):
//   __sa_handler (8), sa_flags (8), sa_restorer (8), sa_mask (sigset_t)
// sa_handler is a macro in abi-bits/signal.h; use distinct member names
struct mini_sigaction {
	void (*handler)(int);
	unsigned long mask;
	int flags;
};

#define SA_RESTORER 0x04000000

int Sysdeps<Sigaction>::operator()(int sig, const struct sigaction *__restrict new_act,
                                    struct sigaction *__restrict old_act) {
	mini_sigaction mini_new = {}, mini_old = {};
	mini_sigaction *new_ptr = nullptr;
	mini_sigaction *old_ptr = old_act ? &mini_old : nullptr;

	if (new_act) {
		mini_new.handler = new_act->sa_handler;
		mini_new.mask    = new_act->sa_mask.__sig[0];
		mini_new.flags   = (int)((unsigned long)new_act->sa_flags & ~(unsigned long)SA_RESTORER);
		new_ptr = &mini_new;
	}

	long r = __syscall3(SYS_sigaction, (long)sig, (long)new_ptr, (long)old_ptr);
	if (r < 0) return (int)(-r);

	if (old_act && old_ptr) {
		old_act->sa_handler       = mini_old.handler;
		old_act->sa_flags         = (unsigned long)mini_old.flags;
		old_act->sa_restorer      = nullptr;
		old_act->sa_mask.__sig[0] = mini_old.mask;
	}
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int, const sigset_t *__restrict, sigset_t *__restrict) {
	return 0;
}

int Sysdeps<ThreadSigmask>::operator()(int, const sigset_t *__restrict, sigset_t *__restrict) {
	return 0;
}

// ── Filesystem ────────────────────────────────────────────────────────────

int Sysdeps<Stat>::operator()(mlibc::fsfd_target fsfdt, int fd, const char *path,
                               int flags, struct stat *statbuf) {
	long r;
	switch (fsfdt) {
	case mlibc::fsfd_target::fd:
		r = __syscall2(SYS_fstat, fd, (long)statbuf);
		break;
	case mlibc::fsfd_target::path:
		if (flags & AT_SYMLINK_NOFOLLOW)
			r = __syscall2(SYS_lstat, (long)path, (long)statbuf);
		else
			r = __syscall2(SYS_stat, (long)path, (long)statbuf);
		break;
	case mlibc::fsfd_target::fd_path:
		r = __syscall2(SYS_lstat, (long)path, (long)statbuf);
		break;
	default:
		return EINVAL;
	}
	if (r < 0) return (int)(-r);
	return 0;
}

// ── statvfs / fstatvfs ────────────────────────────────────────────────────

struct minios_statfs {
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
};

static void minios_statfs_to_statvfs(const minios_statfs *from, struct statvfs *to) {
    to->f_bsize   = (unsigned long)from->f_bsize;
    to->f_frsize  = (unsigned long)(from->f_frsize ? from->f_frsize : from->f_bsize);
    to->f_blocks  = from->f_blocks;
    to->f_bfree   = from->f_bfree;
    to->f_bavail  = from->f_bavail;
    to->f_files   = from->f_files;
    to->f_ffree   = from->f_ffree;
    to->f_favail  = from->f_ffree;
    to->f_fsid    = (unsigned long)from->f_fsid[0];
    to->f_flag    = (unsigned long)from->f_flags;
    to->f_namemax = (unsigned long)from->f_namelen;
}

int Sysdeps<Statvfs>::operator()(const char *path, struct statvfs *out) {
    minios_statfs buf = {};
    long r = __syscall2(SYS_statfs, (long)path, (long)&buf);
    if (r < 0) return (int)(-r);
    minios_statfs_to_statvfs(&buf, out);
    return 0;
}

int Sysdeps<Fstatvfs>::operator()(int fd, struct statvfs *out) {
    minios_statfs buf = {};
    long r = __syscall2(SYS_fstatfs, (long)fd, (long)&buf);
    if (r < 0) return (int)(-r);
    minios_statfs_to_statvfs(&buf, out);
    return 0;
}

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	long r = __syscall3(SYS_open, (long)path, O_RDONLY | O_DIRECTORY, 0);
	if (r < 0) return (int)(-r);
	*handle = (int)r;
	return 0;
}

int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size,
                                      size_t *bytes_read) {
	long r = __syscall3(SYS_getdents, handle, (long)buffer, (long)max_size);
	if (r < 0) return (int)(-r);
	*bytes_read = (size_t)r;
	return 0;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
	long r = __syscall2(SYS_mkdir, (long)path, (long)mode);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Mkdirat>::operator()(int dirfd, const char *path, mode_t mode) {
	if (dirfd == AT_FDCWD) {
		long r = __syscall2(SYS_mkdir, (long)path, (long)mode);
		if (r < 0) return (int)(-r);
		return 0;
	}
	return ENOSYS;
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	long r = __syscall1(SYS_rmdir, (long)path);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Unlinkat>::operator()(int, const char *path, int) {
	long r = __syscall1(SYS_unlink, (long)path);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Rename>::operator()(const char *old_path, const char *new_path) {
	long r = __syscall2(SYS_rename, (long)old_path, (long)new_path);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Readlink>::operator()(const char *path, void *buffer, size_t max_size,
                                   ssize_t *length) {
	long r = __syscall3(SYS_readlink, (long)path, (long)buffer, (long)max_size);
	if (r < 0) return (int)(-r);
	*length = (ssize_t)r;
	return 0;
}

int Sysdeps<Symlink>::operator()(const char *target_path, const char *link_path) {
	long r = __syscall2(SYS_symlink, (long)target_path, (long)link_path);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Access>::operator()(const char *path, int mode) {
	long r = __syscall2(SYS_access, (long)path, mode);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Chmod>::operator()(const char *pathname, mode_t mode) {
	long r = __syscall2(SYS_chmod, (long)pathname, (long)mode);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Fchmod>::operator()(int, mode_t)                 { return ENOSYS; }
int Sysdeps<Fchmodat>::operator()(int, const char *, mode_t, int) { return ENOSYS; }

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	long r = __syscall2(SYS_getcwd, (long)buffer, (long)size);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Chdir>::operator()(const char *path) {
	long r = __syscall1(SYS_chdir, (long)path);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	long arg = va_arg(args, long);
	long r = __syscall3(SYS_fcntl, fd, request, arg);
	if (r < 0) return (int)(-r);
	*result = (int)r;
	return 0;
}

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void *arg, int *result) {
	long r = __syscall3(SYS_ioctl, fd, (long)request, (long)arg);
	if (r < 0) return (int)(-r);
	*result = (int)r;
	return 0;
}

int Sysdeps<Mknodat>::operator()(int dirfd, const char *path, int mode, int dev) {
	if (dirfd == AT_FDCWD) {
		long r = __syscall3(SYS_mknod, (long)path, (long)mode, (long)dev);
		if (r < 0) return (int)(-r);
		return 0;
	}
	return ENOSYS;
}

// ── Pipe / Dup ────────────────────────────────────────────────────────────

int Sysdeps<Pipe>::operator()(int *fds, int flags) {
	long r = __syscall1(SYS_pipe, (long)fds);
	if (r < 0) return (int)(-r);
	if (flags & O_CLOEXEC) {
		__syscall3(SYS_fcntl, fds[0], F_SETFD, FD_CLOEXEC);
		__syscall3(SYS_fcntl, fds[1], F_SETFD, FD_CLOEXEC);
	}
	return 0;
}

int Sysdeps<Dup>::operator()(int fd, int flags, int *newfd) {
	long r = __syscall1(SYS_dup, fd);
	if (r < 0) return (int)(-r);
	*newfd = (int)r;
	if (flags & O_CLOEXEC)
		__syscall3(SYS_fcntl, *newfd, F_SETFD, FD_CLOEXEC);
	return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int flags, int newfd) {
	long r = __syscall2(SYS_dup2, fd, newfd);
	if (r < 0) return (int)(-r);
	if (flags & O_CLOEXEC)
		__syscall3(SYS_fcntl, newfd, F_SETFD, FD_CLOEXEC);
	return 0;
}

// ── Poll ─────────────────────────────────────────────────────────────────

int Sysdeps<Poll>::operator()(struct pollfd *fds, nfds_t count, int timeout,
                               int *num_events) {
	long r = __syscall3(SYS_poll, (long)fds, (long)count, timeout);
	if (r < 0) return (int)(-r);
	*num_events = (int)r;
	return 0;
}

// ── Networking ────────────────────────────────────────────────────────────

int Sysdeps<Socket>::operator()(int family, int type, int protocol, int *fd) {
	long r = __syscall3(SYS_socket, family, type, protocol);
	if (r < 0) return (int)(-r);
	*fd = (int)r;
	return 0;
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr *addr, socklen_t len) {
	long r = __syscall3(SYS_bind, fd, (long)addr, (long)len);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Listen>::operator()(int fd, int backlog) {
	long r = __syscall2(SYS_listen, fd, backlog);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Accept>::operator()(int fd, int *newfd, struct sockaddr *addr,
                                 socklen_t *addr_len, int) {
	long r = __syscall3(SYS_accept, fd, (long)addr, (long)addr_len);
	if (r < 0) return (int)(-r);
	*newfd = (int)r;
	return 0;
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr *addr, socklen_t len) {
	long r = __syscall3(SYS_connect, fd, (long)addr, (long)len);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Sendto>::operator()(int fd, const void *buf, size_t size, int flags,
                                 const struct sockaddr *addr, socklen_t addr_len,
                                 ssize_t *length) {
	long r = __syscall6(SYS_sendto, fd, (long)buf, (long)size, flags,
	                    (long)addr, (long)addr_len);
	if (r < 0) return (int)(-r);
	*length = (ssize_t)r;
	return 0;
}

int Sysdeps<Recvfrom>::operator()(int fd, void *buf, size_t size, int flags,
                                   struct sockaddr *addr, socklen_t *addr_len,
                                   ssize_t *length) {
	long r = __syscall6(SYS_recvfrom, fd, (long)buf, (long)size, flags,
	                    (long)addr, (long)addr_len);
	if (r < 0) return (int)(-r);
	*length = (ssize_t)r;
	return 0;
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number,
                                     void *__restrict buf, socklen_t *__restrict size) {
	long r = __syscall5(SYS_getsockopt, fd, layer, number, (long)buf, (long)size);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<SetSockopt>::operator()(int fd, int layer, int number,
                                     const void *buf, socklen_t size) {
	long r = __syscall5(SYS_setsockopt, fd, layer, number, (long)buf, (long)size);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Shutdown>::operator()(int sockfd, int how) {
	long r = __syscall2(SYS_shutdown, sockfd, how);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Socketpair>::operator()(int domain, int type_and_flags, int proto, int *fds) {
	long r = __syscall4(SYS_socketpair, domain, type_and_flags, proto, (long)fds);
	if (r < 0) return (int)(-r);
	return 0;
}

// ── Terminal ──────────────────────────────────────────────────────────────

int Sysdeps<Tcgetattr>::operator()(int fd, struct termios *attr) {
	long r = __syscall3(SYS_ioctl, fd, 0x5401UL /* TCGETS */, (long)attr);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Tcsetattr>::operator()(int fd, int, const struct termios *attr) {
	long r = __syscall3(SYS_ioctl, fd, 0x5402UL /* TCSETS */, (long)attr);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Tcgetwinsize>::operator()(int fd, struct winsize *winsz) {
	long r = __syscall3(SYS_ioctl, fd, 0x5413UL /* TIOCGWINSZ */, (long)winsz);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<Tcsetwinsize>::operator()(int fd, const struct winsize *winsz) {
	(void)fd; (void)winsz;
	return 0;
}

// ── Timers ────────────────────────────────────────────────────────────────

int Sysdeps<SetItimer>::operator()(int which, const struct itimerval *new_value,
                                    struct itimerval *old_value) {
	long r = __syscall3(452 /* SYS_setitimer_miniOS */, (long)which,
	                    (long)new_value, (long)old_value);
	if (r < 0) return (int)(-r);
	return 0;
}

int Sysdeps<GetItimer>::operator()(int, struct itimerval *) {
	return ENOSYS;
}

} // namespace mlibc

#include <errno.h>
#include <stdarg.h>

extern "C" int ioctl(int fd, unsigned long request, ...) {
	va_list args;
	va_start(args, request);
	int result;
	void *arg = va_arg(args, void *);
	va_end(args);
	if (int e = mlibc::sysdep_or_enosys<Ioctl>(fd, request, arg, &result); e) {
		errno = e;
		return -1;
	}
	return result;
}
