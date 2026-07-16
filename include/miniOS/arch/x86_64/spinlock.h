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
 * @file spinlock.h
 * @defgroup spinlock Ticket Spinlock
 * @brief FIFO ticket spinlock with IRQ-safe variants.
 *
 * Provides a 16-bit ticket spinlock (spinlock_t) and four operations:
 * - spinlock_lock / spinlock_unlock: basic acquire/release (no IRQ management).
 * - spinlock_irqsave / spinlock_irqrestore: IRQ-safe variants using RFLAGS save.
 * - spinlock_irq / spinlock_irq_unlock: unconditional cli/sti variants.
 * @{
 */

#ifndef _MINIOS_ARCH_X86_64_SPINLOCK_H_
#define _MINIOS_ARCH_X86_64_SPINLOCK_H_

#include <miniOS/types.h>

/**
 * spinlock_t — FIFO ticket spinlock.
 *
 * counter layout (16-bit):
 *   bits  [7:0]  = owner  — ticket currently being served
 *   bits [15:8]  = next   — next ticket to issue
 *
 * Free iff owner == next (counter == 0 after SPINLOCK_INIT).
 * Supports at most 256 simultaneous waiters (MAX_CPUS=8 is well within this).
 */
typedef struct {
    volatile uint16_t counter;
} spinlock_t;

/** SPINLOCK_INIT — static initialiser; produces a free (unlocked) spinlock. */
#define SPINLOCK_INIT { .counter = 0 }

/**
 * spinlock_lock() - Acquire the spinlock (spin until free).
 * @lock: Spinlock to acquire.
 *
 * Atomically claims a ticket (incrementing next) and spins with PAUSE hint
 * until owner reaches that ticket. FIFO order guaranteed.
 * Do NOT call from ISR context (use spinlock_irqsave instead).
 */
void spinlock_lock(spinlock_t *lock);

/**
 * spinlock_unlock() - Release the spinlock.
 * @lock: Spinlock to release. Must be held by the calling CPU.
 *
 * Atomically increments the owner field, handing the lock to the next waiter.
 */
void spinlock_unlock(spinlock_t *lock);

/**
 * spinlock_irqsave() - Save interrupt state, disable IRQs, then acquire.
 * @lock:  Spinlock to acquire.
 * @flags: Output — saved RFLAGS value (pass to spinlock_irqrestore).
 *
 * Use in any code path reachable from both normal context and LAPIC timer ISR.
 * Prevents ISR re-entering the same lock while the lock is held.
 */
void spinlock_irqsave(spinlock_t *lock, unsigned long *flags);

/**
 * spinlock_irqrestore() - Release spinlock and restore interrupt state.
 * @lock:  Spinlock to release. Must have been acquired via spinlock_irqsave.
 * @flags: Saved RFLAGS from spinlock_irqsave (restored as-is; re-enables IRQs
 *         if they were enabled before the matching spinlock_irqsave call).
 */
void spinlock_irqrestore(spinlock_t *lock, unsigned long flags);

/**
 * spinlock_irq() - Unconditionally disable IRQs then acquire spinlock.
 * @lock: Spinlock to acquire.
 *
 * Use only when caller is certain IRQs were enabled (no RFLAGS save needed).
 * Paired with spinlock_irq_unlock().
 */
void spinlock_irq(spinlock_t *lock);

/**
 * spinlock_irq_unlock() - Release spinlock and unconditionally re-enable IRQs.
 * @lock: Spinlock to release. Must have been acquired via spinlock_irq().
 */
void spinlock_irq_unlock(spinlock_t *lock);

/** @} */

#endif /* _MINIOS_ARCH_X86_64_SPINLOCK_H_ */
