// SPDX-License-Identifier: MIT
// Kernel-private header shared across syscall_*.c compilation units.

#pragma once

#include <miniOS/sched/sched.h>

#define SYSCALL_DISPATCH_UNHANDLED ((int64_t)0x7fffffffffffffffLL)

static inline int syscall_has_pending_signal(void) {
    struct thread *thread = sched_current();
    return thread && thread->pending_signals;
}

int64_t syscall_dispatch(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5, uint64_t arg6);

int64_t syscall_dispatch_mm(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                            uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t syscall_dispatch_fs(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                            uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t syscall_dispatch_proc(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                              uint64_t arg4, uint64_t arg5, uint64_t arg6);
int64_t syscall_dispatch_net(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                             uint64_t arg4, uint64_t arg5, uint64_t arg6);
