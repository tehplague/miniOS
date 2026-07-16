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

#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/io.h>
#include <miniOS/syscall.h>

/* ------------------------------------------------------------------ */
/*  Trampoline binary symbols (from trampoline.asm)                   */
/* ------------------------------------------------------------------ */

extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];

/* ------------------------------------------------------------------ */
/*  SMP rendezvous barrier (Plan 28-03)                               */
/* ------------------------------------------------------------------ */

/* smp_aps_ready — atomic counter incremented by each AP after it has
   completed per-CPU init and started its LAPIC timer.
   BSP polls this until it reaches smp_cpu_count - 1. */
volatile uint32_t smp_aps_ready = 0;

/* ap_barrier_checkin() — called by each AP to signal readiness to the BSP.
   Uses GCC atomic builtin for a freestanding-safe atomic increment. */
void ap_barrier_checkin(void) {
    __sync_fetch_and_add(&smp_aps_ready, 1);
    __sync_synchronize();
}

/* smp_wait_for_aps() — BSP spins until all APs have checked in.
   Uses 'pause' instruction to reduce bus contention during spin. */
void smp_wait_for_aps(void) {
    if (smp_cpu_count <= 1) return;  /* uniprocessor: nothing to wait for */
    uint32_t expected = smp_cpu_count - 1;
    printk("SMP: waiting for %u APs to check in...\n", expected);
    while (smp_aps_ready < expected) {
        __asm__ volatile("pause" ::: "memory");
    }
    __sync_synchronize();
    printk("SMP: all %u APs checked in, continuing boot\n", expected);
}

/* ------------------------------------------------------------------ */
/*  ap_entry — first C function executed on each AP                   */
/* ------------------------------------------------------------------ */

/* ap_entry() — AP execution after trampoline exits to 64-bit long mode.
   At entry: BSP's page tables active (cr3 from trampoline 0x8FF0),
   RSP = ap_boot_stacks[cpu_id] top (set by trampoline from 0x8FE8),
   no per-CPU GDT/TSS yet. */
static __attribute__((noreturn)) void ap_entry(void) {
    /* Enable SSE/SSE2 on this AP (CR0/CR4 are per-CPU; BSP's enable_sse()
       in kernel_main() only affects the BSP). Without this, any SSE instruction
       in userspace triggers #UD on the AP. */
    cpu_enable_sse();

    /* Software-enable LAPIC on this AP (each AP must do this independently) */
    lapic_write(LAPIC_SVR, LAPIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);

    /* Read own LAPIC ID to determine logical cpu_id */
    uint32_t my_lapic_id = lapic_read(LAPIC_ID) >> 24;

    /* Find logical cpu_id by matching LAPIC ID in smp_lapic_ids[] */
    uint32_t my_cpu_id = 0;
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (smp_lapic_ids[i] == (uint8_t)my_lapic_id) {
            my_cpu_id = i;
            break;
        }
    }

    /* Initialize per-CPU state: GDT copy, TSS, GS-base MSR */
    per_cpu_init(my_cpu_id);

    /* Set up SYSCALL/SYSRET MSRs on this AP (per-CPU MSRs; BSP's syscall_init
     * only configured the BSP).  Needed so fork children can SYSRET to user mode
     * on APs — without this, IA32_STAR=0 causes a #GP on the first SYSRET. */
    syscall_init();

    /* Verify cpu_local() works after GS-base is set */
    cpu_t *self = cpu_local();
    printk("AP: cpu_id=%u LAPIC=%u per-CPU init done, GS.base=0x%llx\n",
           self->cpu_id, self->lapic_id, (uint64_t)self);

    /* Enable interrupts so LAPIC timer fires on this AP */
    __asm__ volatile("sti");

    /* Calibrate and start per-CPU LAPIC timer using BSP's calibrated ICR */
    lapic_timer_init_ap();

    /* Check in to BSP barrier — BSP unblocks once all APs reach here */
    ap_barrier_checkin();

    /* Halt until scheduler idle thread takes over via LAPIC timer tick */
    for (;;) __asm__ volatile("hlt");
}

/* ------------------------------------------------------------------ */
/*  Trampoline placement                                               */
/* ------------------------------------------------------------------ */

/* TRAMPOLINE_PHYS: physical address where trampoline is copied.
   Must be below 1MB and page-aligned. SIPI vector = TRAMPOLINE_PHYS / 0x1000 = 0x08. */
#define TRAMPOLINE_PHYS  0x8000ULL
#define TRAMPOLINE_VA    (TRAMPOLINE_PHYS + KERNEL_VMA)

/* Per-AP bootstrap stacks.
   Each AP needs a valid RSP before it can execute any C code (including
   per_cpu_init). RSP after SIPI is 0 (CPU reset value); calling a C
   function with RSP=0 immediately triple-faults.
   4 KiB per AP is enough for per_cpu_init() + lapic_timer_init_ap() +
   interrupt frames during the hlt loop before the scheduler takes over. */
