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

#include <miniOS/sched/sched.h>
#include <miniOS/syscall.h>
#include <miniOS/signal.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/fs/pipe.h>
#include <miniOS/net/unix_sock.h>
#include <miniOS/mm/heap.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/arch/x86_64/tss.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <miniOS/io.h>
#include <string.h>

static inline uint64_t sched_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Lock ordering (must always be acquired in this order to prevent deadlock):
 *   thread_pool_lock -> heap_lock -> pmm_lock
 * Never acquire an earlier lock while holding a later lock. */

/* per_cpu_update_rsp0() — per-CPU TSS RSP0 + cpu_t.kstack_top updater.
 * Replaces the old tss_set_rsp0() + syscall_kernel_rsp_storage pair with a
 * single per-CPU call so each AP updates only its own state. */
extern void per_cpu_update_rsp0(uint64_t rsp0);

/* Forward declaration: defined in syscall.c */
extern void __attribute__((noreturn)) fork_child_trampoline(void);

/* Forward declaration: defined in syscall_net.c */
extern void proc_close_all_sockets(struct thread *t);

/* --------------------------------------------------------------------------
 * Global scheduler state
 * -------------------------------------------------------------------------- */

struct thread  thread_pool[SCHED_MAX_THREADS];   /* pre-allocated thread slots */
thread_group_t thread_group_pool[THREAD_GROUP_MAX]; /* thread group pool for clone() */
static uint32_t       thread_count = 1;                 /* idle is slot 0; next slot starts at 1 */
static uint32_t       next_tid = 1;                     /* monotonically increasing TID counter */
/* current_thread moved to cpu_t.current_thread (per-CPU).
   Access via cpu_local()->current_thread everywhere. */
static struct thread  idle_thread_storage;               /* idle thread; always in queue */
volatile uint64_t sched_tick_count = 0;
spinlock_t            thread_pool_lock = SPINLOCK_INIT;  /* non-static: used by sched_balance.c; guards thread_pool[], run_queue, thread_count */

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static void sched_schedule(void);

/* --------------------------------------------------------------------------
 * idle_loop  — the body of the idle thread
 *
 * Runs with interrupts enabled (IF=1 from ctx.rflags=0x202).  The CPU halts
 * until the next interrupt (LAPIC timer), at which point sched_tick fires and
 * picks a real thread to run.  We never return from this loop.
 * -------------------------------------------------------------------------- */
void __attribute__((noreturn)) idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        sched_work_steal();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

/**
 * sched_init() - Initialise the scheduler.
 *
 * Initialises thread_pool, sets up the run queue (circular singly-linked list
 * with tail pointer), and captures the current boot context as the idle thread.
 * Idle thread uses static storage (not kmalloc'd stack) and the boot stack.
 * Sets g_current = &idle, state = THREAD_RUNNING.
 */
void sched_init(void)
{
    /* Save fd_table across re-init (sched_init is called twice: once early,
     * once after SMP). On the first call it's NULL; on the second we keep the
     * existing buffer rather than leak it (memset below would clear the pointer). */
    vfs_file_t *saved_fdt = idle_thread_storage.fd_table;

    memset(&idle_thread_storage, 0, sizeof(idle_thread_storage));

    if (saved_fdt) {
        memset(saved_fdt, 0, VFS_MAX_FDS * sizeof(vfs_file_t));
        idle_thread_storage.fd_table = saved_fdt;
    } else {
        idle_thread_storage.fd_table = (vfs_file_t *)kmalloc(VFS_MAX_FDS * sizeof(vfs_file_t));
        if (idle_thread_storage.fd_table)
            memset(idle_thread_storage.fd_table, 0, VFS_MAX_FDS * sizeof(vfs_file_t));
    }

    idle_thread_storage.tid        = 0;
    idle_thread_storage.state      = THREAD_RUNNING;  /* currently executing */
    idle_thread_storage.stack_base = NULL;             /* uses boot stack */
    idle_thread_storage.next       = &idle_thread_storage; /* circular list, length 1 */

    /* Initialise idle's CPU context so context_switch_asm can safely restore
     * it at any time — even before a timer has had a chance to save it.
     * Without this, the first context switch TO idle (e.g. when a timer fires
     * during a long sched_exit_current path) loads ctx.rip=0 and crashes.
     *
     * RSP: capture the current boot-stack RSP.  context_switch_asm pushes
     * ctx.rip at ctx.rsp-8 and rets, so idle_loop starts with RSP = this value.
     * The boot stack is live as long as kernel_main runs, which is forever. */
    uint64_t boot_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(boot_rsp));
    idle_thread_storage.ctx.rip    = (uint64_t)idle_loop;
    idle_thread_storage.ctx.rsp    = boot_rsp;
    idle_thread_storage.ctx.cs     = KERNEL_CS_SEL;  /* kernel code segment */
    idle_thread_storage.ctx.ss     = KERNEL_SS_SEL;  /* kernel data segment */
    idle_thread_storage.ctx.rflags = RFLAGS_IF;      /* IF=1 so hlt can be woken */

    g_cpus[0].current_thread = &idle_thread_storage;
    g_cpus[0].idle_thread    = &idle_thread_storage;  /* fallback for sched_next() */

    /* Wire BSP idle into per-CPU queue for CPU 0 */
    g_cpus[0].run_queue_head = &idle_thread_storage;
    g_cpus[0].run_queue_tail = &idle_thread_storage;
    g_cpus[0].queue_depth    = 0;  /* idle not counted in depth */

    printk("sched: initialized, idle thread tid=0\n");
}

/**
 * sched_create_thread() - Allocate and initialise a kernel thread.
 * @func: Entry function pointer.
 * @arg: Argument passed to @func in RDI (first parameter register).
 *
 * Scans thread_pool[] for the first slot with state == THREAD_DEAD (or
 * unused). Allocates a 16 KiB kernel stack via kmalloc. Sets initial context:
 * RIP=func, RSP=stack_top-8, RDI=arg, CS=0x08, SS=0x10, RFLAGS=0x202.
 * Appends thread to run queue at tail (before idle) for round-robin fairness.
 * Thread starts THREAD_BLOCKED; caller sets THREAD_RUNNABLE when ready.
 *
 * @return: Thread pointer, or NULL on OOM (kmalloc failed or pool exhausted).
 */
