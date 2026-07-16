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

/**
 * @file smp.h
 * @defgroup smp SMP Boot and Per-CPU State
 * @brief ACPI MADT parsing, AP boot sequence, and per-CPU cpu_t management.
 *
 * Declares cpu_t (the per-CPU state block accessed via GS.base), the global
 * g_cpus[] array, and the public API for SMP initialisation: MADT parsing,
 * AP boot, rendezvous barrier, and per-CPU TSS/GDT setup.
 * @{
 */

#ifndef _MINIOS_ARCH_X86_64_SMP_H_
#define _MINIOS_ARCH_X86_64_SMP_H_

#include <miniOS/types.h>

/* Maximum CPUs this kernel supports. Override at compile time with
   -DMAX_CPUS=N. Sizes all per-CPU arrays. */
#ifndef MAX_CPUS
#define MAX_CPUS 8
#endif

/* Number of CPUs found by acpi_find_madt(). Includes BSP (cpu_id 0). */
extern uint32_t smp_cpu_count;

/* LAPIC IDs of all detected CPUs, indexed 0..smp_cpu_count-1.
   Index 0 is always the BSP (the CPU running kernel_main). */
extern uint8_t  smp_lapic_ids[MAX_CPUS];

/* ------------------------------------------------------------------ */
/*  Per-CPU state (cpu_t)                                              */
/* ------------------------------------------------------------------ */

/* Forward declare thread struct (defined in sched.h) */
struct thread;

/**
 * cpu_t - Per-CPU state block; GS.base on each CPU points to this struct.
 *
 * The first field MUST be a self-pointer at offset 0 (gs:0) so cpu_local()
 * works with a single `mov %%gs:0, %0` instruction regardless of cpu count.
 * Fields at fixed GS offsets are used by assembly: current_thread at gs:56,
 * kstack_top at gs:64, user_rsp_scratch at gs:72.
 */
typedef struct cpu_t {
    struct cpu_t   *self;        /* gs:0 — self pointer for cpu_local() */
    uint32_t        cpu_id;      /* logical CPU index (0 = BSP) */
    uint32_t        lapic_id;    /* hardware LAPIC ID from MADT */
    struct thread  *idle_thread; /* per-CPU idle thread */
    uint8_t        *kstack_base; /* base of this CPU's boot/idle kernel stack */
    /* Per-CPU run queue.
       Circular singly-linked list via thread->next.
       idle_thread is always in the queue as the sentinel node.
       queue_depth counts only non-idle RUNNABLE/BLOCKED threads. */
    struct thread  *run_queue_head;  /* first thread in round-robin queue */
    struct thread  *run_queue_tail;  /* last thread (tail->next == head) */
    uint32_t        queue_depth;     /* count of non-idle threads on this CPU */
    struct thread  *current_thread;  /* thread currently running on this CPU — gs:56 */
    uint64_t        kstack_top;      /* per-CPU kernel stack top for SYSCALL/TSS RSP0 — gs:64 */
    uint64_t        user_rsp_scratch;/* per-CPU scratch: user RSP at SYSCALL entry — gs:72 */
} cpu_t;

/* Global array of cpu_t, indexed 0..smp_cpu_count-1.
   Index 0 = BSP. Static allocation, sized by MAX_CPUS. */
extern cpu_t g_cpus[MAX_CPUS];

/* cpu_local() — return pointer to the current CPU's cpu_t.
   Reads gs:0 (the self-pointer installed by per_cpu_init).
   Works without explicit cpu_id because GS.base = &g_cpus[cpu_id].
   Usage: cpu_t *cpu = cpu_local(); */
static inline cpu_t *cpu_local(void) {
    cpu_t *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p) :: "memory");
    return p;
}

/**
 * @brief Enable SSE/SSE2 on the calling CPU.
 *
 * Sets CR4.OSFXSR and CR4.OSXMMEXCPT, clears CR0.EM, sets CR0.MP, then
 * executes FNINIT and LDMXCSR 0x1F80 to put the FPU and SSE unit in a
 * known state. Must be called on every CPU (BSP in kernel_main, each AP
 * in ap_entry) before any userspace SSE instruction executes.
 */
void cpu_enable_sse(void);

