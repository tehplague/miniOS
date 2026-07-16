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

#include <miniOS/arch/x86_64/spinlock.h>

/* -----------------------------------------------------------------------
 * Internal atomic helpers (x86_64 inline asm)
 * --------------------------------------------------------------------- */

/** atomic_xadd_u16 — atomically: tmp = *ptr; *ptr += val; return tmp */
static inline uint16_t atomic_xadd_u16(volatile uint16_t *ptr, uint16_t val)
{
    uint16_t result = val;
    __asm__ volatile("lock xadd %0, %1"
                     : "+r"(result), "+m"(*ptr)
                     : : "memory");
    return result;
}

/** atomic_inc_u8_low — atomically increment low byte of *ptr (owner field).
 * Must NOT use incw: incw increments the full 16-bit value, so when the
 * owner byte overflows 0xFF→0x00 the carry corrupts the next-ticket byte.
 * incb on the low byte increments only bits [7:0] without carry. */
static inline void atomic_inc_u8_low(volatile uint16_t *ptr)
{
    __asm__ volatile("lock incb %0"
                     : "+m"(*(volatile uint8_t *)ptr)
                     : : "memory");
}

/** save_flags_and_cli — push RFLAGS to *flags, then cli */
static inline void save_flags_and_cli(unsigned long *flags)
{
    unsigned long rflags;
    __asm__ volatile(
        "pushf\n\t"
        "pop %0"
        : "=r"(rflags)
        :
        : "memory");
#ifndef TEST_BUILD
    __asm__ volatile("cli" : : : "memory");
#endif
    *flags = rflags;
}

/** restore_flags — push saved value onto stack, popf restores RFLAGS.IF */
static inline void restore_flags(unsigned long flags)
{
    __asm__ volatile(
        "push %0\n\t"
        "popf"
        :
        : "r"(flags)
        : "memory", "cc");
}

/* -----------------------------------------------------------------------
 * Public spinlock API
 * --------------------------------------------------------------------- */

void spinlock_lock(spinlock_t *lock)
{
    /* Claim a ticket by atomically adding 0x0100 (incrementing next field).
     * The return value gives us our ticket in bits [15:8].
     * Our ticket number = old_counter >> 8; but since we added 0x0100 and
     * got back the OLD value, our ticket = (result >> 8). */
    uint16_t ticket = atomic_xadd_u16(&lock->counter, 0x0100);
    uint8_t my_ticket = (uint8_t)(ticket >> 8);

    /* Spin until owner (low byte) matches our ticket.
       Use 'nop' instead of 'pause': 'pause' == 'rep nop' (same opcode F3 90)
       and triggers a KVM PAUSE_FILTER VM exit (~1ms each) in QEMU/KVM.
       A plain 'nop' avoids that while still acting as a memory barrier via
       the 'memory' clobber to prevent compiler hoisting of the load. */
    while ((uint8_t)(lock->counter) != my_ticket) {
        __asm__ volatile("nop" : : : "memory");
    }
}

void spinlock_unlock(spinlock_t *lock)
{
    /* Increment owner field (low byte) to hand off to the next waiter */
    atomic_inc_u8_low(&lock->counter);
}

void spinlock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    save_flags_and_cli(flags);
    spinlock_lock(lock);
}

void spinlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    spinlock_unlock(lock);
    restore_flags(flags);
}

void spinlock_irq(spinlock_t *lock)
{
#ifndef TEST_BUILD
    __asm__ volatile("cli" : : : "memory");
#endif
    spinlock_lock(lock);
}

void spinlock_irq_unlock(spinlock_t *lock)
{
    spinlock_unlock(lock);
#ifndef TEST_BUILD
    __asm__ volatile("sti" : : : "memory");
#endif
}
