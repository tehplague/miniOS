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

#ifndef _MINIOS_SCHED_SCHED_H_
#define _MINIOS_SCHED_SCHED_H_

#include <miniOS/types.h>

#include <miniOS/arch/x86_64/context.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/signal.h>

#define THREAD_STACK_SIZE   16384   /* 16 KB kernel stack per thread */

/* SCHED_MAX_THREADS: thread pool size. Must accommodate MAX_CPUS idle threads
   plus user threads. Set to MAX_CPUS + 16 for headroom.
   MAX_CPUS is defined by the build system (arch/kernel CMakeLists.txt),
   defaulting to 8 if not provided at compile time. */
#ifndef MAX_CPUS
#define MAX_CPUS 8
#endif
#define SCHED_MAX_THREADS   (MAX_CPUS + 8)

#define USER_STACK_TOP   0x7FFFFFFFE000ULL   /* top of user stack */
#define USER_STACK_PAGES 16                  /* 16 x 4 KiB = 64 KiB user stack */

typedef enum {
    THREAD_RUNNABLE    = 0,
    THREAD_RUNNING     = 1,
    THREAD_DEAD        = 2,
    THREAD_WAITING     = 3,   /* blocked in sched_wait() until child exits */
    THREAD_BLOCKED     = 4,   /* not yet ready to run (setup in progress) */
    THREAD_FUTEX_WAIT  = 5,   /* blocked in sys_futex FUTEX_WAIT */
    THREAD_STOPPED     = 6,   /* stopped by SIGSTOP/SIGTSTP; resumes on SIGCONT */
} thread_state_t;

typedef void (*thread_func_t)(void *arg);

#define MMAP_BASE           0x70000000000ULL  /* anonymous mmap high base; grows downward */
#define MMAP_MAX_REGIONS    64

#define MMAP_REGION_ANON   0   /* anonymous private mapping (MAP_ANONYMOUS|MAP_PRIVATE) */
#define MMAP_REGION_FILE   1   /* file-backed private mapping (MAP_PRIVATE, fd >= 3) */

typedef struct {
    uint64_t virt_addr;   /* base VA of the mapping; 0 = free slot */
    uint64_t len;         /* length in bytes (multiple of PAGE_SIZE) */
    int      prot;        /* PROT_* flags as passed by caller */
    int      region_type; /* MMAP_REGION_ANON or MMAP_REGION_FILE */
    int      lazy;        /* 1 = pages not yet mapped; allocate on first #PF */
    /* File-backed mapping metadata (MMAP_REGION_FILE + lazy=1 only) */
    uint32_t   file_inode;   /* inode number of the mapped file */
    uint64_t   file_offset;  /* file offset corresponding to virt_addr */
    uint64_t   file_size;    /* bytes of real file data (tail pages zero-padded) */
    vfs_ops_t *file_ops;     /* filesystem ops; stable static pointer */
} mmap_region_t;

/* ---- Thread group (shared state for CLONE_VM|CLONE_FILES threads) ---- */
#define THREAD_GROUP_MAX 4

typedef struct thread_group {
    int           in_use;
    int           ref_count;   /* number of threads pointing at this group */
    spinlock_t    lock;
    vfs_file_t    *fd_table;
    uint64_t      brk;
    uint64_t      brk_base;    /* lower bound of heap (ELF image end); set once at execve */
    uint64_t      mmap_next;
    mmap_region_t *mmap_regions;
    char          cwd[VFS_PATH_MAX];
    uint64_t      pml4_phys;   /* shared address space (vmm_new_address_space()) */
} thread_group_t;

extern thread_group_t thread_group_pool[THREAD_GROUP_MAX];

struct thread {
    uint32_t           tid;
    thread_state_t     state;
    struct task_cpu_context ctx;   /* saved register state (used when not running) */
    uint8_t           *stack_base; /* kmalloc'd kernel stack buffer base */
    struct thread     *next;       /* intrusive linked list for run queue */

    /* User-mode stack -- NULL/0 for kernel-only threads */
    uint64_t   user_stack_phys_base; /* physical base of first user stack page (for cleanup) */
    uint64_t   user_stack_virt_top;  /* virtual address of stack top (passed in RSP via iretq) */
    uint8_t    user_stack_pages;     /* number of pages allocated (USER_STACK_PAGES) */

    /* Per-task open file table (VFS) */
    vfs_file_t  *fd_table;