struct thread *sched_create_thread(thread_func_t func, void *arg)
{
    struct thread *t    = NULL;
    uint8_t       *stack = NULL;
    bool           reused = false;

    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);

    /* Scan for a dead slot to recycle (slot 0 is idle, skip it).
     * Only recycle slots that have been reaped (pid==0): a DEAD thread with
     * pid!=0 is a zombie waiting for its parent to call waitpid().  Recycling
     * such a slot before the parent reaps it destroys the exit-status record
     * and leaves the parent unable to find any dead children. */
    for (uint32_t i = 1; i < thread_count; i++) {
        if (thread_pool[i].state == THREAD_DEAD && thread_pool[i].pid == 0) {
            t      = &thread_pool[i];
            stack  = t->stack_base;   /* reuse existing kernel stack */
            reused = true;
            break;
        }
    }

    if (!t) {
        if (thread_count >= SCHED_MAX_THREADS) {
            printk("sched: thread pool exhausted (max %d)\n", SCHED_MAX_THREADS);
            spinlock_irqrestore(&thread_pool_lock, flags);
            return NULL;
        }
        /* Drop lock before kmalloc: kmalloc acquires heap_lock (-> pmm_lock).
         * Lock ordering is thread_pool_lock -> heap_lock -> pmm_lock, but
         * kmalloc itself must not be called while holding thread_pool_lock if
         * something else may try to acquire thread_pool_lock while holding
         * heap_lock.  Safest pattern: drop, allocate, reacquire, re-check. */
        spinlock_irqrestore(&thread_pool_lock, flags);
        stack = kmalloc(THREAD_STACK_SIZE);
        if (!stack) {
            printk("sched: kmalloc failed for thread stack\n");
            return NULL;
        }
        spinlock_irqsave(&thread_pool_lock, &flags);
        /* Re-check: another CPU may have filled the last slot while we were outside the lock */
        if (thread_count >= SCHED_MAX_THREADS) {
            spinlock_irqrestore(&thread_pool_lock, flags);
            kfree(stack);
            return NULL;
        }
        t = &thread_pool[thread_count];
    }

    /* Save the run-queue link before zeroing the slot */
    struct thread *saved_next = reused ? t->next : NULL;

    memset(t, 0, sizeof(*t));

    /* Initial CPU context: entry point is func, first arg via rdi (SysV ABI) */
    t->ctx.rip    = (uint64_t)func;
    t->ctx.rsp    = (uint64_t)(stack + THREAD_STACK_SIZE - 8);
    t->ctx.rdi    = (uint64_t)arg;
    t->ctx.cs     = KERNEL_CS_SEL;   /* kernel code segment selector */
    t->ctx.ss     = KERNEL_SS_SEL;   /* kernel data segment selector */
    t->ctx.rflags = RFLAGS_IF;       /* IF=1 (interrupts enabled), reserved bit 1 */

    /* Place null return address at top of stack */
    *(uint64_t *)(stack + THREAD_STACK_SIZE - 8) = 0;

    t->tid        = next_tid++;
    t->state      = THREAD_BLOCKED;  /* caller sets RUNNABLE when fully set up */
    t->stack_base = stack;

    if (reused) {
        /* Restore run-queue link — slot is already in the circular list */
        t->next = saved_next;
    } else {
        /* New slot: increment thread_count, then place on least-busy CPU via
         * sched_balance_enqueue() (called after lock release below). */
        thread_count++;
    }

    printk("sched: created thread tid=%u rip=0x%llx rsp=0x%llx\n",
           t->tid, t->ctx.rip, t->ctx.rsp);

    spinlock_irqrestore(&thread_pool_lock, flags);

    /* Allocate fd_table for all threads — kernel threads need it for VFS.
     * Done outside the lock (lock ordering: thread_pool_lock -> heap_lock). */
    t->fd_table = (vfs_file_t *)kmalloc(VFS_MAX_FDS * sizeof(vfs_file_t));
    if (!t->fd_table) {
        printk("sched: kmalloc failed for fd_table tid=%u\n", t->tid);
        for (;;) __asm__("hlt");
    }
    memset(t->fd_table, 0, VFS_MAX_FDS * sizeof(vfs_file_t));

    if (!reused) {
        /* Enqueue on least-busy CPU's run queue. Called outside thread_pool_lock
         * (sched_balance_enqueue acquires it internally). */
        sched_balance_enqueue(t);
    }

    return t;
}

/**
 * sched_current() - Return pointer to the currently running thread.
 *
 * Returns the global g_current pointer. Called from syscall handlers and
 * context switch paths to identify the active task.
 *
 * @return: Pointer to the current thread (never NULL after sched_init).
 */
struct thread *sched_current(void)
{
    return cpu_local()->current_thread;
}

/**
 * sched_next() - Select the next RUNNABLE thread via round-robin.
 *
 * Starts from current_thread->next and walks the circular run queue,
 * returning the first thread in THREAD_RUNNABLE state. Starting after
 * the current thread (not from run_queue_head) ensures fair round-robin:
 * threads that arrive later in the queue are not starved by earlier threads
 * that are frequently RUNNABLE (e.g. idle, net_poll busy-loop).
 *
 * If no runnable thread is found after a full traversal, returns the idle
 * thread. Does not modify current_thread.
 *
 * @return: Next thread to run (never NULL; falls back to idle).
 */
struct thread *sched_next(void)
{
    cpu_t         *cpu   = cpu_local();
    struct thread *cur   = cpu->current_thread;
    struct thread *t     = (cur && cur->next) ? cur->next : cpu->run_queue_head;
    struct thread *start = t;

    do {
        if (t->state == THREAD_RUNNABLE)
            return t;
        t = t->next;
    } while (t != start);

    /* All threads on this CPU blocked/dead — return this CPU's idle thread */
    return cpu->idle_thread;
}

/**
 * sched_has_user_threads() - Check if any non-idle thread is still alive.
 *
 * Returns true if at least one thread slot with index > 0 (i.e., not the
 * idle thread) has a state other than THREAD_DEAD.  Used by bsp_idle_resume
 * to decide whether to call qemu_exit or continue halting.
 */
bool sched_has_user_threads(void)
{
    for (int i = 1; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].state != THREAD_DEAD && thread_pool[i].tid != 0)
            return true;
    }
    return false;
}

/**
 * sched_signal_thread() - Deliver a signal to a thread, waking it if blocked.
 * @t: Target thread.
 * @sig: Signal number (1-31).
 *
 * Sets the pending bit and, if the thread is THREAD_WAITING, transitions it
 * to THREAD_RUNNABLE so the scheduler will pick it up. This enables waitpid
 * and other blocking waits to return -EINTR when signalled.
 */
