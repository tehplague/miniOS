# SMP in miniOS

miniOS supports symmetric multiprocessing with up to `MAX_CPUS=8` CPUs. The SMP
subsystem consists of four layers: CPU discovery, AP boot, per-CPU state management,
and inter-processor communication.

## CPU Discovery: ACPI MADT Parsing

`acpi_find_madt()` locates the ACPI MADT to enumerate all CPUs:

1. Reads the Multiboot 2 ACPI tag for the RSDP physical address.
2. Prefers the XSDT (64-bit table pointers) when RSDP revision >= 2.
3. Walks RSDT/XSDT entries looking for the MADT signature (`"APIC"`).
4. Extracts Processor Local APIC entries (type 0) into `smp_lapic_ids[]`.

The BSP is always at `smp_lapic_ids[0]` (index 0). `smp_cpu_count` holds the
total count including BSP.

## AP Boot Sequence

`smp_boot_aps()` starts all non-BSP CPUs using the standard INIT-SIPI-SIPI protocol:

1. For each AP: sends INIT IPI, waits 10 ms, sends two SIPI IPIs 200 µs apart.
2. Each SIPI specifies vector = `TRAMP_BASE >> 12` (= 0x8) — the APs start
   executing 16-bit real-mode code at physical address 0x8000.
3. The trampoline enables A20, enters protected mode, then jumps to 64-bit long mode.
4. Each AP calls `ap_entry()` which calls `per_cpu_init()`, `lapic_timer_init_ap()`,
   `ap_barrier_checkin()`, and finally `idle_loop()`.

The BSP calls `smp_wait_for_aps()` after `smp_boot_aps()` and blocks until
all APs have checked in (polling `smp_aps_ready`).

## Per-CPU State: cpu_t

Each CPU has a `cpu_t` struct in the global `g_cpus[]` array:

```c
typedef struct cpu_t {
    struct cpu_t   *self;          // gs:0  — self-pointer for cpu_local()
    uint32_t        cpu_id;        // logical index (0 = BSP)
    uint32_t        lapic_id;      // hardware LAPIC ID
    struct thread  *idle_thread;   // per-CPU idle thread (sentinel)
    uint8_t        *kstack_base;   // per-CPU kernel stack base
    struct thread  *run_queue_head;
    struct thread  *run_queue_tail;
    uint32_t        queue_depth;   // non-idle runnable thread count
    struct thread  *current_thread; // gs:56
    uint64_t        kstack_top;    // gs:64 — used by syscall_entry
    uint64_t        user_rsp_scratch; // gs:72 — scratch at SYSCALL entry
} cpu_t;
```

The `IA32_GS_BASE` MSR on each CPU points to `&g_cpus[cpu_id]`. The `cpu_local()`
macro reads `gs:0` to obtain a pointer to the calling CPU's `cpu_t` in a single
instruction:

```c
static inline cpu_t *cpu_local(void) {
    cpu_t *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}
```

This design was chosen because it requires no explicit `cpu_id` argument and works
correctly regardless of how many CPUs are active.

## Spinlocks

miniOS uses a FIFO ticket spinlock (`spinlock_t`) to protect shared kernel data
structures from concurrent access across CPUs:

| Protected resource | Lock variable |
|--------------------|---------------|
| PMM frame allocator | `pmm_lock` (in pmm.c) |
| Kernel heap | `heap_lock` (in heap.c) |
| VMM page table | `vmm_lock` (in vmm.c) |
| Thread pool + run queues | `thread_pool_lock` (in sched.c) |

**IRQ-safe locking**: Because PMM and scheduler data can be accessed from both
normal kernel context and the LAPIC timer ISR, all locks use `spinlock_irqsave`/
`spinlock_irqrestore` which disable local interrupts before acquiring.

The ticket algorithm guarantees FIFO ordering: each waiter atomically claims a
ticket (incrementing `next[15:8]`), then spins until `owner[7:0]` reaches its
ticket. Supports up to 256 simultaneous waiters (well above `MAX_CPUS=8`).

## Per-CPU Scheduler and Work-Stealing

Each CPU's `cpu_t` holds an independent circular run queue (FIFO, intrusive linked
list via `thread->next`). The per-CPU idle thread is always in the queue as the
circular sentinel — it is never removed.

**Enqueueing**: `sched_balance_enqueue()` tail-inserts a new thread onto the CPU
with the smallest `queue_depth`. If the target differs from the calling CPU, a
scheduler-kick IPI (`SCHEDULER_KICK_VECTOR=50`) wakes the target from `hlt`.

**Work-stealing**: When an AP's idle thread runs and finds `queue_depth == 0`,
it calls `sched_work_steal()` which:
1. Acquires `thread_pool_lock`.
2. Finds the CPU with the highest `queue_depth` (must be > 1 to avoid thrashing).
3. Steals `floor(depth/2)` RUNNABLE threads and moves them to the calling CPU's queue.
4. Releases the lock.

## IPI: TLB Shootdown and Panic Halt

Two IPI channels are implemented (both using `lapic_send_ipi()`):

### TLB Shootdown (vectors 53-60, one per initiator CPU)

`ipi_tlb_shootdown(virt)` is called whenever a PTE changes on a mapping that
might be live on another CPU — `sys_munmap`, `mmap`/`mprotect` remapping a
page, and fork's CoW page-sharing (`fork_cow_share_page`) and CoW-fault
resolution (`vmm_resolve_user_fault`) all go through it.

Each CPU has its own barrier slot (`g_tlb_barriers[cpu_id]`) and its own IPI
vector (`TLB_SHOOTDOWN_VECTOR_BASE + cpu_id`, 8 vectors for `MAX_CPUS=8`), so
two CPUs can initiate a shootdown at the same moment without touching each
other's state — a single shared barrier previously let concurrent initiators
clobber each other's `virt`/`ack_count`, corrupting `ack_count` past zero
(wraps, since it's unsigned) and hanging `ipi_barrier_wait()` forever. The
initiator sets its own slot's `ack_count = smp_cpu_count - 1`, broadcasts its
vector, and blocks in `ipi_barrier_wait()`; each target's `tlb_shootdown_isr`
executes `invlpg(g_tlb_barriers[src_cpu].virt)`, then `ipi_barrier_ack()`
(decrement-before-EOI guarantee), then sends EOI.

`vmm_resolve_user_fault()` (a CoW page fault) can call this with interrupts
disabled (`#PF` is an interrupt gate, IF=0 for the whole handler) — this is
safe: an initiator never waits on itself, only on other CPUs, so it needs no
incoming interrupts to make progress. The only cost is that a peer CPU whose
shootdown targets a CPU currently inside a `#PF` (or any other IF=0 section)
has to wait for that section to finish and return before its IPI is
serviced — bounded, since none of these sections block. An earlier attempt
to shrink that wait by re-enabling interrupts inside `page_fault_exception()`
was reverted: it let a LAPIC timer tick preempt mid-handler, and resuming
from that nested preemption corrupted kernel `.text` mappings (instruction
fetch `#PF`s on kernel code, cascading to a CPU reset) — the exception
return path isn't proven safe against being preempted from inside exception
context.

### Panic Halt (vector 52)

`kernel_panic()` calls `ipi_panic_halt()` which broadcasts `PANIC_HALT_VECTOR=52`
to all APs. Each AP's `panic_halt_isr` executes `cli; hlt`. No EOI is sent — the
halted CPU never needs LAPIC unblocked. The BSP then prints the panic message and
halts with `cli; hlt` as well.