    /* Fork/wait support */
    uint32_t   parent_tid;    /* tid of parent thread; 0 if none */
    int        exit_code;     /* exit code set by SYS_exit, read by parent's SYS_wait */

    /* POSIX process identity */
    uint32_t   pid;   /* process ID — equals tid at task creation */
    uint32_t   ppid;  /* parent process ID — parent->pid at fork, 0 for root tasks */
    uint32_t   pgid;  /* process group ID — default = pid; mutable via SYS_setpgid */
    uint32_t   sid;   /* session ID — default = pid at creation; set via SYS_setsid */

    /* Saved user-mode registers across context switches (used for fork child sysret) */
    uint64_t   saved_user_rsp;   /* user RSP at syscall entry */
    uint64_t   saved_user_rip;   /* user RIP at syscall entry (rcx from SYSCALL) */
    uint64_t   saved_user_rfl;   /* user RFLAGS at syscall entry (r11 from SYSCALL) */

    /* Callee-saved user registers at syscall entry (SYSCALL doesn't save these;
     * fork_child_trampoline must restore them so the child's stack frame is intact) */
    uint64_t   saved_user_rbp;
    uint64_t   saved_user_rbx;
    uint64_t   saved_user_r12;
    uint64_t   saved_user_r13;
    uint64_t   saved_user_r14;
    uint64_t   saved_user_r15;

    /* Argument registers for execve: set to argc/argv before sysretq to ELF entry.
     * Zero-initialised by sched_create_user_task; set by SYS_execve. */
    uint64_t   saved_user_rdi;   /* argc for new process */
    uint64_t   saved_user_rsi;   /* pointer to argv array for new process */
    uint64_t   saved_user_rdx;   /* pointer to envp array for new process */

    /* Heap break for SYS_brk / Newlib malloc support.
     * Initialised to 0; set to ELF load end by SYS_execve.
     * sys_brk() maps pages up to this address on each call. */
    uint64_t   brk;
    uint64_t   brk_base;  /* ELF image end = lowest valid heap VA; set once at task creation */

    /* Anonymous mmap region tracking.
     * mmap_next is the allocation cursor, initialised to MMAP_BASE at task creation.
     * Grows downward: each new mapping is placed below the previous one.
     * mmap_regions[] tracks up to MMAP_MAX_REGIONS active mappings; virt_addr==0 = free slot. */
    uint64_t      mmap_next;
    mmap_region_t *mmap_regions;

    /* Signal delivery */
    uint32_t        pending_signals;      /* bitmask: bit N set if signal N is pending (signals 1-31) */
    mini_sigaction_t signal_actions[32];  /* per-signal disposition; index 0 unused; default = SIG_DFL */

    /* Interval timer — ITIMER_REAL */
    uint64_t      itimer_real_deadline_ticks;  /* lapic tick count when alarm fires; 0 = inactive */
    uint64_t      itimer_real_interval_ticks;  /* reload interval in ticks; 0 = one-shot */
    uint8_t       itimer_real_active;          /* 1 = armed, 0 = disarmed */

    /* SA_RESTART syscall replay state */
    uint64_t  syscall_restart_nr;           /* syscall number of interrupted syscall */
    uint64_t  syscall_restart_args[6];      /* all 6 arguments at syscall entry */
    uint64_t  syscall_restart_saved_rip;    /* user RIP to restore in sigreturn */
    uint8_t   syscall_restart_pending;      /* 1 = interrupted syscall may restart */
    uint8_t   syscall_last_signal;          /* signal number dispatched, for SA_RESTART lookup */
    uint8_t   _restart_pad[6];             /* pad to 8-byte boundary */
    uint64_t  signal_trampoline_va;         /* userspace trampoline VA, set by SYS_register_sigtrampoline */
    /* Snapshot of syscall_restart_* taken at ring3_invoke_handler() time.
     * SYS_sigreturn uses these so syscalls inside the handler cannot corrupt
     * the pre-handler return state. */
    uint64_t  signal_ctx_rip;               /* saved_user_rip at handler-dispatch time */
    uint64_t  signal_ctx_rsp;               /* saved_user_rsp at handler-dispatch time (restored by sigreturn) */
    uint64_t  signal_ctx_restart_rip;       /* copy of syscall_restart_saved_rip */
    uint64_t  signal_ctx_restart_nr;        /* copy of syscall_restart_nr */
    uint64_t  signal_ctx_restart_args[6];   /* copy of syscall_restart_args */
    uint8_t   signal_ctx_restart_pending;   /* copy of syscall_restart_pending */
    uint8_t   _signal_ctx_pad[7];           /* pad to 8-byte boundary */

