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
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/io.h>
#include <miniOS/types.h>
#include <stddef.h>
#include "syscall_internal.h"

/* -----------------------------------------------------------------------
 * MSR addresses for SYSCALL/SYSRET support
 * ----------------------------------------------------------------------- */
#define IA32_EFER   0xC0000080UL
#define IA32_STAR   0xC0000081UL
#define IA32_LSTAR  0xC0000082UL
#define IA32_SFMASK 0xC0000084UL

#define SYSCALL_DISPATCH_UNHANDLED ((int64_t)0x7fffffffffffffffLL)

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Assembly entry points (syscall.asm) */
extern void syscall_entry(void);
extern void int80_entry(void);

/* Guard: fork_child_trampoline.asm has hardcoded offsets into struct thread.
 * If struct thread changes these must be updated in syscall.asm too. */
_Static_assert(offsetof(struct thread, saved_user_rsp) == 0xF8,
    "struct thread layout changed — update fork_child_trampoline offsets in syscall.asm");
_Static_assert(offsetof(struct thread, saved_user_rip) == 0x100,
    "struct thread layout changed — update fork_child_trampoline offsets in syscall.asm");
_Static_assert(offsetof(struct thread, saved_user_rfl) == 0x108,
    "struct thread layout changed — update fork_child_trampoline offsets in syscall.asm");

/* fork_child_trampoline() is implemented in pure asm in syscall.asm.
 * The C inline-asm version had a GCC register clobber bug: with only "memory"
 * in the clobber list, GCC assigned r11 as the holding register for the
 * user_r14 input; the asm wrote user_rfl into r11 first, leaving the child's
 * r14 = RFLAGS = 0x246 instead of the saved path pointer. The asm version
 * loads each register directly from the thread struct with no compiler
 * involvement, avoiding the aliasing hazard entirely. */

/* Kernel stack pointer for SYSCALL path — updated before entering ring 3.
 * Lives in syscall.asm (.data section) so the stub can access it via RIP-rel. */
extern uint64_t syscall_kernel_rsp_storage;

void syscall_save_user_regs(uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp) {
    struct thread *t  = sched_current();
    t->saved_user_rip = user_rip;
    t->saved_user_rfl = user_rflags;
    t->saved_user_rsp = user_rsp;
}

void syscall_save_user_callee_regs(uint64_t user_rbp, uint64_t user_rbx,
                                   uint64_t user_r12, uint64_t user_r13,
                                   uint64_t user_r14, uint64_t user_r15) {
    struct thread *t = sched_current();
    t->saved_user_rbp = user_rbp;
    t->saved_user_rbx = user_rbx;
    t->saved_user_r12 = user_r12;
    t->saved_user_r13 = user_r13;
    t->saved_user_r14 = user_r14;
    t->saved_user_r15 = user_r15;
}

void syscall_get_saved_user_callee_regs(uint64_t *out_rbp, uint64_t *out_rbx,
                                        uint64_t *out_r12, uint64_t *out_r13,
                                        uint64_t *out_r14, uint64_t *out_r15) {
    struct thread *t = sched_current();
    *out_rbp = t->saved_user_rbp;
    *out_rbx = t->saved_user_rbx;
    *out_r12 = t->saved_user_r12;
    *out_r13 = t->saved_user_r13;
    *out_r14 = t->saved_user_r14;
    *out_r15 = t->saved_user_r15;
}

void syscall_get_saved_user_ctx(uint64_t *out_rip, uint64_t *out_rfl,
                                uint64_t *out_rsp) {
    struct thread *t = sched_current();
    *out_rip = t->saved_user_rip;
    *out_rfl = t->saved_user_rfl;
    *out_rsp = t->saved_user_rsp;
}

