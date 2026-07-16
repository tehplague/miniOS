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

#ifndef _MINIOS_SIGNAL_H_
#define _MINIOS_SIGNAL_H_

#include <miniOS/types.h>

/* Signal numbers (POSIX / Linux x86-64 ABI) */
#define SIGHUP   1    /* hangup */
#define SIGINT   2    /* interrupt (Ctrl-C) */
#define SIGQUIT  3    /* quit (Ctrl-\) */
#define SIGILL   4    /* illegal instruction */
#define SIGABRT  6    /* abort */
#define SIGTRAP  5    /* breakpoint/single-step trap (ptrace) */
#define SIGFPE   8    /* floating-point exception */
#define SIGKILL  9    /* unconditional termination; cannot be caught or ignored */
#define SIGSEGV  11   /* segmentation fault */
#define SIGPIPE  13   /* broken pipe */
#define SIGALRM  14   /* alarm clock (ITIMER_REAL expiry) */
#define SIGTERM  15   /* termination request; default action terminates process */
#define SIGCHLD  17   /* sent to parent when a child exits or stops (Linux ABI) */
#define SIGCONT  18   /* continue if stopped */
#define SIGSTOP  19   /* stop (cannot be caught or ignored) */
#define SIGTSTP  20   /* terminal stop (Ctrl-Z) */

/* Signal handler type */
typedef void (*sighandler_t)(int signum);

/* Special handler values */
#define SIG_DFL  ((sighandler_t)0)   /* default signal action */
#define SIG_IGN  ((sighandler_t)1)   /* ignore signal */

/* ── Signal action flags (SA_* — Linux x86-64 ABI) ─────────────────────── */
#define MINIOS_SA_NOCLDWAIT 0x00000002U   /* do not produce zombies; skip SIGCHLD delivery */
#define MINIOS_SA_RESTART   0x10000000U   /* restart interrupted syscall on signal return */
#define MINIOS_SA_RESETHAND 0x80000000U   /* reset to SIG_DFL on delivery */

/* Signal mask type (bitmask of signals 1-31 in a uint32_t) */
typedef uint32_t minisigset_t;

/**
 * mini_sigaction_t — per-signal disposition record.
 *
 * Stored in struct thread::signal_actions[1..31].
 * sa_flags holds MINIOS_SA_RESTART and other SA_* flags.
 * sa_mask is reserved (zero) — miniOS does not block signals during handlers yet.
 */
typedef struct mini_sigaction {
    sighandler_t sa_handler;   /* SIG_DFL, SIG_IGN, or user handler pointer */
    int          sa_flags;     /* MINIOS_SA_RESTART, MINIOS_SA_RESETHAND, ... */
    minisigset_t sa_mask;      /* signals to block during handler (reserved, zero) */
} mini_sigaction_t;

/**
 * signal_dispatch() - Deliver all pending signals before returning to userspace.
 *
 * Called from syscall.asm immediately before SYSRET. Scans the current
 * thread's pending_signals bitmask (bits 1-31). For SIGKILL and SIGTERM
 * calls sched_exit_current(128 + signum). For other signals invokes the
 * registered handler via ring3_invoke_handler() if a non-default handler
 * is registered, otherwise clears the pending bit and continues.
 *
 * Context: Called with interrupts disabled (CLI before SYSRET path).
 * Does not return if SIGKILL or SIGTERM is pending.
 */
void signal_dispatch(void);

/**
 * ring3_invoke_handler() - Set up user stack to invoke a signal handler.
 * @sig: Signal number passed as first argument to handler (RDI per SysV ABI).
 * @handler: User-mode function pointer to invoke.
 *
 * Modifies the current thread's saved_user_rip to point to @handler and
 * adjusts saved_user_rsp to push the signal number onto the user stack.
 * SYSRET then jumps directly into the handler. Full trampoline (return path
 * via SYS_sigreturn).
 */
void ring3_invoke_handler(int sig, sighandler_t handler);

#endif /* _MINIOS_SIGNAL_H_ */
