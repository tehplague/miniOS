# Scheduler in miniOS

The miniOS scheduler is a preemptive, round-robin scheduler responsible for managing threads and switching between them. It forms the core of the operating system's multitasking capabilities.

## Scheduling Algorithm

- **Round-Robin**: The scheduler uses a simple round-robin algorithm. All runnable threads are kept in a queue, and each thread is given a fixed time slice to execute.
- **Preemption**: Preemption is timer-based. The local APIC (LAPIC) timer is configured to fire an interrupt periodically. The timer interrupt handler triggers a context switch, ensuring that no single thread can monopolize the CPU.

## Context Switching

The context switch is the mechanism for saving the state of the currently running thread and restoring the state of the next thread.

- **`context_switch_asm`**: The context switch is implemented in assembly and is designed to be called directly from C code. It saves all general-purpose registers, the instruction pointer (`rip`), and the stack pointer (`rsp`).
- **User Context**: When switching from a user-mode thread, the `SYSCALL`/`SYSRET` instruction pair has specific register clobbering behavior. To handle this, `saved_user_rip` and `saved_user_rflags` are stored separately in `struct thread` to ensure a safe return to userspace.

## Process and Thread Management

- **`fork()`**: The `SYS_fork` syscall creates a new child process. To avoid complex kernel stack duplication, the child process begins execution at a special assembly trampoline, `fork_child_trampoline`.
- **`fork_child_trampoline`**: This trampoline prepares the child's kernel stack and returns to userspace via `sysretq`, using the context saved from the parent's `fork()` call. This ensures the child process starts executing at the instruction following the `fork()` in the parent's code.
- **`exec()`**: The `SYS_execve` syscall loads and runs a new program by replacing the current process's memory image with a new one loaded from an ELF binary.
- **`wait()`**: The `SYS_waitpid`/`SYS_wait4` syscalls allow a parent process to wait for a child process to terminate.

## Per-Process Address Spaces and Fork CoW

Every process has its own PML4 (`vmm_new_address_space()`, `include/miniOS/mm/vmm.h`),
allocated once when the task is created (`sched_create_user_task()`) and stored in
`struct thread.pml4_phys` (or `thread_group_t.pml4_phys` for clone threads — see below;
resolved via the `THREAD_PML4(t)` macro). The kernel high half (canonical PML4 indices
256–511: kernel image, heap, DMA bounce buffer, GOP framebuffer, LAPIC/IOAPIC MMIO) is
copied **by value** into every new PML4 so kernel mappings are identical regardless of
which process's table is loaded in CR3 — all processes share the same underlying
PDPT/PD/PT chain for that half, so kernel-side growth (e.g. the heap mapping a new page)
is automatically visible everywhere without re-syncing. Only the low half (indices
0–255: user code/stack/heap/mmap) differs per process.

`sched_schedule()` reloads CR3 with the next thread's `THREAD_PML4(next)` only when it
differs from the currently-loaded value — this skips the reload (and TLB flush) when
switching between clone threads in the same `thread_group_t`, which already share one
PML4. A kernel-only thread (idle) has `pml4_phys == 0` and is skipped entirely; whatever
CR3 was already loaded is left alone, since idle never touches user memory.

### Real CoW via refcounted frames

`sched_fork()` gives the child its own address space and copies the parent's user stack
content directly (both sides are independently mapped, so no VA sharing/swapping is
needed). Code/data/BSS/heap and mmap-region pages are shared via
`fork_cow_share_page()` (`src/kernel/sched/sched.c`):

- **Read-only pages** (`.text`/`.rodata`, or a page already CoW'd from an earlier fork
  generation): mapped into the child's PML4 with the same permissions — nobody will
  ever write them without going through the CoW fault path first.
- **Writable pages**: *both* the parent's own mapping and the child's new mapping lose
  `PAGE_WRITE` and gain `PAGE_COW` (bit 9). The first writer on **either side** privately
  copies via `vmm_resolve_user_fault()`, which allocates a fresh frame, copies the
  content, remaps its own PTE writable, and drops its share of the original
  (`pmm_unref_frame()`).

Every shared frame is refcounted (`pmm_ref_frame()`/`pmm_unref_frame()`,
`src/kernel/mm/pmm.c`) — a frame is only actually returned to the free pool when its
count reaches zero, so a parent and any number of forked children (or further
generations) can share one physical frame safely; whichever side writes first peels off
its own private copy, and process exit (`vmm_free_address_space()`) or `execve()`
correctly drops just that process's share.

Because the parent's own address space is never touched by the child's existence, the
child is **`THREAD_RUNNABLE` immediately** after `sched_fork()` returns — no deferred
activation, no `THREAD_BLOCKED`-until-`waitpid()` dance. This matches real POSIX `fork()`
semantics (parent and child genuinely run concurrently) and also fixed a documented
known limitation (no true concurrent process execution).

### Process exit and exec

