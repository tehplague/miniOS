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

#include <miniOS/ipi/ipi.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/io.h>

/* One barrier per possible initiator CPU — see the comment on
 * g_tlb_barriers[] in ipi.h for why a single shared barrier isn't safe. */
ipi_barrier_t g_tlb_barriers[MAX_CPUS];

void ipi_tlb_shootdown(uint64_t virt)
{
    /* Only send shootdowns once all APs are fully up and can handle IPIs */
    if (smp_cpu_count <= 1 || smp_aps_ready < smp_cpu_count - 1)
        return;

    cpu_t *local = cpu_local();
    ipi_barrier_t *barrier = &g_tlb_barriers[local->cpu_id];

    /* Set up this CPU's own barrier slot: all other CPUs must acknowledge.
     * No other CPU ever writes g_tlb_barriers[local->cpu_id], so this needs
     * no lock even when other CPUs are concurrently doing their own
     * shootdowns via their own slots. */
    barrier->virt = virt;
    /* Compiler barrier ensures virt is visible before ack_count is set */
    __asm__ volatile("" : : : "memory");
    barrier->ack_count = smp_cpu_count - 1;
    /* Compiler barrier ensures ack_count is visible before IPIs are sent */
    __asm__ volatile("" : : : "memory");

    /* Broadcast to all other CPUs on this initiator's dedicated vector */
    uint8_t vector = (uint8_t)(TLB_SHOOTDOWN_VECTOR_BASE + local->cpu_id);
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (g_cpus[i].cpu_id != local->cpu_id)
            lapic_send_ipi(g_cpus[i].lapic_id, vector);
    }

    /* Spin until all handlers have acknowledged */
    ipi_barrier_wait(barrier);
}

void tlb_shootdown_isr(uint32_t src_cpu)
{
    /* Flush this CPU's TLB entry for the address set by initiator src_cpu */
    ipi_barrier_t *barrier = &g_tlb_barriers[src_cpu];
    uint64_t virt = barrier->virt;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    /* Decrement barrier BEFORE sending EOI — ordering is critical */
    ipi_barrier_ack(barrier);
    lapic_eoi_asm();
}

/* Global panic barrier — counts APs that received halt IPI */
static ipi_barrier_t g_panic_barrier __attribute__((unused)) = { .ack_count = 0, .virt = 0 };

void ipi_panic_halt(void)
{
    /* Disable interrupts on calling CPU (BSP during panic) */
    __asm__ volatile("cli" : : : "memory");

    if (smp_cpu_count <= 1) {
        /* Uniprocessor: just halt */
        for (;;) __asm__ volatile("hlt");
        __builtin_unreachable();
    }

    cpu_t *local = cpu_local();

    /* Broadcast halt signal to all APs */
    for (uint32_t i = 0; i < smp_cpu_count; i++) {
        if (g_cpus[i].cpu_id != local->cpu_id)
            lapic_send_ipi(g_cpus[i].lapic_id, PANIC_HALT_VECTOR);
    }

    /* Wait up to lapic_bsp_timer_icr/2 ticks for APs to halt.
       Use LAPIC CCR as a measure: read start CCR, spin until CCR
       has decreased by lapic_bsp_timer_icr/2 or wrapped.
       This satisfies "within one LAPIC timer period" requirement. */
    uint32_t timeout = lapic_bsp_timer_icr / 2;
    if (timeout == 0) timeout = 500000; /* fallback if ICR not calibrated */
    uint32_t start = lapic_read(LAPIC_TIMER_CCR);
    for (uint32_t spins = 0; spins < timeout; spins++) {
        __asm__ volatile("pause" : : : "memory");
        uint32_t now = lapic_read(LAPIC_TIMER_CCR);
        /* Break if timer has advanced by at least timeout counts */
        if (start >= now && (start - now) >= timeout)
            break;
    }

    /* All APs should be in cli; hlt now — halt BSP */
    for (;;) __asm__ volatile("cli; hlt");
    __builtin_unreachable();
}

void panic_halt_isr(void *frame)
{
    (void)frame;
    /* AP halt: no EOI needed — this CPU will never process another interrupt */
    for (;;) __asm__ volatile("cli; hlt");
    __builtin_unreachable();
}