void sched_signal_thread(struct thread *t, int sig)
{
    if (!t || sig < 1 || sig > 31)
        return;
    /* CR-03: protect pending_signals bitmask and state transition with
     * irqsave to prevent a lost-update race between concurrent deliveries
     * and between normal context and the LAPIC timer ISR. */
    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    /* SIGCONT on a stopped thread resumes it directly — a THREAD_STOPPED
     * thread isn't running, so it can never reach signal_dispatch() to
     * process its own pending-signal bitmask the way every other signal
     * does. SIGCONT itself isn't queued in that case (nothing to deliver
     * to a handler for; matches default SIGCONT semantics). */
    if (sig == SIGCONT && t->state == THREAD_STOPPED) {
        t->state = THREAD_RUNNABLE;
        spinlock_irqrestore(&thread_pool_lock, flags);
        return;
    }
    t->pending_signals |= (1u << sig);
    /* Wake THREAD_WAITING and THREAD_FUTEX_WAIT so blocking syscalls can check pending_signals */
    if (t->state == THREAD_WAITING || t->state == THREAD_FUTEX_WAIT)
        t->state = THREAD_RUNNABLE;
    spinlock_irqrestore(&thread_pool_lock, flags);
}

void sched_stop_current(int stop_signal)
{
    struct thread *cur = sched_current();
    cur->stop_signal = (uint8_t)stop_signal;

    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    cur->state = THREAD_STOPPED;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].tid == cur->parent_tid &&
            thread_pool[i].state == THREAD_WAITING) {
            thread_pool[i].state = THREAD_RUNNABLE;
            break;
        }
    }
    spinlock_irqrestore(&thread_pool_lock, flags);
}

void sched_ptrace_stop(int reason)
{
    struct thread *cur = sched_current();
    cur->ptrace_stop_reason = (uint8_t)reason;
    cur->stop_signal = SIGTRAP;

    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    cur->state = THREAD_STOPPED;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].tid == cur->ptrace_tracer_tid &&
            thread_pool[i].state == THREAD_WAITING) {
            thread_pool[i].state = THREAD_RUNNABLE;
            break;
        }
    }
    spinlock_irqrestore(&thread_pool_lock, flags);

    while (cur->state == THREAD_STOPPED)
        sched_yield();
}

void sched_wake_tid(uint32_t tid)
{
    if (!tid) return;
    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].tid == tid &&
            thread_pool[i].state == THREAD_FUTEX_WAIT) {
            thread_pool[i].state = THREAD_RUNNABLE;
            break;
        }
    }
    spinlock_irqrestore(&thread_pool_lock, flags);
}

/* --------------------------------------------------------------------------
 * sched_schedule  (internal: pick next, update state, context switch)
 * -------------------------------------------------------------------------- */

static void sched_schedule(void)
{
    cpu_t         *cpu  = cpu_local();
    struct thread *old  = cpu->current_thread;
    struct thread *next = sched_next();

    if (next == old)
        return;   /* nothing else to run */

    /* Only reset state to RUNNABLE if the thread is not DEAD, WAITING, FUTEX_WAIT,
     * or STOPPED (e.g. sched_exit_current may have set it to DEAD before calling
     * sched_schedule; sched_wait sets it to WAITING; sys_futex sets it to
     * FUTEX_WAIT; signal_dispatch sets it to STOPPED for SIGSTOP/SIGTSTP). */
    if (old->state != THREAD_DEAD && old->state != THREAD_WAITING &&
        old->state != THREAD_FUTEX_WAIT && old->state != THREAD_STOPPED)
        old->state = THREAD_RUNNABLE;
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;

    /* Update this CPU's TSS RSP0 and cpu_t.kstack_top to the top of the next
     * thread's kernel stack.  per_cpu_update_rsp0() writes both atomically:
     * - percpu_tss[cpu_id].privileged_stack_table[0]: used by hardware on
     *   ring-3 → ring-0 transitions (interrupts, exceptions) from user mode
     * - cpu->kstack_top (gs:64): read by syscall_entry to switch kernel stacks
     * Using a per-CPU path prevents SMP races: each CPU writes only its own
     * TSS and cpu_t, so concurrent context switches on different CPUs are safe. */
    if (next->stack_base != NULL) {
        uint64_t kstack_top = (uint64_t)(next->stack_base + THREAD_STACK_SIZE);
        per_cpu_update_rsp0(kstack_top);
    }

    /* Switch address space if the next thread owns a different one (per-process
     * PML4, Phase 2). A kernel-only thread (idle) has pml4_phys==0 — leave
     * whatever CR3 is currently loaded, since it never touches user memory.
     * Clone threads in the same thread_group_t share one PML4, so switching
     * between them skips the reload (and the TLB flush that comes with it). */
    uint64_t next_pml4 = THREAD_PML4(next);
    if (next_pml4 != 0) {
        uint64_t cur_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if (next_pml4 != cur_cr3)
            __asm__ volatile("mov %0, %%cr3" :: "r"(next_pml4) : "memory");
    }

    context_switch_asm(&old->ctx, &next->ctx);
}

/**
 * sched_tick() - LAPIC timer ISR callback; advance the scheduler.
 *
 * Increments the global tick counter. Calls sched_schedule() to perform a
 * context switch when the current thread's quantum expires (or immediately
 * when a higher-priority thread becomes runnable). EOI is sent by the LAPIC
 * timer ISR before calling this function; the context switch is safe.
 *
 * Context: Must only be called from the LAPIC timer ISR with interrupts
 *          disabled via the interrupt gate mechanism. Must not allocate.
 */
void sched_tick(void)
{
    cpu_t *cpu = cpu_local();
    if (cpu->current_thread == NULL) return;  /* before first sched_init — skip */
    /* Guard against LAPIC timer firing on APs before sched_init_cpu() sets up
       the per-CPU run queue.  run_queue_head == NULL means sched_next() would
       dereference a null pointer (do-while loop on t->state with t=NULL). */
    if (cpu->run_queue_head == NULL) return;
    sched_tick_count++;
    cpu->current_thread->utime_ticks++;

    /* Check ITIMER_REAL expiry for all armed threads.
     * We scan all threads each tick — acceptable overhead for small thread counts. */
    for (int _i = 1; _i < SCHED_MAX_THREADS; _i++) {
        struct thread *_t = &thread_pool[_i];
        if (_t->itimer_real_active &&
            _t->itimer_real_deadline_ticks != 0 &&
            sched_tick_count >= _t->itimer_real_deadline_ticks) {
            /* Fire SIGALRM */
            sched_signal_thread(_t, SIGALRM);
            /* Reload interval or disarm */
            if (_t->itimer_real_interval_ticks != 0) {
                _t->itimer_real_deadline_ticks = sched_tick_count +
                                                 _t->itimer_real_interval_ticks;
            } else {
                _t->itimer_real_active = 0;
                _t->itimer_real_deadline_ticks = 0;
            }
        }
    }

    /* Only context-switch if there is actually a RUNNABLE thread waiting.
     * Without this guard, sched_next() would return the idle thread whenever
     * the current (running) user thread is in THREAD_RUNNING state, causing a
     * spurious switch to idle even though the user task is still alive. */
    if (sched_next() == cpu->idle_thread) return;
    sched_schedule();
}

