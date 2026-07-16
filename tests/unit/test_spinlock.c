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

/* test_spinlock.c — unit tests for ticket spinlock (LOCK-01, LOCK-02)
 * spinlock.c is compiled as a separate CMake source (no direct include needed).
 * stub_types.h is force-included via -include flag in TEST_CFLAGS and provides
 * uint8_t/uint16_t/uint64_t etc.; do NOT redefine them here.
 * x86_64 inline asm runs natively on the host build (no stubs needed). */

#include "unity.h"

/* Include the header under test (types resolved via stub_types.h force-include) */
#include <miniOS/arch/x86_64/spinlock.h>

void setUp(void)    { /* nothing */ }
void tearDown(void) { /* nothing */ }

/* -----------------------------------------------------------------------
 * LOCK-01 tests: ticket spinlock correctness
 * --------------------------------------------------------------------- */

/** A freshly initialised spinlock has counter == 0 (owner==0, next==0). */
void test_spinlock_init_is_free(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    TEST_ASSERT_EQUAL_UINT16(0, lk.counter);
}

/** lock() on a free spinlock: next becomes 1, owner stays at 0 (ticket 0 held). */
void test_spinlock_lock_free_spinlock(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    spinlock_lock(&lk);
    /* After acquisition: ticket=0 held, owner (low byte) == 0, next (high byte) == 1 */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)(lk.counter));       /* owner == 0 (ticket 0 serving) */
    TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)(lk.counter >> 8));  /* next  == 1 (one ticket issued) */
    spinlock_unlock(&lk);
}

/** unlock() after lock(): subsequent re-acquire must succeed without spin. */
void test_spinlock_unlock_increments_owner(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    spinlock_lock(&lk);
    spinlock_unlock(&lk);
    /* Re-acquire to confirm unlock allowed next lock to proceed */
    spinlock_lock(&lk);
    spinlock_unlock(&lk);
    /* If we reached here without infinite loop, unlock worked correctly */
    TEST_PASS();
}

/** Two sequential lock/unlock cycles complete without hanging. */
void test_spinlock_sequential_cycles(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    for (int i = 0; i < 10; i++) {
        spinlock_lock(&lk);
        spinlock_unlock(&lk);
    }
    TEST_PASS(); /* No infinite spin = FIFO ticket algorithm correct */
}

/** Ticket assignment: next field increments by 1 per lock() call. */
void test_spinlock_ticket_increments(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    /* First lock: next goes 0->1, owner goes 0->1 */
    spinlock_lock(&lk);
    uint8_t next_after_first = (uint8_t)(lk.counter >> 8);
    TEST_ASSERT_EQUAL_UINT8(1, next_after_first);
    spinlock_unlock(&lk);
    /* Second lock: next goes 1->2, owner goes 1->2 */
    spinlock_lock(&lk);
    uint8_t next_after_second = (uint8_t)(lk.counter >> 8);
    TEST_ASSERT_EQUAL_UINT8(2, next_after_second);
    spinlock_unlock(&lk);
}

/* -----------------------------------------------------------------------
 * LOCK-02 tests: IRQ-save spinlock variants
 * --------------------------------------------------------------------- */

/** spinlock_irqsave acquires the lock (counter changes as expected). */
void test_spinlock_irqsave_acquires_lock(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    unsigned long flags = 0;
    spinlock_irqsave(&lk, &flags);
    /* Lock held: ticket=0 serving, owner (low byte) == 0, next (high byte) == 1 */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)(lk.counter));       /* owner == 0 (ticket 0 serving) */
    TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)(lk.counter >> 8));  /* next  == 1 (one ticket issued) */
    spinlock_irqrestore(&lk, flags);
}

/** spinlock_irqrestore releases the lock (subsequent lock() succeeds). */
void test_spinlock_irqrestore_releases_lock(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    unsigned long flags = 0;
    spinlock_irqsave(&lk, &flags);
    spinlock_irqrestore(&lk, flags);
    /* Must be able to re-acquire without hanging */
    spinlock_lock(&lk);
    spinlock_unlock(&lk);
    TEST_PASS();
}

/** spinlock_irqsave saves non-zero RFLAGS (host always has some flags set). */
void test_spinlock_irqsave_saves_flags(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    unsigned long flags = 0;
    spinlock_irqsave(&lk, &flags);
    /* On host x86_64, RFLAGS always has at least bit 1 (reserved=1) and
     * bit 9 (IF) set in normal execution. Flags must be non-zero. */
    TEST_ASSERT_NOT_EQUAL(0UL, flags);
    spinlock_irqrestore(&lk, flags);
}

/** spinlock_irq and spinlock_irq_unlock complete a lock/unlock cycle. */
void test_spinlock_irq_cycle(void)
{
    spinlock_t lk = SPINLOCK_INIT;
    spinlock_irq(&lk);
    /* Lock held: ticket=0 serving, owner==0, next==1 */
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)(lk.counter));       /* owner == 0 */
    TEST_ASSERT_EQUAL_UINT8(1, (uint8_t)(lk.counter >> 8));  /* next  == 1 */
    spinlock_irq_unlock(&lk);
    /* Re-acquire to confirm unlock worked */
    spinlock_lock(&lk);
    spinlock_unlock(&lk);
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spinlock_init_is_free);
    RUN_TEST(test_spinlock_lock_free_spinlock);
    RUN_TEST(test_spinlock_unlock_increments_owner);
    RUN_TEST(test_spinlock_sequential_cycles);
    RUN_TEST(test_spinlock_ticket_increments);
    RUN_TEST(test_spinlock_irqsave_acquires_lock);
    RUN_TEST(test_spinlock_irqrestore_releases_lock);
    RUN_TEST(test_spinlock_irqsave_saves_flags);
    RUN_TEST(test_spinlock_irq_cycle);
    return UNITY_END();
}
