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

#include <miniOS/io.h>
#include <miniOS/signal.h>
#include <miniOS/sched/sched.h>
#include <miniOS/mm/vmm.h>

extern void proc_close_all_sockets(struct thread *t);

static int signal_default_terminates(int sig)
{
    return sig != SIGCHLD;
}

void signal_dispatch(void)
{
    struct thread *cur = sched_current();

    for (int sig = 1; sig <= 31; sig++) {
        if (!(cur->pending_signals & (1u << sig)))
            continue;

        /* Clear the pending bit before any dispatch so re-entrant delivery
         * (e.g. handler calls another syscall) does not re-deliver. */
        cur->pending_signals &= ~(1u << sig);

        /* SIGKILL is always fatal and never consults the action table. */
        if (sig == SIGKILL) {
            proc_close_all_sockets(cur);
            sched_exit_current(128 + sig);
#ifndef TEST_BUILD
            __builtin_unreachable();
#else
            return;
#endif
        }

        mini_sigaction_t *action = &cur->signal_actions[sig];
        sighandler_t handler = action->sa_handler;

        /* SIGSTOP always stops (cannot be caught or ignored, per POSIX).
         * SIGTSTP stops only on the default disposition — a registered
         * handler or SIG_IGN falls through to the normal handling below. */
        if (sig == SIGSTOP || (sig == SIGTSTP && handler == SIG_DFL)) {
            sched_stop_current(sig);
            while (cur->state == THREAD_STOPPED)
                sched_yield();
            continue;
        }

        if (handler == SIG_DFL) {
            if (signal_default_terminates(sig)) {
                printk("signal_dispatch: default terminate tid=%u sig=%d\n", cur->tid, sig);
                proc_close_all_sockets(cur);
                sched_exit_current(128 + sig);
#ifndef TEST_BUILD
                __builtin_unreachable();
#else
                return;
#endif
            }
            continue;
        }
        if (handler == SIG_IGN) {
            continue;
        }

        /* Registered handler: invoke via ring-3 trampoline */
        ring3_invoke_handler(sig, handler);
    }
}

void ring3_invoke_handler(int sig, sighandler_t handler)
{
    struct thread *cur = sched_current();

    /* Record which signal is being dispatched for SYS_sigreturn SA_RESTART lookup */
    cur->syscall_last_signal = (uint8_t)sig;

    /* Snapshot pre-handler state so SYS_sigreturn can restore correctly even if
     * the handler makes syscalls (which overwrite syscall_restart_*). */
    cur->signal_ctx_rip              = cur->saved_user_rip;
    cur->signal_ctx_rsp              = cur->saved_user_rsp;
    cur->signal_ctx_restart_rip      = cur->syscall_restart_saved_rip;
    cur->signal_ctx_restart_pending  = cur->syscall_restart_pending;
    cur->signal_ctx_restart_nr       = cur->syscall_restart_nr;
    for (int i = 0; i < 6; i++)
        cur->signal_ctx_restart_args[i] = cur->syscall_restart_args[i];

#ifndef TEST_BUILD
    /* Determine return address: trampoline (for SYS_sigreturn) or original RIP */
    uint64_t return_addr;
    if (cur->signal_trampoline_va != 0) {
        return_addr = cur->signal_trampoline_va;
    } else {
        /* No trampoline registered — fall back to original behavior:
         * handler returns directly to interrupted code (no restart possible) */
        return_addr = cur->saved_user_rip;
    }

    /* Signal handler must be entered with RSP ≡ 8 (mod 16), as-if the handler
     * was reached via a `call` instruction from a 16-byte-aligned stack.
     * Round down to 16-byte boundary then subtract 8 for the return-address slot.
     * This guarantees aligned SSE/AVX locals (-0x40(%rbp) etc.) inside the handler. */
    cur->saved_user_rsp = (cur->saved_user_rsp & ~0xFULL) - 8;
    uint64_t rsp = cur->saved_user_rsp;
    /* CR-01: validate rsp is in userspace before walking page tables */
    if (rsp >= KERNEL_VMA || rsp < 8) {
        sched_exit_current(11 /* SIGSEGV */);
#ifndef TEST_BUILD
        __builtin_unreachable();
#else
        return;
#endif
    }
    uint64_t page_pa = vmm_virt_to_phys(rsp & ~0xFFFULL);
    if (page_pa == 0) {
        sched_exit_current(11 /* SIGSEGV */);
#ifndef TEST_BUILD
        __builtin_unreachable();
#else
        return;
#endif
    }
    uint64_t phys = page_pa + (rsp & 0xFFF);
    *(uint64_t *)(phys + KERNEL_VMA) = return_addr;

    /* Redirect SYSRET to jump to the handler */
    cur->saved_user_rip = (uint64_t)handler;

    /* Signal number is the first argument (RDI) per SysV x86-64 ABI.
     * syscall.asm restores saved_user_rdi into RDI before SYSRET, so writing
     * the signal number here ensures every handler receives the correct signum
     * regardless of what RDI held at syscall entry. */
    cur->saved_user_rdi = (uint64_t)sig;
#else
    /* In TEST_BUILD, the vmm page-walk is not available.
     * Record the handler pointer so signal dispatch tests can confirm delivery. */
    cur->saved_user_rip = (uint64_t)(uintptr_t)handler;
    cur->saved_user_rdi = (uint64_t)sig;
#endif
}