/**
 * sched_yield() - Voluntarily yield to the next runnable thread.
 *
 * Disables interrupts around sched_schedule() to prevent the LAPIC timer
 * from firing inside context_switch_asm during a cooperative yield.
 *
 * Without this guard, the cooperative path (sched_yield from SYS_fork /
 * sched_wait) runs with IF=1 (sti was issued in syscall_entry before
 * syscall_dispatch).  If a timer fires after sched_schedule sets
 * current_thread=next but before context_switch_asm finishes restoring
 * the new thread's registers, the nested sched_schedule saves wrong
 * register values into next->ctx, corrupting the thread on re-schedule.
 *
 * The preemptive path (sched_tick from the timer ISR) is always called
 * with IF=0 (interrupt gates clear IF on entry), so it is not affected.
 */
void sched_yield(void)
{
    __asm__ volatile("cli" ::: "memory");
    sched_schedule();
    __asm__ volatile("sti" ::: "memory");
}

/**
 * sched_create_user_task() - Create a ring-3 user thread.
 * @entry_rip: User-mode entry point virtual address.
 *
 * Calls sched_create_thread() for the kernel scaffolding, then maps
 * USER_STACK_PAGES (64) 4 KiB pages into the user virtual address space
 * just below USER_STACK_TOP=0x7FFFFFFFE000 using vmm_map_page with
 * PAGE_PRESENT|PAGE_WRITE|PAGE_USER flags. Stores the physical base of the
 * first page in user_stack_phys_base for cleanup by sched_exit_current.
 * Sets up TSS RSP0 via tss_set_rsp0() so syscall entries use the thread's
 * kernel stack.
 *
 * @return: Pointer to the new user thread, or NULL on OOM.
 */
struct thread *sched_create_user_task(uint64_t entry_rip)
{
    struct thread *t = sched_create_thread(NULL, NULL);  /* grab a slot (BLOCKED) */
    if (!t) return NULL;
    /* t->state is already THREAD_BLOCKED from sched_create_thread; it stays that
     * way until the caller (sched_fork / sched_activate_task) sets it RUNNABLE
     * after the ctx is fully initialised, preventing any timer from scheduling
     * this thread while ctx.rip is still NULL. */

    /* Initialise POSIX identity fields for root tasks.
     * PID=TID at creation; root tasks have no parent and lead their own group/session. */
    t->pid  = t->tid;   /* PID equals TID at task creation */
    t->ppid = 0;        /* no parent for root tasks */
    t->pgid = t->tid;   /* own process group by default */
    t->sid  = t->tid;   /* own session by default */

    /* Every non-grouped task gets its own address space — this is what makes
     * fork's CoW real (parent and child have independent page tables sharing
     * frames only where explicitly arranged) instead of the old single-shared-
     * PML4 deferred-activation scheme. Clone threads never reach this path
     * (sched_clone uses sched_create_thread directly and shares the parent's
     * tg->pml4_phys), so this unconditional allocation is safe. */
    t->pml4_phys = vmm_new_address_space();
    if (!t->pml4_phys)
        return NULL;

    /* Allocate USER_STACK_PAGES physical frames and map them just below USER_STACK_TOP.
     * Guard page: leave the page at (USER_STACK_TOP - (USER_STACK_PAGES+1)*PAGE_SIZE) unmapped. */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (!phys) {
            /* OOM: unwind already-mapped pages */
            for (int j = i - 1; j >= 0; j--) {
                uint64_t virt = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - j) * PAGE_SIZE;
                vmm_unmap_page_in(t->pml4_phys, virt);
            }
            return NULL;
        }
        /* i=0 -> bottom page, i=USER_STACK_PAGES-1 -> top page just below USER_STACK_TOP */
        uint64_t virt = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PAGE_SIZE;
        vmm_map_page_in(t->pml4_phys, virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        if (i == 0)
            t->user_stack_phys_base = phys;  /* save bottom frame for cleanup */
    }
    t->user_stack_virt_top = USER_STACK_TOP;
    t->user_stack_pages    = USER_STACK_PAGES;

    /* Allocate mmap_regions on heap (fd_table already allocated by sched_create_thread) */
    t->mmap_regions = (mmap_region_t *)kmalloc(MMAP_MAX_REGIONS * sizeof(mmap_region_t));
    if (!t->mmap_regions)
        return NULL;
    memset(t->mmap_regions, 0, MMAP_MAX_REGIONS * sizeof(mmap_region_t));

    /* Initialise mmap cursor with a random page-aligned base (ASLR).
     * Offset 0-255 pages below MMAP_BASE keeps new allocations well clear of
     * the 0x700000000000 boundary while randomising library/mmap addresses. */
    t->mmap_next = MMAP_BASE - ((sched_rdtsc() >> 12) & 0xFFULL) * PAGE_SIZE;

    /* Initialise argument registers; set by SYS_execve, zero for all other paths. */
    t->saved_user_rdi = 0;
    t->saved_user_rsi = 0;

    /* Overwrite the initial context to start in ring 3.
     * After GDT reorder for SYSRET compatibility (04-02):
     *   USER_DS_INDEX=3 → selector 0x1B, USER_CS_INDEX=4 → selector 0x23
     * cs=0x23 (USER_CS_INDEX=4 << 3 | 3), ss=0x1B (USER_DS_INDEX=3 << 3 | 3)
     * rip=entry_rip, rsp=stack_top-8 (16-byte aligned), rflags=0x202 (IF=1)
     * All GPRs zeroed -- no kernel pointer leakage. */
    struct task_cpu_context *ctx = &t->ctx;
    ctx->rip    = entry_rip;
    ctx->cs     = USER_CS_SEL;   /* ring-3 code segment */
    ctx->ss     = USER_DS_SEL;   /* ring-3 data/stack segment */
    /* Randomise initial RSP within the mapped stack region (ASLR stack displacement).
     * Keeps 16-byte alignment required by the System V AMD64 ABI. */
    ctx->rsp    = USER_STACK_TOP - 16 - (sched_rdtsc() & 0xFFULL) * 16;
    ctx->rflags = RFLAGS_IF;
    /* Zero all GPRs */
    ctx->rax = ctx->rdx = ctx->rcx = ctx->rbx = ctx->rbp = 0;
    ctx->rsi = ctx->rdi = 0;
    ctx->r8  = ctx->r9  = ctx->r10 = ctx->r11 = 0;
    ctx->r12 = ctx->r13 = ctx->r14 = ctx->r15 = 0;

    /* Initialize working directory to root */
    t->cwd[0] = '/';
    t->cwd[1] = '\0';

    /* Set up stdin/stdout/stderr as TTY char device fd slots (ops=NULL).
     * This makes alloc_fd() skip 0-2 when they are open, and allows
     * close(0)+open("/dev/null") to reclaim fd=0 (POSIX lowest-fd rule). */
    for (int fd_i = 0; fd_i < VFS_FIRST_OPEN_FD; fd_i++) {
        t->fd_table[fd_i].in_use      = 1;
        t->fd_table[fd_i].ftype       = VFS_FILE_TYPE_CHAR;
        t->fd_table[fd_i].device_type = VFS_DEVICE_TTY;
        t->fd_table[fd_i].ops         = NULL;
    }

    printk("sched: created user task tid=%u rip=0x%llx\n", t->tid, entry_rip);

    return t;
}