/**
 * @brief Initialise per-CPU GDT, TSS, IDT, and GS base for one CPU.
 * @param cpu_id Logical CPU index (0 = BSP, 1..N-1 = APs).
 *
 * Steps performed:
 *  1. Fills g_cpus[cpu_id].self, .cpu_id, .lapic_id.
 *  2. Allocates a 16 KiB kernel stack via kmalloc and stores base pointer.
 *  3. Copies the BSP GDT into a per-CPU buffer and patches in the CPU's TSS descriptor.
 *  4. Executes lgdt (per-CPU GDT), lidt (shared IDT), ltr (per-CPU TSS selector).
 *  5. Writes IA32_GS_BASE MSR to &g_cpus[cpu_id] so cpu_local() returns the correct struct.
 */
void per_cpu_init(uint32_t cpu_id);

/* ------------------------------------------------------------------ */
/*  ACPI / SMP boot API                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse ACPI MADT and populate smp_lapic_ids[]/smp_cpu_count.
 * @param mb_info_phys Physical address of the Multiboot 2 information structure (from RDI at boot).
 *
 * Locates the RSDP via the Multiboot 2 ACPI tag (or EBDA/BIOS ROM scan fallback),
 * prefers XSDT (RSDP revision >= 2) over RSDT to support memory above 4 GB,
 * walks the SDT entries to find the MADT, then extracts all Processor Local APIC
 * entries (type 0) into smp_lapic_ids[]. Index 0 is always the BSP.
 */
void acpi_find_madt(uint64_t mb_info_phys);

/**
 * @brief Send INIT-SIPI-SIPI sequence to all non-BSP CPUs.
 *
 * Iterates smp_lapic_ids[1..smp_cpu_count-1], sends INIT IPI, waits 10 ms,
 * sends two SIPI IPIs 200 us apart with vector = TRAMP_BASE >> 12 (0x8).
 * Each AP executes the 16-bit trampoline at physical 0x8000, transitions to
 * 64-bit long mode, and calls ap_entry(). Requires lapic_init() to have run.
 */
void smp_boot_aps(void);

/* ------------------------------------------------------------------ */
/*  SMP rendezvous barrier                                            */
/* ------------------------------------------------------------------ */

/* AP rendezvous barrier counter.
   Each AP atomically increments this when its per-CPU init is complete.
   BSP waits until smp_aps_ready == smp_cpu_count - 1 (all APs checked in). */
extern volatile uint32_t smp_aps_ready;

/**
 * @brief Update this CPU's TSS RSP0 and cpu_t.kstack_top.
 * @param rsp0 New kernel stack top to install in TSS privileged_stack_table[0]
 *             and cpu_t.kstack_top (read by syscall_entry via gs:64).
 *
 * Must be called whenever the running thread changes (in sched_schedule)
 * and before the first ring-3 entry on any CPU, so that both interrupt-
 * driven (iretq path) and SYSCALL-driven ring-3 to ring-0 transitions use
 * the correct kernel stack for the new thread.
 */
void per_cpu_update_rsp0(uint64_t rsp0);

/**
 * @brief Signal that this AP has completed its CPU init.
 *
 * Atomically increments smp_aps_ready. Called by each AP after per_cpu_init()
 * and lapic_timer_init_ap() succeed, immediately before entering the AP idle loop.
 * The BSP polls smp_aps_ready in smp_wait_for_aps() until it reaches smp_cpu_count-1.
 */
void ap_barrier_checkin(void);

/**
 * @brief Block until all APs have checked in at the rendezvous barrier.
 *
 * Spins with PAUSE until smp_aps_ready == smp_cpu_count - 1. Must be called
 * by the BSP after smp_boot_aps() and before the second call to sched_init()
 * (which sets up per-CPU run queues). Guaranteed to return eventually because
 * each AP calls ap_barrier_checkin() exactly once.
 */
void smp_wait_for_aps(void);

/**
 * @brief Start LAPIC timer on the calling AP using the BSP-calibrated ICR.
 *
 * Reads lapic_bsp_timer_icr (calibrated by the BSP) and writes it directly
 * to LAPIC_TIMER_ICR, avoiding a second PIT calibration which would cause
 * contention on smp_aps_ready. Sets LVT timer to periodic mode at
 * LAPIC_TIMER_VECTOR. Must be called after LAPIC is software-enabled on AP.
 */
void lapic_timer_init_ap(void);

/** @} */

#endif /* _MINIOS_ARCH_X86_64_SMP_H_ */
