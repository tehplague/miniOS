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
 * @file ipi.h
 * @defgroup ipi IPI -- Inter-Processor Interrupts
 * @brief TLB shootdown and panic-halt IPI infrastructure.
 *
 * Implements two IPI paths:
 * - TLB shootdown (vector 51): vmm_unmap_page() calls ipi_tlb_shootdown() to
 *   ensure all CPUs flush the invalidated mapping before the caller continues.
 * - Panic halt (vector 52): kernel_panic() calls ipi_panic_halt() to halt all
 *   APs before the BSP prints the panic message and halts.
 * @{
 */

#ifndef _MINIOS_IPI_IPI_H_
#define _MINIOS_IPI_IPI_H_

#include <miniOS/types.h>
#include <miniOS/arch/x86_64/spinlock.h>

/**
 * ipi_barrier_t - Synchronous completion barrier for broadcast IPIs.
 *
 * The initiator sets ack_count = target CPU count, broadcasts the IPI, then
 * calls ipi_barrier_wait() to spin until ack_count reaches 0. Each handler
 * calls ipi_barrier_ack() before sending EOI to guarantee the initiator
 * observes completion only after invlpg (or cli;hlt) has executed.
 */
typedef struct {
    volatile uint32_t ack_count;  /* decremented by each handler */
    uint64_t          virt;       /* virtual address for TLB shootdown */
} ipi_barrier_t;

/* Global TLB shootdown barrier — shared between initiator and all ISR handlers */
extern ipi_barrier_t g_tlb_barrier;

/**
 * @brief Atomically decrement the barrier's ack_count.
 * @param b Barrier to acknowledge. Called by ISR handlers BEFORE lapic_eoi().
 */
static inline void ipi_barrier_ack(ipi_barrier_t *b) {
    __asm__ volatile("lock subl $1, %0" : "+m"(b->ack_count) : : "memory");
}

/**
 * @brief Spin until barrier->ack_count reaches 0.
 * @param b Barrier to wait on. Called by the IPI initiator after broadcasting.
 */
static inline void ipi_barrier_wait(ipi_barrier_t *b) {
    while (b->ack_count > 0)
        __asm__ volatile("pause" : : : "memory");
}

/**
 * @brief Broadcast TLB-shootdown IPI and wait for all CPUs to flush.
 * @param virt Virtual address to invalidate on all CPUs.
 *
 * Sets g_tlb_barrier.virt = @virt and g_tlb_barrier.ack_count = smp_cpu_count - 1,
 * then broadcasts TLB_SHOOTDOWN_VECTOR (51) to all CPUs except self via
 * lapic_send_ipi. Spins in ipi_barrier_wait() until all handlers have called
 * ipi_barrier_ack(). Caller (sys_munmap) must have already called invlpg locally.
 *
 * Context: Called with interrupts disabled or spinlock protection on g_tlb_barrier.
 */
void ipi_tlb_shootdown(uint64_t virt);

/**
 * @brief ISR for TLB_SHOOTDOWN_VECTOR (51) on remote CPUs.
 * @param frame Pointer to the interrupt frame (unused by this handler).
 *
 * Executes invlpg(g_tlb_barrier.virt), then calls ipi_barrier_ack(&g_tlb_barrier)
 * (decrement-before-EOI guarantees the initiator sees ack_count==0 only after
 * the TLB flush is complete), then calls lapic_eoi_asm().
 */
void tlb_shootdown_isr(void *frame);

/**
 * @brief Broadcast halt IPI to all APs on kernel panic.
 *
 * Sends PANIC_HALT_VECTOR (52) to every CPU except the calling BSP via
 * lapic_send_ipi. Waits up to one LAPIC timer period (lapic_bsp_timer_icr/2
 * CCR ticks) for APs to halt, then returns to allow the BSP to print the
 * panic message and execute cli;hlt. Called from handle_exception().
 */
void ipi_panic_halt(void);

/**
 * @brief ISR for PANIC_HALT_VECTOR (52) on APs.
 * @param frame Pointer to the interrupt frame (unused).
 *
 * Executes cli; hlt. Does NOT send EOI -- the halted CPU never needs the
 * LAPIC unblocked for subsequent interrupts.
 */
void panic_halt_isr(void *frame);

/** @} */

#endif /* _MINIOS_IPI_IPI_H_ */