void syscall_get_saved_user_rdi_rsi_rdx(uint64_t *out_rdi, uint64_t *out_rsi,
                                        uint64_t *out_rdx) {
    struct thread *t = sched_current();
    *out_rdi = t->saved_user_rdi;
    *out_rsi = t->saved_user_rsi;
    *out_rdx = t->saved_user_rdx;
    /* One-shot delivery: clear after reading so subsequent syscall returns
     * do not clobber the actual user registers with stale execve args. */
    t->saved_user_rdi = 0;
    t->saved_user_rsi = 0;
    t->saved_user_rdx = 0;
}

void syscall_save_restart_state(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct thread *t = sched_current();
    t->syscall_restart_nr = nr;
    t->syscall_restart_args[0] = a1;
    t->syscall_restart_args[1] = a2;
    t->syscall_restart_args[2] = a3;
    t->syscall_restart_args[3] = a4;
    t->syscall_restart_args[4] = a5;
    t->syscall_restart_args[5] = a6;
    t->syscall_restart_saved_rip = t->saved_user_rip;
    t->syscall_restart_pending = 0;
}

void syscall_init(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | 1ULL);

    uint64_t star = ((uint64_t)0x0013 << 48) | ((uint64_t)0x0008 << 32);
    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(IA32_SFMASK, (1UL << 9));
    idt_set_handler(0x80, (addr_t)(uintptr_t)int80_entry, 3, IDT_INTERRUPT_GATE);

    printk("Syscall: SYSCALL/SYSRET + INT 0x80 initialised\n");
}

/**
 * syscall_dispatch() - Dispatch a syscall number to grouped subsystem handlers.
 *
 * `src/kernel/syscall.c` owns the stable public entrypoints and delegates the
 * large implementation bodies to subsystem-specific pieces:
 * - `syscall_mm.inc`   : brk/mmap/munmap/clock_gettime
 * - `syscall_fs.inc`   : VFS, pipes, polling, stat and mount operations
 * - `syscall_proc.inc` : fork/exec/wait/signal/session/process helpers
 * - `syscall_net.inc`  : socket syscalls and lwIP callbacks
 */
int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    /* Save syscall state for SA_RESTART replay in SYS_sigreturn.
     * Skip sigreturn itself: it must preserve the interrupted syscall's
     * saved_rip and restart args so sigreturn can restore the correct RIP
     * (or re-execute the interrupted syscall for SA_RESTART). */
    if (nr != SYS_sigreturn)
        syscall_save_restart_state(nr, arg1, arg2, arg3, arg4, arg5, arg6);

    /* PTRACE_SYSCALL entry/exit stops. Never trace SYS_ptrace itself — a
     * tracer stopping on its own PTRACE_CONT/PTRACE_SYSCALL call would
     * deadlock (it can't also be waiting for itself to stop). */
    struct thread *cur = sched_current();
    int trace_this = cur->ptrace_traced && cur->ptrace_trace_syscalls &&
                      nr != (uint64_t)SYS_ptrace;
    if (trace_this) {
        cur->ptrace_syscall_nr = nr;
        sched_ptrace_stop(PTRACE_STOP_SYSCALL_ENTRY);
    }

    int64_t ret;

    ret = syscall_dispatch_mm(nr, arg1, arg2, arg3, arg4, arg5, arg6);
    if (ret == SYSCALL_DISPATCH_UNHANDLED)
        ret = syscall_dispatch_fs(nr, arg1, arg2, arg3, arg4, arg5, arg6);
    if (ret == SYSCALL_DISPATCH_UNHANDLED)
        ret = syscall_dispatch_proc(nr, arg1, arg2, arg3, arg4, arg5, arg6);
    if (ret == SYSCALL_DISPATCH_UNHANDLED)
        ret = syscall_dispatch_net(nr, arg1, arg2, arg3, arg4, arg5, arg6);
    if (ret == SYSCALL_DISPATCH_UNHANDLED)
        ret = -38;  /* ENOSYS */

    if (trace_this) {
        cur->ptrace_syscall_ret = ret;
        sched_ptrace_stop(PTRACE_STOP_SYSCALL_EXIT);
    }

    return ret;
}
