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
 * @file heap.h
 * @defgroup heap Kernel Heap (kmalloc/kfree)
 * @brief Boundary-tag kernel heap allocator at HEAP_START (0xFFFF820000000000).
 *
 * Implements a simple first-fit boundary-tag heap that grows upward from
 * HEAP_START. Maximum size is HEAP_MAX - HEAP_START = 4 MiB. Not thread-safe
 * on its own; callers must hold heap_lock (a spinlock in heap.c) in SMP context.
 * @{
 */

#ifndef _MINIOS_MM_HEAP_H_
#define _MINIOS_MM_HEAP_H_

#include <miniOS/types.h>

/**
 * heap_init() - Initialise the kernel heap allocator.
 *
 * @brief Maps the first heap page at HEAP_START via vmm_map_page() and writes the
 * initial free-block boundary tag covering the entire heap region. Must be
 * called exactly once during boot before any call to kmalloc(). Called by
 * kernel_main() after vmm setup.
 */
void heap_init(void);

/**
 * kmalloc() - Allocate @size bytes from the kernel heap.
 * @param size Number of bytes to allocate. Rounded up to 8-byte alignment internally.
 *
 * @brief Uses a first-fit boundary-tag strategy. Acquires heap_lock (spinlock_irqsave)
 * before scanning. Maps additional pages via vmm_map_page() if the current heap
 * extent is exhausted (up to HEAP_MAX).
 *
 * @return 8-byte-aligned pointer to allocated memory, or NULL on OOM.
 */
void *kmalloc(size_t size);

/**
 * kfree() - Return a kmalloc'd allocation to the kernel heap free list.
 * @param ptr Pointer returned by a previous kmalloc() call. Passing NULL is a no-op.
 *
 * @brief Marks the block free in its boundary tag and coalesces adjacent free blocks
 * (forward and backward). Acquires heap_lock before modifying heap state.
 */
void kfree(void *ptr);

/**
 * heap_free_bytes() - Query the total free bytes currently available in the heap.
 *
 * @brief Walks the boundary-tag chain and sums the sizes of all free blocks. O(n) in
 * the number of heap blocks. Intended for diagnostic/debug output only.
 *
 * @return Total free bytes across all free blocks in the heap.
 */
size_t heap_free_bytes(void);

/** @} */

#endif /* _MINIOS_MM_HEAP_H_ */
