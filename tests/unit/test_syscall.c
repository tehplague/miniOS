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

/* test_syscall.c — syscall dispatch unit tests.
 * Includes syscall.c directly to test syscall_dispatch().
 * Stubs out asm externs and IDT handler. Does NOT call syscall_init()
 * (which touches MSRs and IDT). */

#include "unity.h"
#include <string.h>
#include <miniOS/fs/vfs.h>

/* -----------------------------------------------------------------------
 * Stubs for symbols that syscall.c externs from assembly / arch code
 * --------------------------------------------------------------------- */

/* Stub assembly entry points (declared as extern void in syscall.c) */
void syscall_entry(void) { /* no-op stub */ }
void int80_entry(void)   { /* no-op stub */ }

/* Stub kernel RSP storage (lives in syscall.asm .data in the real build) */
uint64_t syscall_kernel_rsp_storage = 0;

/* Stub IDT handler installer (syscall_init only; we don't call syscall_init) */
void idt_set_handler(uint8_t vec, addr_t addr, unsigned int dpl, uint8_t gate_type) {
    (void)vec; (void)addr; (void)dpl; (void)gate_type;
}

/* context_switch_asm stub: sched.h declares the real signature;
 * stub_sched.c is linked and provides this stub already. */

/* Stub ipi_tlb_shootdown: syscall.c calls this after vmm_unmap_page on SMP.
 * In host unit tests, no real LAPIC or SMP is present — stub is a no-op. */
#include <miniOS/ipi/ipi.h>
ipi_barrier_t g_tlb_barrier = { .ack_count = 0, .virt = 0 };
void ipi_tlb_shootdown(uint64_t virt) { (void)virt; }

/* Stub for cpu_local(): syscall.c calls this for SYS_sched_getcpu.
 * cpu_local() uses mov %%gs:0 which would read garbage on host.
 * Override it here before syscall.c is included. */
#include <miniOS/arch/x86_64/smp.h>
static cpu_t fake_cpu_for_syscall_test;
static cpu_t *stub_cpu_local_ptr = &fake_cpu_for_syscall_test;
/* Redefine cpu_local as a macro so it returns our stub pointer.
 * This overrides the static inline in smp.h before syscall.c uses it. */
#undef cpu_local
#define cpu_local() stub_cpu_local_ptr

/* -----------------------------------------------------------------------
 * Pull in the real syscall dispatch implementation.
 * wrmsr/rdmsr are static inlines in syscall.c — they compile fine on
 * host gcc (emit inline asm) but must not be executed. We only call
 * syscall_dispatch(), not syscall_init().
 * --------------------------------------------------------------------- */
#include "../../src/kernel/syscall.c"

/* signal.c is not included inline by syscall.c; pull in the implementation directly for unit testing */
#include "../../src/kernel/signal.c"

/* pipe.c must be included for pipe tests — SYS_pipe dispatches to pipe_create() */
#include "../../src/kernel/fs/pipe.c"

/* Pull in the three syscall dispatch subsystems. syscall_fs.c also provides the
 * types (mini_pollfd, kernel_stat_t, POLLIN, …) used by the tests below. */
#include "../../src/kernel/syscall_fs.c"
#include "../../src/kernel/syscall_proc.c"

/* syscall_dispatch_mm and syscall_dispatch_net stubs — no MM/net tests in this file. */
int64_t syscall_dispatch_mm(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)nr; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return SYSCALL_DISPATCH_UNHANDLED;
}
int64_t syscall_dispatch_net(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)nr; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return SYSCALL_DISPATCH_UNHANDLED;
}
/* proc_close_all_sockets lives in syscall_net.c; stub it out since net is not included. */
void proc_close_all_sockets(struct thread *t) { (void)t; }

/* -----------------------------------------------------------------------
 * Externals from stub files (linked separately)
 * stub_sched.c exports: sched_current, sched_fork, sched_wait,
 *                       sched_exit_current, stub_sched_exit_called
 * stub_keyboard.c exports: keyboard_read_char, stub_keyboard_char
 * stub_vfs.c exports: vfs_open, vfs_read, vfs_close, vfs_mount, vfs_readdir
 * stub_elf.c exports: elf_load
 * stub_heap.c exports: kmalloc, kfree, stub_heap_reset
 * --------------------------------------------------------------------- */
extern int  stub_sched_exit_called;
extern int  stub_sched_wait_exit_code;
extern int  stub_sched_wait_retval;
extern char stub_keyboard_char;
extern char stub_tty_read_char;
extern struct minios_termios stub_tty_termios;
extern void stub_heap_reset(void);

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    stub_sched_exit_called = 0;
    stub_sched_wait_exit_code = 0;
    stub_sched_wait_retval    = 0;
    stub_keyboard_char = 'x';
    stub_tty_read_char = 'x';
    /* Reset heap arena so pipe_t allocations start at low addresses each test */
    stub_heap_reset();
    /* Zero the test thread's fd_table so VFS fd state is clean */
    struct thread *t = sched_current();
    memset(t->fd_table, 0, sizeof(t->fd_table));
    /* Zero thread_pool slots 0-4 to prevent state bleed between tests */
    memset(&thread_pool[0], 0, sizeof(thread_pool[0]));
    memset(&thread_pool[1], 0, sizeof(thread_pool[1]));
    memset(&thread_pool[2], 0, sizeof(thread_pool[2]));
    memset(&thread_pool[3], 0, sizeof(thread_pool[3]));
    memset(&thread_pool[4], 0, sizeof(thread_pool[4]));
    /* Reset signal state so signal tests are independent */
    t->pending_signals = 0;
    memset(t->signal_actions, 0, sizeof(t->signal_actions));
    /* Reset cwd to "/" before each test so chdir tests start from known state */
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    /* Reset fake cpu_id so SYS_sched_getcpu tests don't bleed state */
    fake_cpu_for_syscall_test.cpu_id = 0;
}

