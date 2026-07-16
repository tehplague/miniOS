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

/* stub_heap.c — host-native heap stubs for unit tests that exercise kernel
 * code using kmalloc/kfree (e.g. pipe.c included by test_syscall.c).
 *
 * IMPORTANT: pipe.c casts pipe_t * to uint32_t via (uint32_t)(uintptr_t)ptr.
 * This is safe on miniOS (128MB RAM, all physical addresses < 4GB), but on a
 * host test machine malloc() returns 64-bit addresses above 4GB, causing
 * truncation. We work around this by using mmap() to allocate a bump-pointer
 * arena at a low virtual address (< 2GB) that fits in uint32_t.
 *
 * The arena is 256KB at virtual address 0x200000 (2MB). This is above the
 * kernel image base and well below the 4GB boundary. If MAP_FIXED_NOREPLACE
 * fails (address already in use), we try MAP_FIXED as a fallback, and if that
 * also fails we fall back to regular malloc (tests may then crash if the pipe
 * pointer cast truncates). */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <miniOS/mm/heap.h>

#define STUB_HEAP_BASE  0x200000UL    /* 2 MB — fits comfortably in uint32_t */
#define STUB_HEAP_SIZE  0x40000UL     /* 256 KB — room for ~64 pipe_t objects */

static uint8_t *g_heap_arena = NULL;
static size_t   g_heap_offset = 0;

static void heap_arena_init(void) {
    if (g_heap_arena) return;
    /* Try MAP_FIXED_NOREPLACE first (Linux 4.17+); fall back to MAP_FIXED */
    g_heap_arena = mmap((void *)STUB_HEAP_BASE, STUB_HEAP_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
    if (g_heap_arena == MAP_FAILED) {
        g_heap_arena = mmap((void *)STUB_HEAP_BASE, STUB_HEAP_SIZE,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                            -1, 0);
    }
    if (g_heap_arena == MAP_FAILED) {
        /* Last resort: unmap attempt failed; arena stays NULL and kmalloc
         * will return NULL, causing tests to fail with NULL-dereference rather
         * than silent uint32_t truncation. */
        g_heap_arena = NULL;
    }
}

/**
 * kmalloc() - Allocate from the low-address test arena.
 * Aligns allocations to 8 bytes. Returns NULL on arena exhaustion.
 */
void *kmalloc(size_t size) {
    heap_arena_init();
    if (!g_heap_arena) return NULL;
    size_t aligned_off = (g_heap_offset + 7) & ~7UL;
    if (aligned_off + size > STUB_HEAP_SIZE) return NULL;
    void *ptr = g_heap_arena + aligned_off;
    g_heap_offset = aligned_off + size;
    return ptr;
}

/**
 * kfree() - No-op for the arena allocator.
 * Individual slots are not reclaimed; the arena is reset between tests
 * via stub_heap_reset() called from test setUp() if needed.
 */
void kfree(void *ptr) {
    (void)ptr;
    /* Arena allocator: memory is bulk-cleared in stub_heap_reset() */
}

/**
 * stub_heap_reset() - Reset arena bump pointer for the next test.
 * Call this from setUp() in test files that use kmalloc/kfree.
 */
void stub_heap_reset(void) {
    heap_arena_init();
    g_heap_offset = 0;
    if (g_heap_arena) memset(g_heap_arena, 0, STUB_HEAP_SIZE);
}

size_t heap_free_bytes(void) {
    return STUB_HEAP_SIZE - g_heap_offset;
}

void heap_init(void) {
    heap_arena_init();
}