`sched_exit_current()` tears down a process's address space once (for a
single-threaded process) or when the last member of a `thread_group_t` exits (shared
address space) — but under SMP it must not call `vmm_free_address_space(pml4_phys)`
directly: that PML4 is still this CPU's own live `CR3` at this point (the actual
switch-away happens later, inside the `sched_schedule()` call at the end of this same
function). Freeing it here would return its physical frames — the PML4 itself and
every PDPT/PD/PT frame beneath it — to the pool while this CPU is still translating
every memory access through them; another CPU's concurrent `pmm_alloc_frame()` can
then hand one of those frames to `vmm_new_address_space()`, whose `memset()` zeroes it
for a completely unrelated new process, corrupting this CPU's live page tables (most
visibly the shared kernel-half mapping) out from under it — a real, non-hypothetical
SMP triple-fault chased down live via GDB against the QEMU stub (kernel `.text` pages
becoming instruction-fetch-not-present on the CPU that hadn't switched CR3 away yet).

Instead, `sched_exit_current()` stashes the doomed PML4 in
`cpu_local()->pending_free_pml4` (`include/miniOS/arch/x86_64/smp.h`), and
`sched_schedule()` frees it — via `vmm_free_address_space()` — right after confirming
this CPU's `CR3` has actually moved off of it. If the next thread scheduled is idle
(`pml4_phys==0`, CR3 deliberately left untouched — see below), the free is correctly
deferred to whichever later call first changes this CPU's CR3 away from the pending
PML4, rather than freed while it's still live.

`vmm_free_address_space()` itself walks the process's own low half,
`pmm_unref_frame()`-ing every mapped page — private pages are freed, still-shared CoW
pages are just decremented, leaving any sibling process's reference intact — then
frees the intermediate PDPT/PD/PT frames and the PML4 itself.

`sys_execve()` no longer special-cases "is this a fork child" at all: it just tears
down (`pmm_unref_frame()`) and rebuilds *this process's own* PML4, since CoW-shared
frames are safely refcounted regardless of lineage.

**Execve failure safety**: the old image is unmapped *before* the new ELF is loaded
(otherwise already-present pages at the same fixed VAs would be silently kept instead of
overwritten). If `elf_load()` then fails — truncated read, corrupt ELF, a disk I/O
error — there is no valid old image left to return an error into; `sys_execve()` kills
the process via `sched_exit_current()` instead of resuming execution on unmapped or
half-written memory.

## Clone Threads and Thread Groups

`clone(CLONE_VM|CLONE_FILES|CLONE_THREAD, child_stack, ..., child_tid, tls)` creates a new
thread that shares the caller's address space. The kernel path is `SYS_clone` →
`sched_clone()`.

### Thread group (`thread_group_t`)

On the first `clone()` call the parent thread's per-thread resources are migrated into a
heap-allocated `thread_group_t` (from a pool of `THREAD_GROUP_MAX` static slots):

| Field | Shared resource |
|-------|----------------|
| `*fd_table` | Open file descriptors (heap-allocated, `VFS_MAX_FDS` entries) |
| `brk` / `mmap_next` / `*mmap_regions` | Heap and mmap state (mmap_regions heap-allocated) |
| `cwd[]` | Current working directory |

All threads in the group access these via the `THREAD_FDT(t)`, `THREAD_BRK(t)`, etc.
macros, which route through `t->tg` when non-NULL and fall back to `t->fd_table` / fields
for single-threaded processes. **Every VFS operation that takes a file descriptor must use
`THREAD_FDT(t)[fd]`**, not `t->fd_table[fd]` directly; bypassing it makes clone threads
see an empty private fd table.

### TLS

`sched_clone()` sets the child's `ctx.fs_base` from:
1. The explicit `tls` argument if non-zero (mlibc pthread path), or
2. The live `rdfsbase` value of the calling thread — **not** `parent->ctx.fs_base`, which
   may be stale because `SYSCALL` does not update the saved context.

`fork_child_trampoline` restores FS.base via `wrfsbase` before `sysretq` so the child
enters userspace with the correct TLS pointer.

### Clone child entry and exit

`sched_clone()` wires `child->ctx.rip = fork_child_trampoline` — the same trampoline used
by `sched_fork()`. The child inherits the parent's saved user registers
(`saved_user_rip`, etc.) so it returns from `clone()` at the same instruction as the
parent, but with RAX = 0 and RSP = `child_stack`.

On exit the child calls `_exit()` → `sched_exit_current()`. If `CLONE_CHILD_CLEARTID` was
set, the kernel writes 0 to `clear_child_tid` and does a `futex(WAKE)` on that address
before marking the thread DEAD, allowing the joining thread to wait with
`futex(WAIT, clear_child_tid, saved_tid)`.

## Job Control: SIGSTOP / SIGCONT

`THREAD_STOPPED` is a distinct scheduler state (separate from `THREAD_WAITING`/
`THREAD_BLOCKED`/`THREAD_FUTEX_WAIT`) for a thread paused by `SIGSTOP` or `SIGTSTP`.

- **Stopping**: `signal_dispatch()` (`src/kernel/signal.c`) special-cases `SIGSTOP`
  (always — cannot be caught or ignored, per POSIX) and `SIGTSTP` (only on the default
  disposition). It calls `sched_stop_current()`, which sets the thread's state to
  `THREAD_STOPPED` and wakes any parent blocked in `THREAD_WAITING` (so a
  `wait4(WUNTRACED)` call returns as soon as the child stops, not just on exit), then
  loops `sched_yield()` until resumed. `sched_schedule()`'s "reset to RUNNABLE unless..."
  guard explicitly excludes `THREAD_STOPPED` so a preemption between "set STOPPED" and
  the actual context switch away can't silently un-stop the thread.
- **Resuming**: `SIGCONT` is handled directly in `sched_signal_thread()` — a
  `THREAD_STOPPED` thread isn't running, so it can never reach `signal_dispatch()` to
  process its own pending-signal bitmask the way every other signal does. If the target
  is `THREAD_STOPPED`, `SIGCONT` transitions it straight to `THREAD_RUNNABLE` instead of
  being queued.
- **`wait4`/`waitpid`**: implement `WNOHANG` (return 0 immediately if no child status is
  available rather than blocking — important once a child can sit in `THREAD_STOPPED`
  indefinitely, since a caller polling without `WNOHANG` would otherwise hang forever
  waiting for a stop-only child to *exit*) and `WUNTRACED` (report — without reaping — a
  `THREAD_STOPPED` child). The status word matches the real Linux ABI
  (`WIFEXITED`/`WIFSIGNALED`/`WIFSTOPPED`/`WEXITSTATUS`/`WTERMSIG`/`WSTOPSIG`) instead of
  a bare `exit_code << 8`.

## Futex

`SYS_futex` (nr 202) implements `FUTEX_WAIT` and `FUTEX_WAKE` (with the `FUTEX_PRIVATE`
flag silently stripped — matching is always by raw virtual address, and since a futex
only makes sense between threads sharing an address space — either clone threads in one
`thread_group_t`, or `MAP_SHARED`, which miniOS doesn't implement yet — the private/shared
distinction doesn't currently change any behavior).

`FUTEX_WAIT`: if `*uaddr != val` returns `-EAGAIN` immediately; otherwise sets
`ft->state = THREAD_FUTEX_WAIT` and `ft->futex_uaddr = uaddr`, then loops on
`sched_yield()` until state changes.

`FUTEX_WAKE`: scans the thread pool for threads in `THREAD_FUTEX_WAIT` with matching
`futex_uaddr`, sets up to `val` of them to `THREAD_RUNNABLE`.

## Thread Exit Safety

`sched_exit_current()` follows this protocol to prevent scheduler races:

1. `spinlock_irqsave(&thread_pool_lock)` — acquire with IRQs disabled.
2. `cur->state = THREAD_DEAD` — mark dead while holding the lock.
3. Walk thread pool to wake any `THREAD_WAITING` parent.
4. `spinlock_irqrestore()` — releases the lock and **re-enables IRQs**.
5. `cli` — immediately **re-disable IRQs** before the final schedule.
6. `sched_schedule()` — pick next thread; never returns in the dying thread.

Step 5 is essential. Without it, a LAPIC timer or sched-kick IPI can fire between steps 4
and 6 on the dying thread's kernel stack. `sched_schedule()` would be called from the ISR
(with the dead thread as `old`), save the dead thread's context, and switch to another
thread. The dead thread's own `sched_schedule()` call would then never run — but on some
paths the interrupt's saved interrupt frame (pushed by the CPU when the ISR fired) would
remain on the dead thread's kernel stack and never be popped, potentially causing a GP
if the thread were ever resumed (which it is not, but the invariant keeps the path
predictable and testable).

`sched_yield()` maintains the same invariant: it disables IRQs with `cli` before calling
`sched_schedule()` and re-enables with `sti` on return.

## SMP Scheduling: Per-CPU Run Queues and Work-Stealing

Each CPU has an independent run queue stored in its `cpu_t` struct. The scheduler provides three mechanisms:

### Per-CPU Run Queues

`sched_tick()` dequeues exclusively from the calling CPU's run queue. The idle
thread is a permanent sentinel in each queue (never dequeued). `queue_depth`
counts only non-idle RUNNABLE threads and serves as the load signal.

### Load Balancing on Fork/Exec

`sched_balance_enqueue()` is called when a new thread is created via
`sched_create_thread()`. It scans all `g_cpus[]` entries for the minimum
`queue_depth` and tail-inserts the new thread there. If the target CPU is not
the calling CPU, a scheduler-kick IPI (vector 50) is sent via `lapic_send_ipi()`
to wake the target from its `hlt` instruction.

### Work-Stealing

When a CPU's run queue empties and its idle thread runs, `sched_work_steal()` is
called with interrupts enabled. It:

1. Acquires `thread_pool_lock`.
2. Finds the CPU with `queue_depth > 1` (threshold prevents thrashing on single-thread victim).
3. Steals `floor(queue_depth / 2)` RUNNABLE threads (unlinks them from victim's queue,
   appends to caller's queue).
4. Releases the lock.

The threshold `> 1` ensures a victim CPU always retains at least one real thread,
preventing ping-pong migration.
