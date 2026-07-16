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

/* test_heap.c — Heap allocator unit tests
 * Overrides HEAP_START/HEAP_MAX to point at a host-allocated buffer,
 * then directly #includes heap.c. stub_pmm.c + stub_vmm.c are linked in. */

#include "unity.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Heap backing buffer: 1 MiB, 4096-byte-aligned
 * MUST be defined before #include "../../src/kernel/mm/heap.c" so the
 * address is available for the HEAP_START/HEAP_MAX macro overrides.
 * --------------------------------------------------------------------- */
static char heap_backing[1 * 1024 * 1024] __attribute__((aligned(4096)));

/* Override the kernel's virtual-address heap range to point at our buffer.
 * heap.c uses #define HEAP_START so we undef then redefine. */
#undef HEAP_START
#define HEAP_START ((uint64_t)(uintptr_t)(void *)heap_backing)

#undef HEAP_MAX
#define HEAP_MAX   (HEAP_START + (uint64_t)sizeof(heap_backing))

/* -----------------------------------------------------------------------
 * Spinlock stubs for host unit test build.
 * heap.c includes <miniOS/arch/x86_64/spinlock.h> which declares these;
 * provide no-op implementations so the test build avoids kernel asm.
 * These are defined BEFORE #include "...heap.c" so heap.c sees the stubs.
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

/* Now include the real heap implementation */
#include "../../src/kernel/mm/heap.c"

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    memset(heap_backing, 0, sizeof(heap_backing));
    /* heap_init sets heap_top = HEAP_START and maps the first page */
    heap_init();
}

void tearDown(void) {
    /* nothing — next setUp resets everything */
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

/* kmalloc(0) returns NULL */
void test_kmalloc_zero_returns_null(void) {
    void *p = kmalloc(0);
    TEST_ASSERT_NULL(p);
}

/* kmalloc(64) returns non-NULL; write/readback works */
void test_kmalloc_small_alloc_readback(void) {
    char *p = (char *)kmalloc(64);
    TEST_ASSERT_NOT_NULL(p);
    /* Write a pattern and read it back */
    for (int i = 0; i < 64; i++) p[i] = (char)(i & 0xFF);
    for (int i = 0; i < 64; i++) TEST_ASSERT_EQUAL_INT(i & 0xFF, (unsigned char)p[i]);
}

/* kfree then kmalloc reuses freed block */
void test_kfree_then_kmalloc_reuses_block(void) {
    void *a = kmalloc(64);
    TEST_ASSERT_NOT_NULL(a);
    void *b = kmalloc(64);
    TEST_ASSERT_NOT_NULL(b);
    kfree(a);
    /* Next alloc of same size should get the freed slot (first-fit) */
    void *c = kmalloc(64);
    TEST_ASSERT_EQUAL_PTR(a, c);
}

/* kmalloc(1024) allocates a larger block successfully */
void test_kmalloc_large_alloc(void) {
    void *p = kmalloc(1024);
    TEST_ASSERT_NOT_NULL(p);
    /* Touch all bytes to verify mapping */
    memset(p, 0xAB, 1024);
    unsigned char *bytes = (unsigned char *)p;
    TEST_ASSERT_EQUAL_UINT8(0xAB, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, bytes[1023]);
}

/* heap_free_bytes decreases after kmalloc, increases after kfree */
void test_heap_free_bytes_tracks_usage(void) {
    size_t free_before = heap_free_bytes();
    void *p = kmalloc(128);
    TEST_ASSERT_NOT_NULL(p);
    size_t free_after_alloc = heap_free_bytes();
    TEST_ASSERT_LESS_THAN(free_before, free_after_alloc);

    kfree(p);
    size_t free_after_free = heap_free_bytes();
    /* After free+coalesce, should be back to original or larger (due to coalesce) */
    TEST_ASSERT_GREATER_OR_EQUAL(free_after_alloc, free_after_free);
}

/* multiple sequential allocations do not overlap */
void test_sequential_allocs_no_overlap(void) {
    void *ptrs[8];
    size_t alloc_size = 128;
    for (int i = 0; i < 8; i++) {
        ptrs[i] = kmalloc(alloc_size);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    /* Verify all pointers are distinct and spaced at least alloc_size apart */
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            uintptr_t pi = (uintptr_t)(void *)ptrs[i];
            uintptr_t pj = (uintptr_t)(void *)ptrs[j];
            uint64_t diff = (pi > pj) ? (uint64_t)(pi - pj) : (uint64_t)(pj - pi);
            TEST_ASSERT_GREATER_OR_EQUAL(alloc_size, diff);
        }
    }
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kmalloc_zero_returns_null);
    RUN_TEST(test_kmalloc_small_alloc_readback);
    RUN_TEST(test_kfree_then_kmalloc_reuses_block);
    RUN_TEST(test_kmalloc_large_alloc);
    RUN_TEST(test_heap_free_bytes_tracks_usage);
    RUN_TEST(test_sequential_allocs_no_overlap);
    return UNITY_END();
}
