/* sys/ptrace.h — mlibc doesn't install this header. ptrace() itself is a
 * miniOS-specific syscall wrapper in user/libc/miniOS_compat.c.
 *
 * Scope: PTRACE_TRACEME only (no PTRACE_ATTACH to an already-running
 * unrelated process). Request numbers are the real Linux x86-64 values.
 *
 * PTRACE_PEEK* callers should always pass a non-NULL @data pointer to
 * receive the word (safer than relying on the return value — this simple
 * wrapper doesn't implement glibc's errno-clearing trick to distinguish a
 * legitimate negative-looking word from a real -1 error). */
#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PTRACE_TRACEME    0
#define PTRACE_PEEKTEXT   1
#define PTRACE_PEEKDATA   2
#define PTRACE_POKETEXT   4
#define PTRACE_POKEDATA   5
#define PTRACE_CONT       7
#define PTRACE_KILL       8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS    12
#define PTRACE_SETREGS    13
#define PTRACE_SYSCALL    24

/* Linux x86-64 struct user_regs_struct layout. miniOS only ever populates a
 * subset (see the kernel-side comment in syscall_proc.c); the rest read as
 * 0. Field order/sizes match the real ABI so this stays a drop-in shape. */
struct user_regs_struct {
    unsigned long long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    unsigned long long rax, rcx, rdx, rsi, rdi, orig_rax;
    unsigned long long rip, cs, eflags, rsp, ss;
    unsigned long long fs_base, gs_base, ds, es, fs, gs;
};

long ptrace(long request, int pid, void *addr, void *data);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PTRACE_H */