/**
 * sched_activate_task() - Transfer "current" ownership to @t without context_switch_asm.
 * @t: Thread to activate. Must currently be in THREAD_BLOCKED state.
 *
 * Sets g_current->state = THREAD_RUNNABLE, then sets g_current = t and
 * t->state = THREAD_RUNNING. Used by kernel_main before enter_ring3() so
 * the user task is correctly identified as "current" when it issues its
 * first syscall or triggers a preemptive switch back to the idle thread.
 */
void sched_activate_task(struct thread *t)
{
    cpu_t *cpu = cpu_local();
    struct thread *old = cpu->current_thread;
    if (old != t) {
        old->state = THREAD_RUNNABLE;
        t->state   = THREAD_RUNNING;
        cpu->current_thread = t;
    }
}

/**
 * sched_exit_current() - Terminate the current thread and switch away.
 * @code: Exit code stored in thread->exit_code.
 *
 * Stores @code, sets state = THREAD_DEAD, frees this process's own address
 * space via vmm_free_address_space() (stack, code/data/BSS/heap, and mmap
 * regions all in one pass — CoW-shared frames are pmm_unref_frame()'d, so a
 * parent still using a shared page is unaffected). Wakes any THREAD_WAITING
 * parent by setting its state to THREAD_RUNNABLE, then calls
 * sched_schedule() — does not return.
 *
 * Context: Called from SYS_exit syscall handler. Does not return.
 */
void sched_exit_current(int code)
{
    struct thread *cur = sched_current();

    /* Close any sockets not explicitly closed before exit. Only close if this
     * thread owns its own fd_table (not in a thread group — the group's fds
     * persist until the last group member exits). */
    if (cur->tg == NULL)
        proc_close_all_sockets(cur);

    /* Free the mmap_regions/fd_table heap metadata (not page-table content —
     * vmm_free_address_space() below tears down the actual mappings/frames
     * for every VA this process/group owns, stack+code+heap+mmap alike, in
     * one pass). Skip for thread-group members — the group's copies are
     * freed once, when the last member exits (below). */
    if (cur->tg == NULL && cur->mmap_regions) {
        kfree(cur->mmap_regions);
        cur->mmap_regions = NULL;
    }
    if (cur->fd_table) {
        kfree(cur->fd_table);
        cur->fd_table = NULL;
    }

    cur->exit_code = code;

    /* clear-child-tid: notify futex waiters that this thread has exited */
    if (cur->clear_tid_addr) {
        *cur->clear_tid_addr = 0;
        /* FUTEX_WAKE 1: wake any thread waiting on clear_tid_addr */
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            if (thread_pool[i].state == THREAD_FUTEX_WAIT &&
                thread_pool[i].futex_uaddr == cur->clear_tid_addr) {
                thread_pool[i].futex_uaddr = NULL;
                thread_pool[i].state = THREAD_RUNNABLE;
                break;
            }
        }
        cur->clear_tid_addr = NULL;
    }

    /* thread_group cleanup: the group's address space is shared by every
     * member, so it's only torn down once, when the last member exits. */
    if (cur->tg) {
        unsigned long tg_flags;
        spinlock_irqsave(&cur->tg->lock, &tg_flags);
        cur->tg->ref_count--;
        int last_member = (cur->tg->ref_count == 0);
        uint64_t tg_pml4 = cur->tg->pml4_phys;
        if (last_member) {
            kfree(cur->tg->fd_table);
            cur->tg->fd_table = NULL;
            kfree(cur->tg->mmap_regions);
            cur->tg->mmap_regions = NULL;
            cur->tg->in_use = 0;
        }
        spinlock_irqrestore(&cur->tg->lock, tg_flags);
        cur->tg = NULL;
        if (last_member)
            vmm_free_address_space(tg_pml4);
    } else if (cur->pml4_phys) {
        /* Single-threaded process: this thread solely owns its address
         * space (stack, code/data/BSS/heap, mmap regions — all of it). */
        vmm_free_address_space(cur->pml4_phys);
        cur->pml4_phys = 0;
    }

    /* Acquire thread_pool_lock with IRQs disabled for the critical section:
     * setting THREAD_DEAD, waking the parent, and calling sched_schedule()
     * must be atomic with respect to the LAPIC timer ISR.
     *
     * Race without lock: if a timer fires after cur->state = THREAD_DEAD but
     * before the parent-wakeup loop runs, the nested sched_schedule() (from
     * sched_tick) sees cur=DEAD and parent=WAITING (no RUNNABLE threads),
     * falls back to idle, and switches away permanently.  The parent-wakeup
     * code never runs, leaving the parent WAITING forever (deadlock). */
    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    cur->state = THREAD_DEAD;
    printk("task %u exited with code %d\n", cur->tid, code);

    /* Wake parent if it is blocked in sched_wait() or SYS_fork's sched_yield() */
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].tid == cur->parent_tid &&
            thread_pool[i].state == THREAD_WAITING) {
            thread_pool[i].state = THREAD_RUNNABLE;
            break;
        }
    }

    /* Deliver SIGCHLD to parent unless parent set SIG_IGN or SA_NOCLDWAIT.
     * Done inline (no sched_signal_thread) since thread_pool_lock is already held. */
    if (cur->parent_tid != 0) {
        for (int i = 0; i < SCHED_MAX_THREADS; i++) {
            struct thread *p = &thread_pool[i];
            if (p->tid != cur->parent_tid || p->state == THREAD_DEAD)
                continue;
            mini_sigaction_t *act = &p->signal_actions[SIGCHLD];
            int ignored = (act->sa_handler == SIG_IGN) ||
                          (act->sa_flags & (int)MINIOS_SA_NOCLDWAIT);
            if (!ignored) {
                p->pending_signals |= (1u << SIGCHLD);
                if (p->state == THREAD_WAITING || p->state == THREAD_FUTEX_WAIT)
                    p->state = THREAD_RUNNABLE;
            }
            break;
        }
    }
    spinlock_irqrestore(&thread_pool_lock, flags);
    /* Disable IRQs before scheduling so no timer/IPI fires between here and
     * context_switch_asm — mirrors the invariant in sched_yield(). */
    __asm__ volatile("cli" ::: "memory");
    sched_schedule();  /* pick next thread; does not return to this thread */
}