    /* Current working directory.
     * Initialized to "/" at task creation; updated by SYS_chdir.
     * Inherited (copied) by sched_fork(). Max length: VFS_PATH_MAX-1 chars. */
    char          cwd[VFS_PATH_MAX];

    /* Thread group (CLONE_VM|CLONE_FILES threads). NULL = single-threaded. */
    struct thread_group *tg;
    uint32_t  tgid;          /* thread group ID (= pid of group leader); 0 if not in group */
    uint32_t  _tg_pad;

    /* Per-process address space (vmm_new_address_space()). Every thread not in a
     * group owns its own; grouped threads share tg->pml4_phys via THREAD_PML4(). */
    uint64_t  pml4_phys;

    /* futex: uaddr being waited on, NULL if not in FUTEX_WAIT */
    volatile uint32_t *futex_uaddr;

    /* clone CLONE_CHILD_CLEARTID / set_tid_address: clear this VA on exit then FUTEX_WAKE 1 */
    volatile uint32_t *clear_tid_addr;

    /* /proc/<pid> support. comm = basename of the exec'd binary (TASK_COMM_LEN-style,
     * truncated). cmdline = NUL-separated argv as execve() received it, cmdline_len
     * bytes total (including each arg's NUL). utime_ticks = LAPIC ticks charged to
     * this thread while it was cpu->current_thread (approximate; no user/kernel split). */
    char      comm[16];
    char      cmdline[128];
    uint32_t  cmdline_len;
    uint64_t  utime_ticks;

    /* Job control: signal number that put this thread into THREAD_STOPPED
     * (SIGSTOP or SIGTSTP), for WSTOPSIG() via wait4's status word. Also
     * used for ptrace stops (always SIGTRAP) — wait4's existing THREAD_STOPPED
     * reporting path is shared by both job control and ptrace. */
    uint8_t   stop_signal;

    /* ptrace(2) — PTRACE_TRACEME only (no PTRACE_ATTACH to an unrelated running
     * process). ptrace_traced/ptrace_tracer_tid identify the tracer; a traced
     * thread's SYS_execve raises an initial PTRACE_EVENT_EXEC-equivalent stop
     * (see syscall_proc.c). ptrace_trace_syscalls arms PTRACE_SYSCALL
     * entry/exit stops in syscall_dispatch(). ptrace_stop_reason records why
     * the thread is (or was last) THREAD_STOPPED for ptrace's benefit, and
     * ptrace_syscall_nr/ptrace_syscall_ret let GETREGS report orig_rax/rax at
     * a syscall stop (rax otherwise never leaves the CPU register file at
     * that point — syscall_restart_nr/args capture the entry-time values). */
    uint8_t   ptrace_traced;
    uint8_t   ptrace_trace_syscalls;
    uint8_t   ptrace_stop_reason;   /* PTRACE_STOP_* below */
    uint32_t  ptrace_tracer_tid;
    uint64_t  ptrace_syscall_nr;
    int64_t   ptrace_syscall_ret;

    /* Set to the #DB handler's local interrupt-frame pointer while stopped
     * for PTRACE_STOP_SINGLESTEP, NULL otherwise. miniOS has two distinct
     * "return to userspace" paths — sysretq (SYSCALL entry, resumes from
     * saved_user_rip/rfl/rsp) and iretq (interrupt/exception entry, resumes
     * from this per-trap stack-local context) — and a ptrace stop can
     * legitimately happen via either one (a syscall stop always resumes via
     * sysretq; a singlestep #DB stop always resumes via iretq). Whichever
     * one applies, PTRACE_CONT/PTRACE_SINGLESTEP must set the TF flag (or
     * not) on the correct copy, or the resumed thread won't actually
     * single-step. NULL means "resumes via sysretq, use saved_user_rfl".
     * Never dereferenced by anything other than the thread's own #DB
     * handler and its own tracer, and only while genuinely stopped. */
    struct task_cpu_context *ptrace_trap_ctx;
};

/* Why a traced thread is (or was last) THREAD_STOPPED. Shared by
 * syscall.c (entry/exit stops), syscall_proc.c (TRACEME's initial exec
 * stop), and the #DB handler in exceptions.c (singlestep stops). */
#define PTRACE_STOP_SYSCALL_ENTRY 1
#define PTRACE_STOP_SYSCALL_EXIT  2
#define PTRACE_STOP_EXEC          3
#define PTRACE_STOP_SINGLESTEP    4