static uint8_t ap_boot_stacks[MAX_CPUS][4096] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */
/*  ICR delivery mode constants                                        */
/* ------------------------------------------------------------------ */

#define ICR_INIT        0x00000500UL  /* INIT IPI delivery mode */
#define ICR_SIPI        0x00000600UL  /* Startup IPI delivery mode */
#define ICR_ASSERT      0x00004000UL  /* Assert level (INIT assert) */
#define ICR_DEASSERT    0x00008000UL  /* Level de-assert (INIT deassert) */

/* ------------------------------------------------------------------ */
/*  Simple busy-wait delay (~10ms using pause loop)                   */
/* ------------------------------------------------------------------ */

/* delay_10ms: busy-loop providing at least 200µs delay (Intel MP spec minimum).
   Uses a small iteration count to avoid stalling under QEMU TCG/KVM where each
   vCPU only gets a fraction of host time and VM exit overhead per iteration
   makes large loop counts take seconds instead of milliseconds.
   500K iterations: fast enough on QEMU TCG (< 100ms) and real hardware. */
static void delay_10ms(void) {
    for (volatile uint32_t i = 500000U; i > 0; i--) {
        __asm__ volatile("" ::: "memory");  /* prevent compiler from hoisting */
    }
}

/* ------------------------------------------------------------------ */
/*  smp_boot_aps — public API                                         */
/* ------------------------------------------------------------------ */

void smp_boot_aps(void) {
    if (smp_cpu_count <= 1) {
        printk("SMP: only 1 CPU detected, no APs to boot\n");
        return;
    }

    /* Step 1: Copy trampoline binary to physical 0x8000.
       Physical 0x8000 is within the KERNEL_VMA identity window (0..1GB mapped). */
    uint32_t tramp_size = (uint32_t)((uint64_t)trampoline_end - (uint64_t)trampoline_start);
    uint8_t *dst = (uint8_t *)(TRAMPOLINE_VA);
    uint8_t *src = trampoline_start;
    for (uint32_t i = 0; i < tramp_size; i++) {
        dst[i] = src[i];
    }

    /* Step 2: Fill static pointer slots (cr3, ap_entry) in the trampoline page.
       RSP is AP-specific and is written per-AP in the loop below.
       0x8FE8 (offset 0xFE8) = AP initial RSP  (written per AP in loop)
       0x8FF0 (offset 0xFF0) = BSP cr3 physical address
       0x8FF8 (offset 0xFF8) = ap_entry virtual address */
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    *(volatile uint64_t *)(TRAMPOLINE_VA + 0xFF0) = cr3_val;              /* cr3 */
    *(volatile uint64_t *)(TRAMPOLINE_VA + 0xFF8) = (uint64_t)ap_entry;  /* entry VA */

    /* Step 3: Send INIT-SIPI-SIPI to each AP (all non-BSP CPUs).
       smp_lapic_ids[0] = BSP (skip), smp_lapic_ids[1..n-1] = APs.

       ICR_HIGH[31:24] = destination LAPIC ID.
       ICR_LOW delivery encoding:
         INIT assert:    mode=5 (INIT), level=1 (assert)
         INIT deassert:  mode=5 (INIT), level=0 (deassert), trigger=1 (level)
         SIPI:           mode=6 (startup), vector=0x08 (page number = 0x8000 / 0x1000) */
    for (uint32_t i = 1; i < smp_cpu_count; i++) {
        uint8_t lapic_id = smp_lapic_ids[i];
        printk("SMP: sending INIT-SIPI-SIPI to LAPIC %u\n", lapic_id);

        /* Write per-AP bootstrap stack top into trampoline pointer table.
           The trampoline sets RSP = [0x8FE8] before jumping to ap_entry().
           Stack grows downward so use the end of the array as the top. */
        uint64_t stack_top = (uint64_t)(uintptr_t)(ap_boot_stacks[i] + sizeof(ap_boot_stacks[i]));
        *(volatile uint64_t *)(TRAMPOLINE_VA + 0xFE8) = stack_top;

        /* INIT assert */
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
        lapic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_ASSERT);
        delay_10ms();

        /* INIT deassert */
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
        lapic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_DEASSERT);
        delay_10ms();

        /* SIPI #1 — vector 0x08 -> AP starts at physical 0x8000 */
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
        lapic_write(LAPIC_ICR_LOW,  ICR_SIPI | 0x08);
        delay_10ms();

        /* SIPI #2 — second SIPI per Intel MP Specification */
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)lapic_id << 24);
        lapic_write(LAPIC_ICR_LOW,  ICR_SIPI | 0x08);
        delay_10ms();
    }

    printk("SMP: INIT-SIPI-SIPI sent to %u APs\n", smp_cpu_count - 1);
}