/**
 * sched_fork() - Fork the calling user task.
 *
 * Allocates a child user task via sched_create_user_task(), which gives it
 * its own address space (vmm_new_address_space()) with a fresh user stack
 * already mapped in. Copies the parent's stack content into it directly via
 * the KERNEL_VMA direct-map alias, then CoW-shares the code/data/BSS/heap
 * and mmap regions (fork_cow_share_page()) — real fork() semantics, no
 * deferred activation. Sets child's saved_user_* registers from the
 * parent's saved_user_* and wires child's ctx.rip = fork_child_trampoline.
 *
 * Context: Called from SYS_fork syscall handler. Panics via printk+hlt on OOM.
 * @return: Pointer to the child thread (caller is the parent).
 */
/* Share a page — currently mapped in the parent's live address space — with
 * the fork child's new address space. Read-only pages (text/rodata, or a
 * page already CoW'd from an earlier fork generation) are aliased directly:
 * nobody will ever write them without going through the CoW fault path
 * first, so the existing PAGE_COW bit (0 for genuine rodata, 1 if inherited)
 * is simply preserved. Writable pages become CoW-shared: both the parent's
 * own mapping and the child's new mapping lose PAGE_WRITE and gain
 * PAGE_COW, so the first writer on *either* side privately copies via
 * vmm_resolve_user_fault(). Frames not yet faulted in (lazy mmap/brk) are
 * left alone — whichever side touches them first demand-pages independently,
 * which is equivalent to sharing-then-copying since both start from the
 * same (zero or file-backed) content. No-op if the page isn't mapped. */
static void fork_cow_share_page(struct thread *child, uint64_t virt)
{
    uint64_t pte = vmm_virt_to_pte(virt);   /* parent's live PML4 */
    if (!(pte & PAGE_PRESENT))
        return;

    uint64_t phys = pte & PTE_ADDR_MASK;
    uint64_t base = PAGE_PRESENT | PAGE_USER;

    if (pte & PAGE_WRITE) {
        vmm_map_page(virt, phys, base | PAGE_COW);                       /* parent */
        vmm_map_page_in(child->pml4_phys, virt, phys, base | PAGE_COW);  /* child */
    } else {
        vmm_map_page_in(child->pml4_phys, virt, phys, base | (pte & PAGE_COW));
    }
    pmm_ref_frame(phys);
}

struct thread *sched_fork(void) {
    struct thread *parent = sched_current();

    /* sched_create_user_task() gives the child its own address space
     * (vmm_new_address_space()) with a fresh, zeroed user stack already
     * mapped into it — real per-process page tables, so there is no shared
     * VA to fight over with the parent. */
    struct thread *child = sched_create_user_task(0);
    if (!child) {
        printk("sched_fork: OOM — no free thread slot\n");
        for (;;) __asm__("hlt");
    }