/* Resolve the active fd_table, brk, mmap_next, mmap_regions, cwd for thread t.
 * Threads in a group share all these via tg; single-threaded tasks use own fields. */
#define THREAD_FDT(t)           ((t)->tg ? (t)->tg->fd_table    : (t)->fd_table)
#define THREAD_BRK_PTR(t)       ((t)->tg ? &(t)->tg->brk        : &(t)->brk)
#define THREAD_BRK_BASE_PTR(t)  ((t)->tg ? &(t)->tg->brk_base   : &(t)->brk_base)
#define THREAD_MMAP_PTR(t)      ((t)->tg ? &(t)->tg->mmap_next  : &(t)->mmap_next)
#define THREAD_MMAPR(t)         ((t)->tg ? (t)->tg->mmap_regions : (t)->mmap_regions)
#define THREAD_CWD(t)           ((t)->tg ? (t)->tg->cwd          : (t)->cwd)
#define THREAD_PML4(t)          ((t)->tg ? (t)->tg->pml4_phys    : (t)->pml4_phys)

/* idle_loop: per-CPU idle function; runs sti;hlt in a loop. Not static so
   sched_init_cpu() can wire it as the per-AP idle thread entry point. */
void __attribute__((noreturn)) idle_loop(void *arg);

/**
 * sched_init() - Initialise the scheduler and create the idle thread.
 *
 * Captures the current execution context (boot stack, boot code) as the
 * idle thread. Must be called with interrupts disabled, before the first
 * call to sched_tick().
 */
void sched_init(void);

/**
 * sched_init_cpu() - Create the idle thread for CPU @cpu_id.
 * @cpu_id: Logical CPU index (0=BSP, 1..N-1=APs).
 *
 * Creates a per-CPU idle thread using sched_create_thread(idle_loop, NULL),
 * sets its state to THREAD_RUNNING (since the CPU is currently "running" it),
 * and assigns it to g_cpus[cpu_id].idle_thread.
 *
 * For cpu_id == 0 (BSP): called after sched_init() as a no-op (BSP idle is
 *   already set up by sched_init via idle_thread_storage).
 * For cpu_id > 0 (APs): creates a fresh idle thread in thread_pool[].
 */
void sched_init_cpu(uint32_t cpu_id);

/**
 * sched_create_thread() - Create a new kernel thread.
 * @func: Entry function; called with @arg as its sole argument.
 * @arg: Opaque argument forwarded to @func.
 *
 * Allocates a thread slot from thread_pool and a 16 KiB kernel stack via
 * kmalloc. The thread is enqueued as THREAD_BLOCKED; the caller must set
 * state = THREAD_RUNNABLE once the context is fully initialised to avoid
 * a race with the LAPIC timer ISR.
 *
 * @return: Pointer to the new thread on success, NULL on OOM.
 */
struct thread *sched_create_thread(thread_func_t func, void *arg);

/**
 * sched_tick() - Advance the scheduler from the LAPIC timer ISR.
 *
 * Increments the tick counter and calls the internal scheduler to switch
 * to the next runnable thread when the quantum expires.
 *
 * Context: Must only be called from the LAPIC timer ISR with interrupts
 *          disabled. Must not sleep or allocate memory.
 */
void sched_tick(void);

/**
 * sched_current() - Return the currently running thread.
 *
 * @return: Pointer to the thread currently executing on the CPU.
 */
struct thread *sched_current(void);

/**
 * sched_next() - Pick the next RUNNABLE thread (round-robin).
 *
 * Walks the run queue to find the next thread in THREAD_RUNNABLE state.
 * Falls back to the idle thread if no runnable thread is found. Does NOT
 * perform the actual CPU context switch — that is done by context_switch_asm.
 *
 * @return: Pointer to the next thread to run (never NULL).
 */
struct thread *sched_next(void);

/**
 * sched_yield() - Voluntarily yield the CPU to the next runnable thread.
 *
 * Marks the calling thread as THREAD_RUNNABLE and invokes the internal
 * scheduler to switch to the next thread immediately.
 */
void sched_yield(void);

/**
 * sched_create_user_task() - Create a new ring-3 user thread.
 * @entry_rip: Virtual address of the user-mode entry point.
 *
 * Allocates USER_STACK_PAGES (64) pages of user stack just below
 * USER_STACK_TOP (0x7FFFFFFFE000), with an implicit guard page below.
 * The thread starts at ring-3 with RIP=@entry_rip, RSP=USER_STACK_TOP-8,
 * RFLAGS=0x202 (interrupts enabled).
 *
 * @return: Pointer to the new thread on success, NULL on OOM.
 */