void tearDown(void) {
    /* nothing */
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* SYS_write (nr=1) with fd=1, valid buf, count=5 returns 5 */
void test_write_to_stdout_returns_count(void) {
    const char buf[] = "hello";
    int64_t r = syscall_dispatch(SYS_write, 1, (uint64_t)(uintptr_t)buf, 5, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, r);
}

/* SYS_write with fd=2 (stderr) also routes to console — returns count */
void test_write_to_non_stdout_returns_ebadf(void) {
    const char buf[] = "hello";
    int64_t r = syscall_dispatch(SYS_write, 2, (uint64_t)(uintptr_t)buf, 5, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, r);
}

/* SYS_read (nr=0) with fd=0 (stdin), count=1 returns 1 char from keyboard stub */
void test_read_from_stdin_returns_keyboard_char(void) {
    stub_keyboard_char = 'A';
    stub_tty_read_char = 'A';
    char buf[4] = {0};
    int64_t r = syscall_dispatch(SYS_read, 0, (uint64_t)(uintptr_t)buf, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(1, r);
    TEST_ASSERT_EQUAL_CHAR('A', buf[0]);
}

void test_ioctl_tiocgwinsz_returns_25x80(void) {
    struct minios_winsize ws = {0};
    int64_t r = syscall_dispatch(SYS_ioctl, 1, TIOCGWINSZ, (uint64_t)(uintptr_t)&ws, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    TEST_ASSERT_EQUAL_UINT16(25, ws.ws_row);
    TEST_ASSERT_EQUAL_UINT16(80, ws.ws_col);
}

void test_ioctl_non_tty_fd_returns_enotty_tcgets(void) {
    struct thread *t = sched_current();
    t->fd_table[3].in_use = 1;
    t->fd_table[3].ftype = VFS_FILE_TYPE_REG;
    t->fd_table[3].device_type = VFS_DEVICE_NONE;

    int64_t r = syscall_dispatch(SYS_ioctl, 3, TCGETS, (uint64_t)(uintptr_t)&stub_tty_termios, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-25, r);
}

void test_ioctl_non_tty_fd_returns_enotty_tiocgwinsz(void) {
    struct thread *t = sched_current();
    t->fd_table[3].in_use = 1;
    t->fd_table[3].ftype = VFS_FILE_TYPE_REG;
    t->fd_table[3].device_type = VFS_DEVICE_NONE;

    struct minios_winsize ws = {0};
    int64_t r = syscall_dispatch(SYS_ioctl, 3, TIOCGWINSZ, (uint64_t)(uintptr_t)&ws, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-25, r);
}

/* SYS_open (nr=2) with NULL path returns -22 (EINVAL) */
void test_open_with_null_path_returns_einval(void) {
    int64_t r = syscall_dispatch(SYS_open, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_open with valid path returns VFS_FIRST_OPEN_FD */
void test_open_returns_fd_three_for_first_file(void) {
    const char path[] = "/test";
    int64_t r = syscall_dispatch(SYS_open, (uint64_t)(uintptr_t)path, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(VFS_FIRST_OPEN_FD, r);
}

void test_read_from_stdout_rejects_non_file_fd(void) {
    char buf[4] = {0};
    int64_t r = syscall_dispatch(SYS_read, VFS_FD_STDOUT, (uint64_t)(uintptr_t)buf, sizeof(buf), 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

void test_getdents_rejects_stdio_fd(void) {
    char buf[32] = {0};
    int64_t r = syscall_dispatch(SYS_getdents, VFS_FD_STDIN, (uint64_t)(uintptr_t)buf, sizeof(buf), 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_close (nr=3) with any fd returns 0 (stub_vfs always succeeds) */
void test_close_valid_fd_returns_zero(void) {
    int64_t r = syscall_dispatch(SYS_close, 3, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

/* unknown syscall number (e.g., 999) returns -38 (ENOSYS) */
void test_unknown_syscall_returns_enosys(void) {
    int64_t r = syscall_dispatch(999, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-38, r);
}

/* SYS_read with fd=0, NULL buf returns -22 (EINVAL) */
void test_read_with_null_buf_returns_einval(void) {
    int64_t r = syscall_dispatch(SYS_read, 0, 0, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_getpid (nr=39): returns sched_current()->pid */
void test_getpid_returns_current_pid(void) {
    struct thread *t = sched_current();
    t->pid = 5;
    int64_t r = syscall_dispatch(SYS_getpid, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, r);
}

/* SYS_getppid (nr=110): returns parent->pid from thread_pool lookup */
void test_getppid_returns_parent_pid(void) {
    struct thread *t = sched_current();
    t->pid        = 3;
    t->parent_tid = 42;
    /* Install a fake parent entry in thread_pool */
    thread_pool[1].tid   = 42;
    thread_pool[1].pid   = 7;
    thread_pool[1].state = THREAD_RUNNABLE;
    int64_t r = syscall_dispatch(SYS_getppid, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(7, r);
    /* Cleanup: clear the fake entry */
    thread_pool[1].tid = 0;
    thread_pool[1].pid = 0;
}

/* SYS_getppid: returns 0 when parent state is THREAD_DEAD */
void test_getppid_returns_zero_if_parent_dead(void) {
    struct thread *t = sched_current();
    t->parent_tid = 55;
    thread_pool[2].tid   = 55;
    thread_pool[2].pid   = 9;
    thread_pool[2].state = THREAD_DEAD;
    int64_t r = syscall_dispatch(SYS_getppid, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    thread_pool[2].tid = 0;
    thread_pool[2].pid = 0;
}

/* SYS_setsid (nr=112): non-group-leader succeeds, returns new SID */
void test_setsid_non_leader_succeeds(void) {
    struct thread *t = sched_current();
    t->pid  = 5;
    t->pgid = 3;   /* pgid != pid → not a group leader */
    t->sid  = 3;
    int64_t r = syscall_dispatch(SYS_setsid, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, r);       /* returns new SID */
    TEST_ASSERT_EQUAL_UINT32(5, t->sid);
    TEST_ASSERT_EQUAL_UINT32(5, t->pgid);
}

/* SYS_setsid: group leader (pgid == pid) returns -EPERM */
void test_setsid_group_leader_returns_eperm(void) {
    struct thread *t = sched_current();
    t->pid  = 5;
    t->pgid = 5;   /* pgid == pid → already a group leader */
    int64_t r = syscall_dispatch(SYS_setsid, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-1, r);   /* EPERM */
}

/* SYS_setpgid (nr=109): move self to own group (pgid=own pid) succeeds */
void test_setpgid_self_to_own_group(void) {
    struct thread *t = sched_current();
    t->pid  = 5;
    t->pgid = 3;
    t->sid  = 1;
    /* thread_pool[0] must reflect caller — stub_sched returns &test_thread
     * but SYS_setpgid scans thread_pool by pid; ensure test_thread is in pool */
    thread_pool[0].pid   = 5;
    thread_pool[0].ppid  = 0;
    thread_pool[0].pgid  = 3;
    thread_pool[0].sid   = 1;
    thread_pool[0].state = THREAD_RUNNING;
    /* setpgid(0, 5): pid=0 means self, pgid=5 means "make own group" */
    int64_t r = syscall_dispatch(SYS_setpgid, 0, 5, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    thread_pool[0].pid = 0;
    thread_pool[0].state = 0;
}

/* SYS_setpgid: moving target to different-session group returns -EPERM */
void test_setpgid_cross_session_returns_eperm(void) {
    struct thread *caller = sched_current();
    caller->pid = 5;
    caller->sid = 1;
    /* Install a target process with different session */
    thread_pool[3].pid   = 10;
    thread_pool[3].ppid  = caller->pid;
    thread_pool[3].pgid  = 10;
    thread_pool[3].sid   = 2;   /* different session than caller */
    thread_pool[3].state = THREAD_RUNNABLE;
    int64_t r = syscall_dispatch(SYS_setpgid, 10, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-1, r);   /* EPERM */
    thread_pool[3].pid = 0;
    thread_pool[3].state = 0;
}

/* SYS_setpgid: unknown PID returns -ESRCH */
void test_setpgid_unknown_pid_returns_esrch(void) {
    int64_t r = syscall_dispatch(SYS_setpgid, 999, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-3, r);   /* ESRCH */
}

/* SYS_setpgid: negative pgid returns -EINVAL */
void test_setpgid_negative_pgid_returns_einval(void) {
    struct thread *t = sched_current();
    t->pid = 5;
    int64_t r = syscall_dispatch(SYS_setpgid, 0, (uint64_t)-1, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);   /* EINVAL */
}

/* -----------------------------------------------------------------------
 * SYS_waitpid tests
 * --------------------------------------------------------------------- */

/* Helper: install a fake child in thread_pool[4] */
static void install_fake_child(uint32_t child_pid, uint32_t parent_pid,
                                thread_state_t state, int exit_code) {
    thread_pool[4].pid       = child_pid;
    thread_pool[4].ppid      = parent_pid;
    thread_pool[4].tid       = child_pid + 100;  /* tid != pid is intentional */
    thread_pool[4].state     = state;
    thread_pool[4].exit_code = exit_code;
}

/* SYS_waitpid (nr=7): successfully waits for a DEAD child, returns child PID */
void test_waitpid_matches_specific_child(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    install_fake_child(20, 1, THREAD_DEAD, 0);
    int64_t r = syscall_dispatch(SYS_waitpid, 20, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(20, r);
}

/* SYS_waitpid: unknown PID (not a child of caller) returns -ECHILD */
void test_waitpid_returns_echild_for_unknown_pid(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    int64_t r = syscall_dispatch(SYS_waitpid, 999, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-10, r);   /* ECHILD */
}

/* SYS_waitpid: after reaping, second call on same PID returns -ECHILD */
void test_waitpid_reaps_zombie_second_call_returns_echild(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    install_fake_child(20, 1, THREAD_DEAD, 0);
    /* First wait: succeeds */
    int64_t r1 = syscall_dispatch(SYS_waitpid, 20, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(20, r1);
    /* Second wait: child->pid==0 after reap → ECHILD */
    int64_t r2 = syscall_dispatch(SYS_waitpid, 20, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-10, r2);
}

/* SYS_waitpid: exit status encoded as (exit_code << 8); WEXITSTATUS == exit_code */
void test_waitpid_encodes_exit_status(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    install_fake_child(20, 1, THREAD_DEAD, 42);   /* child exited with code 42 */
    int status = 0;
    int64_t r = syscall_dispatch(SYS_waitpid, 20, (uint64_t)(uintptr_t)&status, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(20, r);
    TEST_ASSERT_EQUAL_INT(42 << 8, status);           /* raw encoded value */
    TEST_ASSERT_EQUAL_INT(42, (status >> 8) & 0xff);  /* WEXITSTATUS equivalent */
}

/* SYS_waitpid: a thread killed by an uncaught signal (signal.c's default-
 * terminate path stores exit_code = 128+signum) encodes as WIFSIGNALED —
 * low byte holds the terminating signal, not (code<<8) like a real exit. */
void test_waitpid_encodes_signaled_status(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    install_fake_child(20, 1, THREAD_DEAD, 128 + 11);  /* killed by SIGSEGV */
    int status = 0;
    int64_t r = syscall_dispatch(SYS_waitpid, 20, (uint64_t)(uintptr_t)&status, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(20, r);
    TEST_ASSERT_EQUAL_INT(11, status);         /* WTERMSIG equivalent */
    TEST_ASSERT_EQUAL_INT(0, status & 0x80);   /* not confused with WIFSTOPPED's 0x7f sentinel */
}

/* SYS_wait4 with WUNTRACED (2): a THREAD_STOPPED child is reported (WIFSTOPPED
 * sentinel 0x7f, WSTOPSIG in the next byte) without being reaped — a second
 * wait4 call must still find the same (still-alive) child. */
void test_wait4_wuntraced_reports_stopped_child_without_reaping(void) {
    struct thread *t = sched_current();
    t->pid = 1;
    install_fake_child(20, 1, THREAD_STOPPED, 0);
    thread_pool[4].stop_signal = SIGSTOP;

    int status = 0;
    int64_t r = syscall_dispatch(SYS_wait4, 20, (uint64_t)(uintptr_t)&status, 2 /* WUNTRACED */, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(20, r);
    TEST_ASSERT_EQUAL_INT(0x7f, status & 0x7f);              /* WIFSTOPPED */
    TEST_ASSERT_EQUAL_INT(SIGSTOP, (status >> 8) & 0xff);    /* WSTOPSIG */
    TEST_ASSERT_EQUAL_UINT32(20, thread_pool[4].pid);        /* not reaped */

    /* Without WUNTRACED, a still-stopped child must not be reported as if
     * it were the (nonexistent) dead child — falls through to the blocking
     * wait path instead (not exercised further here; this just confirms the
     * WUNTRACED gate, not the blocking loop). */
}

/* -----------------------------------------------------------------------
 * signal_dispatch() tests
 * --------------------------------------------------------------------- */

/* signal_dispatch(): SIGKILL (nr=9) must call sched_exit_current */
void test_signal_dispatch_sigkill_calls_exit(void) {
    struct thread *t = sched_current();
    t->pending_signals = (1u << SIGKILL);
    signal_dispatch();
    TEST_ASSERT_EQUAL_INT(1, stub_sched_exit_called);
    t->pending_signals = 0;
}

/* signal_dispatch(): SIGTERM (nr=15) must call sched_exit_current */
void test_signal_dispatch_sigterm_calls_exit(void) {
    struct thread *t = sched_current();
    t->pending_signals = (1u << SIGTERM);
    signal_dispatch();
    TEST_ASSERT_EQUAL_INT(1, stub_sched_exit_called);
    t->pending_signals = 0;
}

/* signal_dispatch(): SIG_DFL on SIGCHLD must clear pending bit without calling exit.
 * SIGCHLD's default action is "ignore", not "terminate", so the process must survive. */
void test_signal_dispatch_clears_pending_on_sigdfl(void) {
    struct thread *t = sched_current();
    t->pending_signals = (1u << SIGCHLD);   /* SIGCHLD default = ignore, not terminate */
    t->signal_actions[SIGCHLD].sa_handler = SIG_DFL;
    signal_dispatch();
    TEST_ASSERT_EQUAL_INT(0, stub_sched_exit_called);
    TEST_ASSERT_EQUAL_UINT32(0, t->pending_signals);
}

/* signal_dispatch(): SIG_IGN handler must clear pending bit without calling exit */
void test_signal_dispatch_clears_pending_on_sigign(void) {
    struct thread *t = sched_current();
    t->pending_signals = (1u << 4);
    t->signal_actions[4].sa_handler = SIG_IGN;
    signal_dispatch();
    TEST_ASSERT_EQUAL_INT(0, stub_sched_exit_called);
    TEST_ASSERT_EQUAL_UINT32(0, t->pending_signals);
}

/* -----------------------------------------------------------------------
 * SYS_kill tests
 * --------------------------------------------------------------------- */

/* SYS_kill: sets pending_signals on target task */
void test_kill_sets_pending_signal(void) {
    /* Set up a fake target in thread_pool slot 1 */
    thread_pool[1].pid   = 5;
    thread_pool[1].state = THREAD_RUNNABLE;
    thread_pool[1].pending_signals = 0;

    int64_t r = syscall_dispatch(SYS_kill, 5, SIGTERM, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    TEST_ASSERT_EQUAL_UINT32((1u << SIGTERM), thread_pool[1].pending_signals);

    /* Cleanup */
    thread_pool[1].pid   = 0;
    thread_pool[1].state = THREAD_DEAD;
    thread_pool[1].pending_signals = 0;
}

/* SYS_kill: returns -EINVAL for sig == 0 (out of range) */
void test_kill_returns_einval_for_sig_zero(void) {
    int64_t r = syscall_dispatch(SYS_kill, 5, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_kill: returns -ESRCH when target PID not found */
void test_kill_returns_esrch_for_missing_pid(void) {
    int64_t r = syscall_dispatch(SYS_kill, 9999, SIGTERM, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-3, r);
}

/* SYS_kill: THREAD_DEAD tasks are not valid targets */
void test_kill_does_not_target_dead_thread(void) {
    thread_pool[1].pid   = 6;
    thread_pool[1].state = THREAD_DEAD;

    int64_t r = syscall_dispatch(SYS_kill, 6, SIGTERM, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-3, r);

    thread_pool[1].pid   = 0;
    thread_pool[1].state = THREAD_DEAD;
}

/* -----------------------------------------------------------------------
 * SYS_pipe tests
 * --------------------------------------------------------------------- */

/* SYS_pipe: returns 0, writes two valid fds >= VFS_FIRST_OPEN_FD */
void test_pipe_allocates_two_fds(void) {
    int fds[2] = {-1, -1};
    int64_t r = syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    TEST_ASSERT_TRUE(fds[0] >= VFS_FIRST_OPEN_FD);
    TEST_ASSERT_TRUE(fds[1] >= VFS_FIRST_OPEN_FD);
    TEST_ASSERT_NOT_EQUAL(fds[0], fds[1]);
}

/* SYS_pipe with NULL ptr returns -14 (EFAULT) */
void test_pipe_null_ptr_returns_efault(void) {
    int64_t r = syscall_dispatch(SYS_pipe, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-14, r);
}

/* SYS_pipe + SYS_write to write-end + SYS_read from read-end: data round-trips */
void test_pipe_write_read_roundtrip(void) {
    int fds[2] = {-1, -1};
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);

    const char msg[] = "hello";
    int64_t wr = syscall_dispatch(SYS_write, (uint64_t)fds[1],
                                  (uint64_t)(uintptr_t)msg, 5, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, wr);

    char buf[8] = {0};
    int64_t rd = syscall_dispatch(SYS_read, (uint64_t)fds[0],
                                  (uint64_t)(uintptr_t)buf, 8, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(5, rd);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, 5);
}

/* SYS_pipe + close write-end + SYS_read returns 0 (EOF) */
void test_pipe_read_returns_eof_after_write_close(void) {
    int fds[2] = {-1, -1};
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);

    /* Close write end via SYS_close */
    syscall_dispatch(SYS_close, (uint64_t)fds[1], 0, 0, 0, 0, 0);

    char buf[8] = {0};
    int64_t rd = syscall_dispatch(SYS_read, (uint64_t)fds[0],
                                  (uint64_t)(uintptr_t)buf, 8, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, rd);  /* EOF */
}

/* -----------------------------------------------------------------------
 * SYS_dup / SYS_dup2 tests
 * --------------------------------------------------------------------- */

/* SYS_dup on invalid fd returns -9 (EBADF) */
void test_dup_invalid_fd_returns_ebadf(void) {
    int64_t r = syscall_dispatch(SYS_dup, (uint64_t)99, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-9, r);
}

/* SYS_dup on valid pipe read-end: returns distinct new fd; closing original leaves dup'd fd alive */
void test_dup_close_independence(void) {
    /* Create a pipe */
    int fds[2] = {-1, -1};
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);
    int rfd = fds[0];
    int wfd = fds[1];

    /* dup the read end */
    int64_t dup_fd = syscall_dispatch(SYS_dup, (uint64_t)rfd, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(dup_fd >= VFS_FIRST_OPEN_FD);
    TEST_ASSERT_NOT_EQUAL(dup_fd, rfd);

    /* Both fds should point to the same pipe_t */
    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_PTR(t->fd_table[rfd].pipe, t->fd_table[dup_fd].pipe);

    /* Write via write end, close original read fd, read via dup'd fd */
    const char msg[] = "dup";
    syscall_dispatch(SYS_write, (uint64_t)wfd, (uint64_t)(uintptr_t)msg, 3, 0, 0, 0);

    /* Close original read fd — pipe should NOT be freed (dup_fd still open) */
    syscall_dispatch(SYS_close, (uint64_t)rfd, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, t->fd_table[rfd].in_use);

    /* dup'd fd should still work */
    char buf[8] = {0};
    int64_t rd = syscall_dispatch(SYS_read, dup_fd, (uint64_t)(uintptr_t)buf, 8, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(3, rd);
    TEST_ASSERT_EQUAL_STRING_LEN("dup", buf, 3);
    (void)wfd;
}

/* SYS_dup2(oldfd, newfd): newfd ends up pointing to oldfd's pipe */
void test_dup2_redirects_newfd_to_oldfd(void) {
    int fds[2] = {-1, -1};
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);
    int rfd = fds[0];
    int wfd = fds[1];

    /* Pick a newfd slot that is currently empty */
    int newfd = 10;  /* slot 10 is free after setUp memset */
    int64_t r = syscall_dispatch(SYS_dup2, (uint64_t)rfd, (uint64_t)newfd, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64((int64_t)newfd, r);

    /* newfd should point to same pipe as rfd */
    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_INT(1, t->fd_table[newfd].in_use);
    TEST_ASSERT_EQUAL_PTR(t->fd_table[rfd].pipe, t->fd_table[newfd].pipe);

    /* Write via write end, read via newfd */
    const char msg[] = "d2";
    syscall_dispatch(SYS_write, (uint64_t)wfd, (uint64_t)(uintptr_t)msg, 2, 0, 0, 0);
    char buf[8] = {0};
    int64_t rd = syscall_dispatch(SYS_read, (uint64_t)newfd, (uint64_t)(uintptr_t)buf, 8, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(2, rd);
    TEST_ASSERT_EQUAL_STRING_LEN("d2", buf, 2);
    (void)rfd;
}

/* -----------------------------------------------------------------------
 * SYS_chdir / SYS_getcwd tests (Phase 26)
 * --------------------------------------------------------------------- */

/* SYS_chdir (nr=81) with NULL path returns -22 (EINVAL) */
void test_chdir_rejects_null(void) {
    int64_t r = syscall_dispatch(SYS_chdir, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_chdir with "/" (root dir, always a dir) returns 0 and updates cwd */
void test_chdir_updates_cwd(void) {
    const char *target = "/";
    int64_t r = syscall_dispatch(SYS_chdir, (uint64_t)(uintptr_t)target, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);

    struct thread *t = sched_current();
    TEST_ASSERT_EQUAL_STRING("/", t->cwd);
}

/* SYS_chdir with a regular file path returns -20 (ENOTDIR) */
void test_chdir_enotdir(void) {
    /* stub_vfs returns VFS_FILE_TYPE_REG for "/test" */
    const char *target = "/test";
    int64_t r = syscall_dispatch(SYS_chdir, (uint64_t)(uintptr_t)target, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-20, r);
}

/* SYS_getcwd (nr=80) copies cwd to buffer and returns buffer pointer */
void test_getcwd_returns_cwd(void) {
    struct thread *t = sched_current();
    strcpy(t->cwd, "/mydir");

    char buf[64];
    int64_t r = syscall_dispatch(SYS_getcwd, (uint64_t)(uintptr_t)buf, sizeof(buf), 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64((int64_t)(uintptr_t)buf, r);
    TEST_ASSERT_EQUAL_STRING("/mydir", buf);
}

/* SYS_getcwd returns -34 (ERANGE) when buffer is too small */
void test_getcwd_erange(void) {
    struct thread *t = sched_current();
    strcpy(t->cwd, "/mydir");  /* 7 bytes including null */

    char buf[4];  /* too small for "/mydir\0" */
    int64_t r = syscall_dispatch(SYS_getcwd, (uint64_t)(uintptr_t)buf, sizeof(buf), 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-34, r);
}

/* SYS_getcwd returns -22 (EINVAL) when buf is NULL */
void test_getcwd_null_buf(void) {
    int64_t r = syscall_dispatch(SYS_getcwd, 0, 64, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* -----------------------------------------------------------------------
 * SYS_fcntl (nr=72) tests
 * --------------------------------------------------------------------- */

/* SYS_fcntl F_GETFL (cmd=3) returns flags|status_flags for an open fd */
void test_fcntl_getfl_returns_flags(void) {
    struct thread *t = sched_current();
    /* Manually set up an open fd at slot 3 with flags=0, status_flags=0 */
    t->fd_table[3].in_use       = 1;
    t->fd_table[3].flags        = 0;   /* O_RDONLY */
    t->fd_table[3].status_flags = 0;

    int64_t r = syscall_dispatch(SYS_fcntl, 3, 3, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

/* SYS_fcntl F_SETFL (cmd=4) stores O_NONBLOCK (0x800) in status_flags */
void test_fcntl_setfl_stores_nonblock(void) {
    struct thread *t = sched_current();
    t->fd_table[3].in_use       = 1;
    t->fd_table[3].flags        = 0;
    t->fd_table[3].status_flags = 0;

    int64_t r_set = syscall_dispatch(SYS_fcntl, 3, 4, 0x800, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r_set);

    int64_t r_get = syscall_dispatch(SYS_fcntl, 3, 3, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0x800, r_get);
}

/* SYS_fcntl with invalid fd returns -9 (EBADF) */
void test_fcntl_invalid_fd_returns_ebadf(void) {
    int64_t r = syscall_dispatch(SYS_fcntl, 99, 3, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-9, r);
}

/* SYS_fcntl with unknown cmd returns -22 (EINVAL) */
void test_fcntl_unknown_cmd_returns_einval(void) {
    struct thread *t = sched_current();
    t->fd_table[3].in_use = 1;
    t->fd_table[3].flags  = 0;

    int64_t r = syscall_dispatch(SYS_fcntl, 3, 99, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* -----------------------------------------------------------------------
 * SYS_poll (nr=23) tests
 * --------------------------------------------------------------------- */

/* poll on a pipe fd with pending data returns 1 immediately with POLLIN set */
void test_poll_pollin_on_readable_pipe(void) {
    /* Create a real pipe so the pipe_t pointer is valid */
    int fds[2];
    int64_t r_pipe = syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r_pipe);

    /* Write 1 byte into the write end */
    const char byte = 'X';
    syscall_dispatch(SYS_write, (uint64_t)fds[1], (uint64_t)(uintptr_t)&byte, 1, 0, 0, 0);

    /* Poll read end for POLLIN with timeout=0 (non-blocking) */
    struct mini_pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };
    int64_t r = syscall_dispatch(SYS_poll, (uint64_t)(uintptr_t)&pfd, 1, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(1, r);
    TEST_ASSERT_TRUE(pfd.revents & POLLIN);
}

/* poll with timeout=0 on an empty pipe returns 0 */
void test_poll_timeout_zero_empty_pipe_returns_zero(void) {
    int fds[2];
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);

    struct mini_pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };
    int64_t r = syscall_dispatch(SYS_poll, (uint64_t)(uintptr_t)&pfd, 1, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

/* poll with invalid fd populates POLLNVAL and returns 1 (not -1) */
void test_poll_invalid_fd_sets_pollnval(void) {
    struct mini_pollfd pfd = { .fd = 99, .events = POLLIN, .revents = 0 };
    int64_t r = syscall_dispatch(SYS_poll, (uint64_t)(uintptr_t)&pfd, 1, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(1, r);
    TEST_ASSERT_TRUE(pfd.revents & POLLNVAL);
}

/* poll with nfds=0 returns 0 immediately */
void test_poll_nfds_zero_returns_zero(void) {
    int64_t r = syscall_dispatch(SYS_poll, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

/* -----------------------------------------------------------------------
 * SYS_select (nr=24) tests
 * --------------------------------------------------------------------- */

/* select on a readable pipe fd with readfds set returns 1; fd bit set in readfds */
void test_select_readable_pipe_returns_one(void) {
    int fds[2];
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);

    const char byte = 'Y';
    syscall_dispatch(SYS_write, (uint64_t)fds[1], (uint64_t)(uintptr_t)&byte, 1, 0, 0, 0);

    /* Build an fd_set with fds[0] set */
    mini_fd_set_t readfds;
    for (int b = 0; b < 8; b++) readfds.bits[b] = 0;
    readfds.bits[fds[0] / 64] |= (1ULL << (fds[0] % 64));

    /* Timeout = {0, 0} (non-blocking) */
    mini_timeval_t tv = { .tv_sec = 0, .tv_usec = 0 };

    int64_t r = syscall_dispatch(SYS_select,
                                 (uint64_t)(fds[0] + 1),
                                 (uint64_t)(uintptr_t)&readfds,
                                 0, 0,
                                 (uint64_t)(uintptr_t)&tv, 0);
    TEST_ASSERT_EQUAL_INT64(1, r);
    /* fds[0] bit must still be set in readfds (it was ready) */
    TEST_ASSERT_TRUE(readfds.bits[fds[0] / 64] & (1ULL << (fds[0] % 64)));
}

/* select on empty pipe with timeout={0,0} returns 0; fd bit cleared in readfds */
void test_select_empty_pipe_timeout_returns_zero(void) {
    int fds[2];
    syscall_dispatch(SYS_pipe, (uint64_t)(uintptr_t)fds, 0, 0, 0, 0, 0);

    mini_fd_set_t readfds;
    for (int b = 0; b < 8; b++) readfds.bits[b] = 0;
    readfds.bits[fds[0] / 64] |= (1ULL << (fds[0] % 64));

    mini_timeval_t tv = { .tv_sec = 0, .tv_usec = 0 };

    int64_t r = syscall_dispatch(SYS_select,
                                 (uint64_t)(fds[0] + 1),
                                 (uint64_t)(uintptr_t)&readfds,
                                 0, 0,
                                 (uint64_t)(uintptr_t)&tv, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
    /* fd bit must be cleared (not ready) */
    TEST_ASSERT_FALSE(readfds.bits[fds[0] / 64] & (1ULL << (fds[0] % 64)));
}

/* -----------------------------------------------------------------------
 * SYS_lstat (nr=6) and SYS_readlink (nr=89) tests
 * --------------------------------------------------------------------- */

/* SYS_lstat on an existing path returns 0 (same as stat; ext2 has no symlinks) */
void test_lstat_existing_path_returns_zero(void) {
    kernel_stat_t st;
    memset(&st, 0, sizeof(st));
    /* stub_vfs returns VFS_FILE_TYPE_REG for "/test" */
    int64_t r = syscall_dispatch(SYS_lstat, (uint64_t)(uintptr_t)"/test",
                                 (uint64_t)(uintptr_t)&st, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

/* SYS_lstat with NULL path returns -22 (EINVAL) */
void test_lstat_null_path_returns_einval(void) {
    kernel_stat_t st;
    int64_t r = syscall_dispatch(SYS_lstat, 0, (uint64_t)(uintptr_t)&st, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_readlink on existing non-symlink path returns -22 (EINVAL) */
void test_readlink_non_symlink_returns_einval(void) {
    char buf[64];
    /* stub_vfs returns VFS_FILE_TYPE_REG for "/test" — not a symlink */
    int64_t r = syscall_dispatch(SYS_readlink, (uint64_t)(uintptr_t)"/test",
                                 (uint64_t)(uintptr_t)buf, 64, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_readlink with NULL path returns -22 (EINVAL) */
void test_readlink_null_path_returns_einval(void) {
    char buf[64];
    int64_t r = syscall_dispatch(SYS_readlink, 0,
                                 (uint64_t)(uintptr_t)buf, 64, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_readlink on non-existent path returns -2 (ENOENT) */
void test_readlink_missing_path_returns_enoent(void) {
    char buf[64];
    /* stub_vfs returns -2 (ENOENT) for any path that is not known */
    int64_t r = syscall_dispatch(SYS_readlink, (uint64_t)(uintptr_t)"/nonexistent",
                                 (uint64_t)(uintptr_t)buf, 64, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-2, r);
}

void test_unlink_regular_file_returns_zero(void) {
    int64_t r = syscall_dispatch(SYS_unlink, (uint64_t)(uintptr_t)"/test", 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

void test_unlink_directory_returns_eisdir(void) {
    int64_t r = syscall_dispatch(SYS_unlink, (uint64_t)(uintptr_t)"/tmp", 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-21, r);
}

void test_access_existing_file_fok_returns_zero(void) {
    int64_t r = syscall_dispatch(SYS_access, (uint64_t)(uintptr_t)"/test", 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

void test_access_regular_file_xok_returns_eacces(void) {
    int64_t r = syscall_dispatch(SYS_access, (uint64_t)(uintptr_t)"/test", 1, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-13, r);
}

void test_rmdir_valid_path_returns_zero(void) {
    int64_t r = syscall_dispatch(SYS_rmdir, (uint64_t)(uintptr_t)"/tmp", 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

void test_rmdir_null_path_returns_einval(void) {
    int64_t r = syscall_dispatch(SYS_rmdir, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(-22, r);
}

/* SYS_sched_getcpu (nr=318): returns current CPU ID from cpu_local()->cpu_id */
void test_sched_getcpu_returns_zero_on_cpu0(void) {
    fake_cpu_for_syscall_test.cpu_id = 0;
    int64_t r = syscall_dispatch(SYS_sched_getcpu, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(0, r);
}

void test_sched_getcpu_returns_correct_id_cpu2(void) {
    fake_cpu_for_syscall_test.cpu_id = 2;
    int64_t r = syscall_dispatch(SYS_sched_getcpu, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT64(2, r);
}

void test_sched_getcpu_bounded(void) {
    fake_cpu_for_syscall_test.cpu_id = 3;
    int64_t r = syscall_dispatch(SYS_sched_getcpu, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, r);
    TEST_ASSERT_LESS_THAN_INT64(MAX_CPUS, r);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_write_to_stdout_returns_count);
    RUN_TEST(test_write_to_non_stdout_returns_ebadf);
    RUN_TEST(test_read_from_stdin_returns_keyboard_char);
    RUN_TEST(test_ioctl_tiocgwinsz_returns_25x80);
    RUN_TEST(test_ioctl_non_tty_fd_returns_enotty_tcgets);
    RUN_TEST(test_ioctl_non_tty_fd_returns_enotty_tiocgwinsz);
    RUN_TEST(test_read_with_null_buf_returns_einval);
    RUN_TEST(test_read_from_stdout_rejects_non_file_fd);
    RUN_TEST(test_open_with_null_path_returns_einval);
    RUN_TEST(test_open_returns_fd_three_for_first_file);
    RUN_TEST(test_getdents_rejects_stdio_fd);
    RUN_TEST(test_close_valid_fd_returns_zero);
    RUN_TEST(test_unknown_syscall_returns_enosys);
    RUN_TEST(test_getpid_returns_current_pid);
    RUN_TEST(test_getppid_returns_parent_pid);
    RUN_TEST(test_getppid_returns_zero_if_parent_dead);
    RUN_TEST(test_setsid_non_leader_succeeds);
    RUN_TEST(test_setsid_group_leader_returns_eperm);
    RUN_TEST(test_setpgid_self_to_own_group);
    RUN_TEST(test_setpgid_cross_session_returns_eperm);
    RUN_TEST(test_setpgid_unknown_pid_returns_esrch);
    RUN_TEST(test_setpgid_negative_pgid_returns_einval);
    RUN_TEST(test_waitpid_matches_specific_child);
    RUN_TEST(test_waitpid_returns_echild_for_unknown_pid);
    RUN_TEST(test_waitpid_reaps_zombie_second_call_returns_echild);
    RUN_TEST(test_waitpid_encodes_exit_status);
    RUN_TEST(test_signal_dispatch_sigkill_calls_exit);
    RUN_TEST(test_signal_dispatch_sigterm_calls_exit);
    RUN_TEST(test_signal_dispatch_clears_pending_on_sigdfl);
    RUN_TEST(test_signal_dispatch_clears_pending_on_sigign);
    RUN_TEST(test_kill_sets_pending_signal);
    RUN_TEST(test_kill_returns_einval_for_sig_zero);
    RUN_TEST(test_kill_returns_esrch_for_missing_pid);
    RUN_TEST(test_kill_does_not_target_dead_thread);
    RUN_TEST(test_pipe_allocates_two_fds);
    RUN_TEST(test_pipe_null_ptr_returns_efault);
    RUN_TEST(test_pipe_write_read_roundtrip);
    RUN_TEST(test_pipe_read_returns_eof_after_write_close);
    RUN_TEST(test_dup_invalid_fd_returns_ebadf);
    RUN_TEST(test_dup_close_independence);
    RUN_TEST(test_dup2_redirects_newfd_to_oldfd);
    RUN_TEST(test_chdir_rejects_null);
    RUN_TEST(test_chdir_updates_cwd);
    RUN_TEST(test_chdir_enotdir);
    RUN_TEST(test_getcwd_returns_cwd);
    RUN_TEST(test_getcwd_erange);
    RUN_TEST(test_getcwd_null_buf);
    RUN_TEST(test_fcntl_getfl_returns_flags);
    RUN_TEST(test_fcntl_setfl_stores_nonblock);
    RUN_TEST(test_fcntl_invalid_fd_returns_ebadf);
    RUN_TEST(test_fcntl_unknown_cmd_returns_einval);
    RUN_TEST(test_poll_pollin_on_readable_pipe);
    RUN_TEST(test_poll_timeout_zero_empty_pipe_returns_zero);
    RUN_TEST(test_poll_invalid_fd_sets_pollnval);
    RUN_TEST(test_poll_nfds_zero_returns_zero);
    RUN_TEST(test_select_readable_pipe_returns_one);
    RUN_TEST(test_select_empty_pipe_timeout_returns_zero);
    RUN_TEST(test_lstat_existing_path_returns_zero);
    RUN_TEST(test_lstat_null_path_returns_einval);
    RUN_TEST(test_readlink_non_symlink_returns_einval);
    RUN_TEST(test_readlink_null_path_returns_einval);
    RUN_TEST(test_readlink_missing_path_returns_enoent);
    RUN_TEST(test_unlink_regular_file_returns_zero);
    RUN_TEST(test_unlink_directory_returns_eisdir);
    RUN_TEST(test_access_existing_file_fok_returns_zero);
    RUN_TEST(test_access_regular_file_xok_returns_eacces);
    RUN_TEST(test_rmdir_valid_path_returns_zero);
    RUN_TEST(test_rmdir_null_path_returns_einval);
    RUN_TEST(test_sched_getcpu_returns_zero_on_cpu0);
    RUN_TEST(test_sched_getcpu_returns_correct_id_cpu2);
    RUN_TEST(test_sched_getcpu_bounded);
    RUN_TEST(test_waitpid_encodes_signaled_status);
    RUN_TEST(test_wait4_wuntraced_reports_stopped_child_without_reaping);
    return UNITY_END();
}