    /* Copy parent's stack content into the child's own fresh frames via the
     * KERNEL_VMA direct-map alias on both sides — no VA remapping needed,
     * parent and child each have their own page tables now. */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t virt  = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PAGE_SIZE;
        uint64_t pphys = vmm_virt_to_phys(virt);
        uint64_t cphys = vmm_virt_to_phys_in(child->pml4_phys, virt);
        if (pphys && cphys)
            memcpy((void *)(cphys + KERNEL_VMA), (void *)(pphys + KERNEL_VMA), PAGE_SIZE);
    }

    /* Set up child's kernel-side context to enter fork_child_trampoline.
     * fork_child_trampoline reads ctx.rcx (user RIP), ctx.r11 (user RFLAGS),
     * and saved_user_rsp from the child thread, then does sysretq. */
    child->ctx.rip    = (uint64_t)fork_child_trampoline;
    child->ctx.rsp    = (uint64_t)(child->stack_base + THREAD_STACK_SIZE - 8);
    child->ctx.cs     = KERNEL_CS_SEL;  /* kernel CS — trampoline runs in ring 0 */
    child->ctx.ss     = KERNEL_SS_SEL;
    child->ctx.rflags = RFLAGS_IF;     /* IF=1 */

    /* Inherit parent's user FS.base (TLS pointer).  SYSCALL does not change FS.base,
     * so rdfsbase here still returns the parent's user TLS address. The trampoline
     * calls wrfsbase with this value before SYSRET so the child starts with valid TLS. */
    uint64_t parent_fs_base;
    __asm__ volatile("rdfsbase %0" : "=r"(parent_fs_base));
    child->ctx.fs_base = parent_fs_base;

    /* Copy user-return registers from parent (saved by syscall_save_user_regs).
     * Use saved_user_rip/saved_user_rfl — NOT ctx.rcx/ctx.r11.
     * context_switch_asm overwrites ctx.rcx with the kernel RCX value at the time
     * of a preemptive context switch, so ctx.rcx is NOT safe to use as user RIP. */
    child->saved_user_rip = parent->saved_user_rip;  /* user RIP (sysretq target) */
    child->saved_user_rfl = parent->saved_user_rfl;  /* user RFLAGS */

    /* Callee-saved user registers: fork child must see the same values as parent
     * so RBP-relative locals (e.g. shell's path[] array) resolve correctly. */
    child->saved_user_rbp = parent->saved_user_rbp;
    child->saved_user_rbx = parent->saved_user_rbx;
    child->saved_user_r12 = parent->saved_user_r12;
    child->saved_user_r13 = parent->saved_user_r13;
    child->saved_user_r14 = parent->saved_user_r14;
    child->saved_user_r15 = parent->saved_user_r15;

    /* User RSP: same offset into the stack as parent */
    uint64_t rsp_offset  = USER_STACK_TOP - parent->saved_user_rsp;
    child->saved_user_rsp = USER_STACK_TOP - rsp_offset;

    /* CoW-share code/data/BSS/heap (0x400000..parent->brk) — parent's own
     * mapping is only touched for pages that were writable (they lose
     * PAGE_WRITE and gain PAGE_COW on both sides); the parent is otherwise
     * completely unaffected by the child's existence, unlike the old
     * single-shared-PML4 scheme. */
    if (*THREAD_BRK_PTR(parent) > 0x400000ULL) {
        uint64_t code_start = 0x400000ULL;
        uint64_t code_end   = *THREAD_BRK_PTR(parent);
        for (uint64_t virt = code_start; virt < code_end; virt += PAGE_SIZE)
            fork_cow_share_page(child, virt);
    }

    /* CoW-share already-materialized mmap region pages the same way; copy
     * region metadata unconditionally so a page nobody has faulted in yet
     * is demand-paged independently by whichever side touches it first —
     * equivalent to sharing-then-copying since both start from the same
     * zero/file-backed content. */
    {
        mmap_region_t *pmmap = THREAD_MMAPR(parent);
        mmap_region_t *cmmap = THREAD_MMAPR(child);
        for (int mi = 0; mi < MMAP_MAX_REGIONS; mi++) {
            if (pmmap[mi].virt_addr == 0) continue;
            cmmap[mi] = pmmap[mi];
            uint64_t rb = pmmap[mi].virt_addr;
            uint64_t rl = (pmmap[mi].len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
            for (uint64_t virt = rb; virt < rb + rl; virt += PAGE_SIZE)
                fork_cow_share_page(child, virt);
        }
    }

    /* Inherit parent's heap break so child's first brk() continues from the
     * correct address rather than triggering the 0x602000 fallback. */
    child->brk      = *THREAD_BRK_PTR(parent);
    child->brk_base = *THREAD_BRK_BASE_PTR(parent);

    /* Inherit parent's mmap cursor so child's exec allocates below parent's
     * existing mmap regions (e.g. TLS).  Without this the child resets to
     * MMAP_BASE and maps TLS at the same VA as the parent's TLS, destroying it. */
    child->mmap_next = *THREAD_MMAP_PTR(parent);

    child->parent_tid = parent->tid;
    child->pid  = child->tid;     /* unique PID = unique TID */
    child->ppid = parent->pid;    /* child's parent is the forking process */
    child->pgid = parent->pgid;   /* inherit parent's process group */
    child->sid  = parent->sid;    /* inherit parent's session */

    /* Inherit signal actions from parent */
    for (int i = 0; i < 32; i++) {
        child->signal_actions[i] = parent->signal_actions[i];
    }

    /* Parent and child now have independent address spaces (including
     * independent stacks), so there's no shared-VA hazard between fork()
     * returning in the parent and the child running — real POSIX fork()
     * semantics. The child can run immediately; no deferred activation. */
    child->state      = THREAD_RUNNABLE;

    /* Inherit open file descriptors from parent (POSIX fork semantics).
     * Copy from THREAD_FDT(parent) since parent may be in a thread group.
     * Pipe and unix socket ref_counts are incremented for the new child. */
    vfs_file_t *parent_fdt = THREAD_FDT(parent);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        child->fd_table[i] = parent_fdt[i];
        if (!child->fd_table[i].in_use) continue;
        if (child->fd_table[i].ftype == VFS_FILE_TYPE_PIPE &&
            child->fd_table[i].pipe) {
            child->fd_table[i].pipe->ref_count++;
        } else if (child->fd_table[i].ftype == VFS_FILE_TYPE_UNIX_SOCKET &&
                   child->fd_table[i].usock) {
            child->fd_table[i].usock->ref_count++;
        }
    }

    /* Inherit parent's working directory */
    strncpy(child->cwd, THREAD_CWD(parent), VFS_PATH_MAX - 1);
    child->cwd[VFS_PATH_MAX - 1] = '\0';

    printk("sched_fork: child tid=%u created from parent tid=%u\n",
           child->tid, parent->tid);
    return child;
}

/**
 * sched_wait() - Block until child thread @child_tid exits.
 * @child_tid: TID of the child to wait for.
 * @exit_code: Output; written with child->exit_code when child exits.
 *             May be NULL.
 *
 * Spins (yielding) until child->state == THREAD_DEAD. Sets calling thread
 * to THREAD_WAITING and calls sched_yield() in a loop to avoid burning CPU.
 * Validates that @child_tid exists in thread_pool and that parent_tid matches.
 *
 * @return: 0 on success, -1 if @child_tid not found or not a child of caller.
 */
int sched_wait(uint32_t child_tid, int *exit_code) {
    struct thread *child = NULL;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].tid == child_tid &&
            thread_pool[i].parent_tid == sched_current()->tid) {
            child = &thread_pool[i];
            break;
        }
    }
    if (!child) return -1;

    if (child->state != THREAD_DEAD) {
        /* Block until child exits.  sched_exit_current() wakes us. */
        sched_current()->state = THREAD_WAITING;
        while (child->state != THREAD_DEAD)
            sched_yield();
        sched_current()->state = THREAD_RUNNING;
    }

    if (exit_code) *exit_code = child->exit_code;
    return 0;
}

/**
 * sched_init_cpu() - Create the per-CPU idle thread and assign to g_cpus[cpu_id].
 * @cpu_id: Logical CPU index (0=BSP, 1..N-1=APs).
 *
 * Called once per AP during boot from kernel_main after smp_wait_for_aps().
 * Uses sched_create_thread(idle_loop, NULL) to allocate a thread slot and a
 * dedicated 16 KiB kernel stack, then sets state=THREAD_RUNNABLE (APs will
 * pick it up when the LAPIC timer fires and sched_next falls back to idle).
 *
 * Note: BSP idle is already set up by sched_init() via idle_thread_storage.
 * This function handles cpu_id > 0 (APs). Calling it for cpu_id == 0 is a
 * no-op if g_cpus[0].idle_thread is already set.
 *
 * The idle loop halts the CPU until the next LAPIC timer interrupt, at which
 * point sched_tick picks a runnable thread from the shared run queue.
 */
