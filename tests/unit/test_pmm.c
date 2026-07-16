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

/* test_pmm.c — PMM bitmap unit tests
 * Directly #includes pmm.c to access static internals. */

#include "unity.h"
#include <string.h>

/* Provide the linker symbol that pmm.c references */
uint64_t __image_end = 0;

/* -----------------------------------------------------------------------
 * Spinlock stubs for host unit test build.
 * pmm.c includes <miniOS/arch/x86_64/spinlock.h> which declares these;
 * provide no-op implementations so the test build avoids kernel asm.
 * These are defined BEFORE #include "...pmm.c" so pmm.c sees the stubs.
 * --------------------------------------------------------------------- */
#ifndef _MINIOS_ARCH_X86_64_SPINLOCK_H_
#define _MINIOS_ARCH_X86_64_SPINLOCK_H_
#include <stdint.h>
typedef struct { volatile uint16_t counter; } spinlock_t;
#define SPINLOCK_INIT { .counter = 0 }
static inline void spinlock_lock(spinlock_t *l)                        { (void)l; }
static inline void spinlock_unlock(spinlock_t *l)                      { (void)l; }
static inline void spinlock_irqsave(spinlock_t *l, unsigned long *f)   { (void)l; (void)f; }
static inline void spinlock_irqrestore(spinlock_t *l, unsigned long f) { (void)l; (void)f; }
static inline void spinlock_irq(spinlock_t *l)                         { (void)l; }
static inline void spinlock_irq_unlock(spinlock_t *l)                  { (void)l; }
#endif /* _MINIOS_ARCH_X86_64_SPINLOCK_H_ */

/* Include the PMM implementation directly to access its statics */
#include "../../src/kernel/mm/pmm.c"

/* -----------------------------------------------------------------------
 * setUp / tearDown — run before/after every test
 * --------------------------------------------------------------------- */
static uint8_t s_test_bitmap[8];      /* covers 64 frames */
static uint8_t s_test_refcount[64];   /* one byte per frame, per g_max_frames */

void setUp(void) {
    g_pmm_bitmap   = s_test_bitmap;
    g_bitmap_bytes = sizeof(s_test_bitmap);
    g_max_frames   = sizeof(s_test_bitmap) * 8;
    memset(g_pmm_bitmap, 0, g_bitmap_bytes);
    g_pmm_refcount = s_test_refcount;
    memset(g_pmm_refcount, 0, sizeof(s_test_refcount));
    total_free = 0;
}

void tearDown(void) {
    /* nothing */
}

/* -----------------------------------------------------------------------
 * Helper: manually mark a frame as free (seed the bitmap)
 * --------------------------------------------------------------------- */
static void seed_frame_free(uint64_t frame_idx) {
    if (frame_idx < g_max_frames) {
        g_pmm_bitmap[frame_idx / 8] |= (uint8_t)(1u << (frame_idx % 8));
        total_free++;
    }
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* pmm_alloc_frame returns non-zero address after manually seeding bitmap */
void test_alloc_returns_nonzero_after_seeding(void) {
    seed_frame_free(1);   /* frame 1 = phys 0x1000 */
    uint64_t phys = pmm_alloc_frame();
    TEST_ASSERT_NOT_EQUAL(0, (int)phys);
    TEST_ASSERT_EQUAL_UINT64(0x1000, phys);
}

/* pmm_free_frame marks frame as free; pmm_alloc_frame returns same frame */
void test_free_then_alloc_returns_same_frame(void) {
    seed_frame_free(5);
    uint64_t phys = pmm_alloc_frame();
    TEST_ASSERT_EQUAL_UINT64(5 * PAGE_SIZE, phys);

    /* Free it, then alloc again */
    pmm_free_frame(phys);
    uint64_t phys2 = pmm_alloc_frame();
    TEST_ASSERT_EQUAL_UINT64(phys, phys2);
}

/* pmm_free_count returns correct count after alloc and free */
void test_free_count_tracks_alloc_and_free(void) {
    seed_frame_free(10);
    seed_frame_free(11);
    seed_frame_free(12);
    TEST_ASSERT_EQUAL_UINT64(3, pmm_free_count());

    uint64_t a = pmm_alloc_frame();
    TEST_ASSERT_EQUAL_UINT64(2, pmm_free_count());

    pmm_free_frame(a);
    TEST_ASSERT_EQUAL_UINT64(3, pmm_free_count());
}

/* pmm_alloc_frame returns 0 (OOM) when bitmap is all-zero */
void test_alloc_returns_zero_when_oom(void) {
    /* bitmap already all-zero from setUp */
    uint64_t phys = pmm_alloc_frame();
    TEST_ASSERT_EQUAL_UINT64(0, phys);
}

/* pmm_free_frame on frame 0 is a no-op (guard) */
void test_free_frame_zero_is_noop(void) {
    uint64_t count_before = pmm_free_count();
    pmm_free_frame(0);
    TEST_ASSERT_EQUAL_UINT64(count_before, pmm_free_count());
    /* Frame 0 must remain marked used */
    TEST_ASSERT_EQUAL_UINT8(0, g_pmm_bitmap[0] & 1);
}

/* double-free is idempotent (total_free does not double-increment) */
void test_double_free_is_idempotent(void) {
    seed_frame_free(20);
    pmm_alloc_frame();  /* allocate frame 20 */
    TEST_ASSERT_EQUAL_UINT64(0, pmm_free_count());

    pmm_free_frame(20 * PAGE_SIZE);
    TEST_ASSERT_EQUAL_UINT64(1, pmm_free_count());

    /* Second free should not increment count again */
    pmm_free_frame(20 * PAGE_SIZE);
    TEST_ASSERT_EQUAL_UINT64(1, pmm_free_count());
}

/* pmm_unref_frame on a freshly-allocated frame (refcount 1) behaves like
 * pmm_free_frame — the common case for a private (non-CoW-shared) page. */
void test_unref_frame_at_refcount_one_frees(void) {
    seed_frame_free(7);
    uint64_t phys = pmm_alloc_frame();
    TEST_ASSERT_EQUAL_UINT64(0, pmm_free_count());

    pmm_unref_frame(phys);
    TEST_ASSERT_EQUAL_UINT64(1, pmm_free_count());
}

/* pmm_ref_frame bumps the refcount so pmm_unref_frame must be called twice
 * (once per sharer) before the frame is actually returned to the pool —
 * models fork() CoW-sharing a page between parent and child. */
void test_ref_frame_requires_matching_unrefs(void) {
    seed_frame_free(8);
    uint64_t phys = pmm_alloc_frame();
    pmm_ref_frame(phys);  /* now shared by two owners */

    pmm_unref_frame(phys);  /* first owner (e.g. the child) drops its share */
    TEST_ASSERT_EQUAL_UINT64(0, pmm_free_count());  /* still held by the other owner */

    pmm_unref_frame(phys);  /* second owner (e.g. the parent) drops its share */
    TEST_ASSERT_EQUAL_UINT64(1, pmm_free_count());  /* now actually freed */
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_alloc_returns_nonzero_after_seeding);
    RUN_TEST(test_free_then_alloc_returns_same_frame);
    RUN_TEST(test_free_count_tracks_alloc_and_free);
    RUN_TEST(test_alloc_returns_zero_when_oom);
    RUN_TEST(test_free_frame_zero_is_noop);
    RUN_TEST(test_double_free_is_idempotent);
    RUN_TEST(test_unref_frame_at_refcount_one_frees);
    RUN_TEST(test_ref_frame_requires_matching_unrefs);
    return UNITY_END();
}
