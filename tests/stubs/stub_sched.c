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

/* stub_sched.c — fake scheduler for host-native unit tests.
 * Provides sched_current() returning a static struct thread with fd_table. */

/* Pull in the real sched.h so struct thread layout matches exactly.
 * context.h references only miniOS/types.h (handled by stub_types.h force-include).
 * vfs.h (included via sched.h) references ext2.h (only miniOS/types.h). */
#include <miniOS/sched/sched.h>

/* -----------------------------------------------------------------------
 * Thread pool export (referenced by sched.h extern declaration)
 * --------------------------------------------------------------------- */
struct thread thread_pool[SCHED_MAX_THREADS];

/* -----------------------------------------------------------------------
 * The single test thread: a zero-initialized global with a static fd_table.
 * setUp() in each test file should zero fd_table before each test.
 * --------------------------------------------------------------------- */
static vfs_file_t test_fd_table[VFS_MAX_FDS];
static struct thread test_thread = { .fd_table = test_fd_table };

struct thread *sched_current(void) {
    return &test_thread;
}

struct thread *sched_next(void) {
    return &test_thread;
}

/* sched_fork: returns a pointer to a second static thread (simulates child) */
static vfs_file_t fork_child_fd_table[VFS_MAX_FDS];
static struct thread fork_child_thread = { .fd_table = fork_child_fd_table };
int stub_sched_exit_called = 0;  /* tests can inspect this flag */
int stub_sched_wait_exit_code = 0;  /* tests set this to control returned exit_code */
int stub_sched_wait_retval    = 0;  /* tests set to -1 to simulate wait failure */

struct thread *sched_fork(void) {
    fork_child_thread = test_thread;
    fork_child_thread.tid  = 99;
    fork_child_thread.pid  = 99;                /* stub child PID = TID */
    fork_child_thread.ppid = test_thread.pid;   /* parent is test_thread */
    fork_child_thread.pgid = test_thread.pgid;  /* inherit group */
    fork_child_thread.sid  = test_thread.sid;   /* inherit session */
    return &fork_child_thread;
}

/* sched_wait: configurable via stub_sched_wait_exit_code / stub_sched_wait_retval */
int sched_wait(uint32_t child_tid, int *exit_code) {
    (void)child_tid;
    if (exit_code) *exit_code = stub_sched_wait_exit_code;
    return stub_sched_wait_retval;
}

/* sched_exit_current: set flag so tests can detect the call; do NOT
 * call __builtin_unreachable — this is a test host build. */
void sched_exit_current(int code) {
    (void)code;
    stub_sched_exit_called = 1;
    /* Return so the caller (syscall_dispatch SYS_exit path) continues.
     * syscall.c follows this with __builtin_unreachable(); the test
     * should NOT call SYS_exit via the normal path — call it through
     * the dispatch wrapper below, or skip the test. */
}

void sched_init(void) { /* no-op */ }
void sched_tick(void)  { /* no-op */ }
void sched_yield(void) { /* no-op */ }
void sched_activate_task(struct thread *t) { (void)t; /* no-op */ }

struct thread *sched_create_thread(thread_func_t func, void *arg) {
    (void)func; (void)arg;
    return NULL;
}

struct thread *sched_create_user_task(uint64_t entry_rip) {
    (void)entry_rip;
    return NULL;
}

/* context_switch_asm: declared in sched.h; needed by any test that pulls in sched.h.
 * Never actually called in unit tests (no scheduler preemption on host). */
void context_switch_asm(struct task_cpu_context *old_ctx, struct task_cpu_context *new_ctx) {
    (void)old_ctx; (void)new_ctx;
}

/* lapic_tick_count: LAPIC timer counter used by lwip_port.c for timing. */
volatile uint64_t lapic_tick_count = 0;

/* lwip_netif_poll: project-local poller (lwip_netif.c); no-op in test builds. */
void lwip_netif_poll(void) {}

volatile uint64_t sched_tick_count = 0;

struct thread *sched_clone(uint64_t child_stack, uint64_t tls, uint32_t *child_tid) {
    (void)child_stack; (void)tls; (void)child_tid;
    return NULL;
}

void sched_signal_thread(struct thread *t, int sig) {
    if (!t || sig <= 0 || sig >= 32)
        return;
    if (sig == SIGCONT && t->state == THREAD_STOPPED) {
        t->state = THREAD_RUNNABLE;
        return;
    }
    t->pending_signals |= (1u << sig);
}

/* sched_stop_current: no thread_pool_lock/parent-wake dance needed on the
 * single-threaded test host — just record the state transition so tests
 * can assert on it. Callers that then spin on `while (state==STOPPED)
 * sched_yield()` would hang here (sched_yield is a no-op stub); no current
 * test exercises that path directly. */
void sched_stop_current(int stop_signal) {
    struct thread *cur = sched_current();
    cur->state = THREAD_STOPPED;
    cur->stop_signal = (uint8_t)stop_signal;
}