void sched_init_cpu(uint32_t cpu_id) {
    /* BSP idle thread: wire idle_thread_storage to g_cpus[0].idle_thread.
     * The per-CPU queue for CPU 0 was already initialized in sched_init(). */
    if (cpu_id == 0) {
        g_cpus[0].idle_thread = &idle_thread_storage;
        printk("sched: CPU 0 (BSP) idle thread assigned (tid=%u)\n",
               idle_thread_storage.tid);
        return;
    }

    /* AP: allocate a dedicated kernel stack and create an idle thread.
     * sched_create_thread() is NOT used here because it calls
     * sched_balance_enqueue() which appends the new thread to BSP's circular
     * queue.  When we then set idle->next = idle below, it breaks BSP's
     * circular invariant (idle0->next pointed to idle, now idle->next = idle
     * making BSP's list loop on idle forever).
     * Instead, allocate and initialize the thread slot directly. */
    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (!stack) {
        printk("sched_init_cpu: OOM for CPU %u stack\n", cpu_id);
        for (;;) __asm__ volatile("cli; hlt");
    }

    unsigned long flags;
    spinlock_irqsave(&thread_pool_lock, &flags);
    if (thread_count >= SCHED_MAX_THREADS) {
        spinlock_irqrestore(&thread_pool_lock, flags);
        kfree(stack);
        printk("sched_init_cpu: thread pool full for CPU %u\n", cpu_id);
        for (;;) __asm__ volatile("cli; hlt");
    }
    struct thread *idle = &thread_pool[thread_count++];
    memset(idle, 0, sizeof(*idle));
    idle->tid        = next_tid++;
    idle->state      = THREAD_RUNNING;  /* current on this CPU — must be RUNNING so sched_next skips it */
    idle->stack_base = stack;
    idle->ctx.rip    = (uint64_t)idle_loop;
    idle->ctx.rsp    = (uint64_t)(stack + THREAD_STACK_SIZE - 8);
    idle->ctx.rdi    = 0;
    idle->ctx.cs     = KERNEL_CS_SEL;
    idle->ctx.ss     = KERNEL_SS_SEL;
    idle->ctx.rflags = RFLAGS_IF;
    *(uint64_t *)(stack + THREAD_STACK_SIZE - 8) = 0;  /* null return address */
    spinlock_irqrestore(&thread_pool_lock, flags);

    /* Assign to per-CPU slot */
    g_cpus[cpu_id].idle_thread    = idle;
    g_cpus[cpu_id].current_thread = idle;

    /* Initialize per-CPU run queue: single-element circular list (idle only). */
    idle->next                    = idle;  /* circular: points to itself */
    g_cpus[cpu_id].run_queue_head = idle;
    g_cpus[cpu_id].run_queue_tail = idle;
    g_cpus[cpu_id].queue_depth    = 0;

    printk("sched: CPU %u idle thread created (tid=%u)\n", cpu_id, idle->tid);
}

/**
 * sched_clone() - Create a new thread sharing the caller's address space (pthreads).
 */
struct thread *sched_clone(uint64_t child_stack, uint64_t tls, uint32_t *child_tid_ptr)
{
    struct thread *parent = sched_current();

    /* Allocate or reuse thread group */
    if (!parent->tg) {
        thread_group_t *g = NULL;
        for (int i = 0; i < THREAD_GROUP_MAX; i++) {
            if (!thread_group_pool[i].in_use) {
                g = &thread_group_pool[i];
                break;
            }
        }
        if (!g) return NULL;
        memset(g, 0, sizeof(*g));
        g->in_use = 1;
        g->lock = (spinlock_t)SPINLOCK_INIT;
        /* Migrate parent's fd_table, brk, mmap_regions, cwd into group */
        g->fd_table = (vfs_file_t *)kmalloc(VFS_MAX_FDS * sizeof(vfs_file_t));
        g->mmap_regions = (mmap_region_t *)kmalloc(MMAP_MAX_REGIONS * sizeof(mmap_region_t));
        if (!g->fd_table || !g->mmap_regions) {
            if (g->fd_table) kfree(g->fd_table);
            if (g->mmap_regions) kfree(g->mmap_regions);
            return NULL;
        }
        memcpy(g->fd_table, parent->fd_table, VFS_MAX_FDS * sizeof(vfs_file_t));
        g->brk      = parent->brk;
        g->brk_base = parent->brk_base;
        g->mmap_next = parent->mmap_next;
        memcpy(g->mmap_regions, parent->mmap_regions, MMAP_MAX_REGIONS * sizeof(mmap_region_t));
        memcpy(g->cwd, parent->cwd, VFS_PATH_MAX);
        g->pml4_phys = parent->pml4_phys;  /* clone threads share the address space */
        g->ref_count = 1;
        parent->tg = g;
        parent->tgid = parent->pid;
        /* Parent's per-thread buffers superseded by group buffers */
        kfree(parent->fd_table);
        parent->fd_table = NULL;
        kfree(parent->mmap_regions);
        parent->mmap_regions = NULL;
    }

    struct thread *child = sched_create_thread(NULL, NULL);
    if (!child) return NULL;

    /* Override ctx.rip to fork_child_trampoline — same mechanism as sched_fork */
    child->ctx.rip = (uint64_t)fork_child_trampoline;
    child->ctx.rsp = (uint64_t)(child->stack_base + THREAD_STACK_SIZE - 8);
    child->ctx.cs  = KERNEL_CS_SEL;
    child->ctx.ss  = KERNEL_SS_SEL;
    child->ctx.rflags = RFLAGS_IF;

    /* Copy parent's saved user context: child resumes at same RIP (clone() return) */
    child->saved_user_rip = parent->saved_user_rip;
    child->saved_user_rfl = parent->saved_user_rfl;
    child->saved_user_rsp = child_stack;  /* new stack */
    child->saved_user_rbp = parent->saved_user_rbp;
    child->saved_user_rbx = parent->saved_user_rbx;
    child->saved_user_r12 = parent->saved_user_r12;
    child->saved_user_r13 = parent->saved_user_r13;
    child->saved_user_r14 = parent->saved_user_r14;
    child->saved_user_r15 = parent->saved_user_r15;
    child->saved_user_rdi = 0;   /* clone() returns 0 in child */

    /* TLS — use live rdfsbase; ctx.fs_base may be stale if SYSCALL didn't update it */
    if (tls) {
        child->ctx.fs_base = tls;
    } else {
        uint64_t cur_fs_base;
        __asm__ volatile("rdfsbase %0" : "=r"(cur_fs_base));
        child->ctx.fs_base = cur_fs_base;
    }

    /* Join parent's thread group */
    unsigned long flags;
    spinlock_irqsave(&parent->tg->lock, &flags);
    parent->tg->ref_count++;
    spinlock_irqrestore(&parent->tg->lock, flags);
    child->tg         = parent->tg;
    child->tgid       = parent->tgid;
    child->pid        = parent->pid;    /* same process */
    child->ppid       = parent->ppid;
    child->pgid       = parent->pgid;
    child->sid        = parent->sid;
    child->parent_tid = parent->tid;    /* so sched_exit_current can wake joiner */

    /* Copy parent's signal actions */
    memcpy(child->signal_actions, parent->signal_actions, sizeof(parent->signal_actions));

    /* TID for child */
    if (child_tid_ptr)
        *child_tid_ptr = child->tid;

    child->state = THREAD_RUNNABLE;
    sched_balance_enqueue(child);
    return child;
}