struct thread *sched_create_user_task(uint64_t entry_rip);

/**
 * sched_exit_current() - Terminate the current thread.
 * @code: Exit code stored in thread->exit_code for the parent's SYS_wait.
 *
 * Marks the current thread THREAD_DEAD, frees its user stack pages, and
 * invokes the scheduler. Does not return.
 *
 * Context: Called from the SYS_exit syscall handler.
 */
void sched_exit_current(int code);

/**
 * sched_has_user_threads() - Return true if any non-idle thread is still alive.
 * Used by bsp_idle_resume to avoid calling qemu_exit while a user task is
 * still running (e.g. preempted by the LAPIC timer).
 */
bool sched_has_user_threads(void);

/**
 * sched_fork() - Fork the current user task.
 * @child_rdi: Value to restore into the child's RDI before it returns to
 *             userspace (fork_child_trampoline reads it from
 *             child->saved_user_rdi) — the raw syscall arg1, which for a
 *             fork(2) call is whatever the caller's libc left in RDI across
 *             the syscall instruction (e.g. mlibc's internal fork() sysdep
 *             uses it to stash a scratch pointer it writes through after
 *             the syscall returns).
 *
 * Allocates a new thread and copies all USER_STACK_PAGES physical frames
 * from the parent's user stack (walking page tables via vmm_virt_to_phys
 * to handle non-contiguous PMM allocations). The parent returns the child's
 * thread pointer; the child is wired to return 0 via fork_child_trampoline.
 *
 * IMPORTANT: @child_rdi must be set on the child BEFORE it is marked
 * THREAD_RUNNABLE, not by the caller afterward — under SMP, a remote CPU can
 * dequeue and start running the child (via a scheduler-kick IPI) before the
 * fork() syscall handler's own C code resumes on the parent's CPU, so any
 * post-hoc `child->saved_user_rdi = arg1` after sched_fork() returns races
 * the child's own fork_child_trampoline reading that same field and reliably
 * loses under real multi-CPU timing (the child observes 0, not @child_rdi).
 *
 * Context: Called from the SYS_fork / SYS_clone (no-CLONE_VM fallback)
 * syscall handlers. Panics on OOM.
 * @return: Pointer to the child thread (for the parent); child takes the
 *          fork_child_trampoline path and never returns through this function.
 */
struct thread *sched_fork(uint64_t child_rdi);

/**
 * sched_wait() - Block until a child thread exits.
 * @child_tid: TID of the child thread to wait for.
 * @exit_code: Output pointer; set to the child's exit code on success.
 *             May be NULL if the caller does not need the exit code.
 *
 * Sets the calling thread to THREAD_WAITING and yields until the child
 * sets its state to THREAD_DEAD.
 *
 * @return: 0 on success, -1 if @child_tid is not found or is not a child
 *          of the calling thread.
 */
int sched_wait(uint32_t child_tid, int *exit_code);

/**
 * sched_clone() - Create a new thread sharing the caller's address space.
 * @child_stack: Top of the new thread's user stack (passed in by clone syscall).
 * @tls:         FS.base value for TLS (0 = inherit from parent).
 * @child_tid:   If non-NULL, write new thread's TID here.
 *
 * Creates a new thread that shares the calling thread's thread_group (fd_table,
 * brk, mmap_regions, cwd). If the calling thread has no group yet, allocates
 * a new thread_group_t and migrates the caller into it.
 *
 * @return Pointer to new thread, or NULL on OOM.
 */
struct thread *sched_clone(uint64_t child_stack, uint64_t tls, uint32_t *child_tid);

/**
 * sched_balance_enqueue() - Enqueue @t on the CPU with the shortest run queue.
 * @t: Thread to enqueue. Must be in THREAD_BLOCKED state; caller sets RUNNABLE.
 *
 * Scans g_cpus[0..smp_cpu_count-1] for minimum queue_depth and tail-inserts
 * @t there. If the target CPU differs from the calling CPU, sends a
 * scheduler-kick IPI (SCHEDULER_KICK_VECTOR) to wake it from hlt.
 *
 * Context: Called from sched_create_thread() with thread_pool_lock NOT held.
 */
void sched_balance_enqueue(struct thread *t);

