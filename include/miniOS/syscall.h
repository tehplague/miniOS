#ifndef _MINIOS_SYSCALL_H_
#define _MINIOS_SYSCALL_H_

/**
 * @file syscall.h
 * @defgroup syscall Syscall Interface
 * @brief Linux-compatible syscall numbers and kernel syscall initialisation.
 *
 * Defines SYS_* constants matching the Linux x86-64 ABI syscall table.
 * The syscall entry point (syscall.asm) dispatches on RAX to the kernel
 * syscall dispatcher in syscall.c, which fans out to four independent
 * compilation units: syscall_mm.c (memory, brk, mmap, gettimeofday,
 * clock_gettime), syscall_fs.c (VFS, poll, select, pipe, dup, ioctl,
 * getdents, fcntl), syscall_proc.c (fork, execve, exit, wait, signals),
 * and syscall_net.c (socket, bind, connect, sendto, recvfrom, accept).
 * Supports 45+ syscalls covering file I/O, process management, memory
 * management, networking, signals, and terminal control.
 * @{
 */

#include <miniOS/types.h>

/* Syscall numbers */
#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_stat    4
#define SYS_fstat   5
#define SYS_lstat   6
#define SYS_waitpid 7 /* wait4 is a superset; this is for waitpid() */
#define SYS_lseek   8
#define SYS_mmap      9
#define SYS_mprotect 10
#define SYS_munmap   11
#define SYS_signal  13
#define SYS_sigreturn  15   /* return from signal handler; SA_RESTART decision here */
#define SYS_ioctl   16
#define SYS_pread64 17
#define SYS_pwrite64 18
#define SYS_access  21
#define SYS_pipe    22
#define SYS_poll    23
#define SYS_select  24
#define SYS_dup     32
#define SYS_dup2    33
#define SYS_nanosleep 35
#define SYS_getpid  39
#define SYS_fork    57
#define SYS_execve  59
#define SYS_exit    60
#define SYS_wait4   61 /* waitpid is implemented via this */
#define SYS_kill    62
#define SYS_gettimeofday 96
#define SYS_sysinfo      99
#define SYS_chmod   90
#define SYS_statfs  137
#define SYS_fstatfs 138
#define SYS_mknod   133
#define SYS_fcntl   72
#define SYS_ftruncate 77
#define SYS_getdents 78
#define SYS_brk     79
#define SYS_getcwd  80
#define SYS_chdir   81
#define SYS_rename  82
#define SYS_mkdir   83
#define SYS_rmdir   84
#define SYS_unlink  87
#define SYS_symlink 88
#define SYS_readlink 89
#define SYS_ptrace  101
#define SYS_setpgid 109
#define SYS_getppid 110
#define SYS_setsid  112
#define SYS_mount   165
#define SYS_umount  166
#define SYS_sigaction 174
#define SYS_sched_getcpu 318
#define SYS_clock_gettime 228

/* ── Socket syscalls (Linux x86-64 ABI, nr 41-55) ─────────────────────── */
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
#define SYS_clone      56
#define SYS_arch_prctl 158
#define SYS_gettid     186
#define SYS_futex      202
#define SYS_set_tid_address 218

/* ── miniOS-specific syscall numbers (beyond Linux x86-64 ABI range) ───── */
#define SYS_setitimer_miniOS       452   /* miniOS setitimer(): set ITIMER_REAL precisely */
#define SYS_alarm_miniOS           453   /* miniOS alarm(): set ITIMER_REAL in seconds */
#define SYS_register_sigtrampoline 454   /* register userspace signal trampoline VA */

/**
 * syscall_save_restart_state() - Save syscall nr + args for SA_RESTART replay.
 * @nr, @a1..@a6: Syscall number and all 6 arguments at syscall entry.
 *
 * Called as the FIRST action in syscall_dispatch(). Records the syscall state
 * in the current thread so SYS_sigreturn can replay it if SA_RESTART applies.
 * Clears syscall_restart_pending (set by blocking syscalls that were interrupted).
 */
void syscall_save_restart_state(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6);

/**
 * syscall_init() - Initialise the SYSCALL/SYSRET mechanism.
 *
 * @brief Writes IA32_LSTAR MSR to the address of syscall_entry (the NASM entry stub
 * in syscall.asm), writes IA32_STAR MSR with kernel CS (0x08) and user CS
 * (0x1B) selectors, writes IA32_FMASK to mask IF during syscall entry
 * (interrupts disabled at syscall boundary), and sets SCE bit in IA32_EFER.
 * Called once by the BSP during boot.
 */
void syscall_init(void);

/**
 * syscall_get_saved_user_rdi_rsi_rdx() - Retrieve user-mode argument registers saved at syscall entry.
 * @param out_rdi Output pointer; set to the user's RDI value at SYSCALL entry.
 * @param out_rsi Output pointer; set to the user's RSI value at SYSCALL entry.
 * @param out_rdx Output pointer; set to the user's RDX value at SYSCALL entry.
 *
 * @brief The SYSCALL instruction clobbers RCX and R11; arguments in RDI/RSI/RDX are
 * preserved by the syscall.asm stub in per-CPU scratch space. This helper
 * exposes them to kernel C code (e.g., SYS_execve handler) that needs all
 * three argument registers.
 */
void syscall_get_saved_user_rdi_rsi_rdx(uint64_t *out_rdi, uint64_t *out_rsi, uint64_t *out_rdx);

/** @} */

#endif // _MINIOS_SYSCALL_H_