/**
 * sched_signal_thread() - Deliver a signal to a thread, waking it if blocked.
 * @t: Target thread.
 * @sig: Signal number (1-31).
 *
 * Sets the pending bit and, if the thread is THREAD_WAITING, transitions it
 * to THREAD_RUNNABLE so the scheduler will pick it up. This enables waitpid
 * and other blocking waits to return -EINTR when signalled.
 */
void sched_signal_thread(struct thread *t, int sig);

/**
 * sched_stop_current() - Transition the calling thread to THREAD_STOPPED.
 * @stop_signal: SIGSTOP or SIGTSTP — recorded on the thread for WSTOPSIG().
 *
 * Sets the current thread's state to THREAD_STOPPED and wakes any parent
 * blocked in THREAD_WAITING (job control: a parent's wait4(WUNTRACED) call
 * should return as soon as a child stops, not just on exit). Called from
 * signal_dispatch() for the default SIGSTOP/SIGTSTP action; the caller is
 * responsible for yielding afterward until SIGCONT resumes the thread
 * (sched_signal_thread() handles the THREAD_STOPPED -> THREAD_RUNNABLE
 * transition on SIGCONT).
 */
void sched_stop_current(int stop_signal);

/**
 * sched_ptrace_stop() - Stop the calling (traced) thread for its tracer.
 * @reason: PTRACE_STOP_* value recorded in ptrace_stop_reason.
 *
 * Sets stop_signal = SIGTRAP (wait4's THREAD_STOPPED reporting path is shared
 * with job control) and state = THREAD_STOPPED, wakes the tracer
 * (ptrace_tracer_tid) if it's blocked in THREAD_WAITING, then blocks
 * (sched_yield() loop) until PTRACE_CONT/PTRACE_SINGLESTEP resumes this
 * thread. Called from syscall_dispatch() (PTRACE_SYSCALL entry/exit stops)
 * and the user-mode #DB handler (PTRACE_SINGLESTEP stops).
 */
void sched_ptrace_stop(int reason);

/**
 * sched_wake_tid() - Wake a thread sleeping in THREAD_FUTEX_WAIT by tid.
 * @tid: Thread ID to wake.
 *
 * Used by network receive callbacks to wake a thread blocked in poll/select.
 * No-op if the thread is not in THREAD_FUTEX_WAIT state.
 */
void sched_wake_tid(uint32_t tid);

/**
 * sched_work_steal() - Steal half the work from the busiest CPU.
 *
 * Called from idle_loop() when the current CPU's run queue is empty.
 * Acquires thread_pool_lock, finds the CPU with the highest queue_depth > 1,
 * steals floor(queue_depth/2) THREAD_RUNNABLE threads, transfers them to the
 * calling CPU's queue.
 *
 * Context: Called with interrupts enabled (from idle_loop before hlt).
 * Does nothing if no stealable work exists.
 */
void sched_work_steal(void);

/* Expose thread_pool for sched_wait iteration */
extern struct thread thread_pool[SCHED_MAX_THREADS];

/* sched_tick_count: LAPIC timer ticks since boot, incremented by sched_tick().
 * Used as a coarse jiffies-equivalent (e.g. /proc/stat's "cpu" line). */
extern volatile uint64_t sched_tick_count;

/* Exposed for sched_balance.c — guards thread_pool[], run queues, thread_count */
extern spinlock_t thread_pool_lock;

/**
 * sched_activate_task() - Hand off scheduler "current" to @t without a full context switch.
 * @t: Thread to make the new current thread. Must be THREAD_BLOCKED.
 *
 * Used by kernel_main before enter_ring3() to ensure that when @t calls
 * SYS_exit the scheduler correctly identifies it as the running thread and
 * can switch back to idle. Sets the old current to THREAD_RUNNABLE and @t
 * to THREAD_RUNNING.
 */
void sched_activate_task(struct thread *t);

/**
 * context_switch_asm() - Save current CPU state and load new context.
 * @old_ctx: Pointer to the task_cpu_context of the thread being switched away from.
 *           Callee-saved registers and RIP/RSP are written here.
 * @new_ctx: Pointer to the task_cpu_context of the thread to switch to.
 *           Registers are restored from this structure.
 *
 * Context: Called with interrupts disabled. Uses call-based save (not iretq):
 *          saves the return address as the resumed RIP. Pure C-callable function.
 */
extern void context_switch_asm(struct task_cpu_context *old_ctx,
                                struct task_cpu_context *new_ctx);

#endif /* _MINIOS_SCHED_SCHED_H_ */
