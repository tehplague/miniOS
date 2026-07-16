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

#ifndef _MINIOS_MM_PMM_H_
#define _MINIOS_MM_PMM_H_

#include <miniOS/types.h> 


/**
 * pmm_init() - Initialise the physical memory manager from Multiboot 2 info.
 * @mb_info_phys: Physical address of the Multiboot 2 information structure.
 *
 * Parses memory map tags from the Multiboot 2 structure to identify available
 * RAM regions. Marks all available frames FREE in the bitmap (bit=1 means FREE).
 * Explicitly marks frame 0 as USED. Kernelimage frames are also marked USED to
 * prevent the PMM from returning kernel memory.
 *
 * Context: Must be called once at boot before any call to pmm_alloc_frame.
 */
void pmm_init(uint64_t mb_info_phys);

/**
 * pmm_alloc_frame() - Allocate a free 4 KiB physical frame.
 *
 * Scans the bitmap for the first non-zero byte (O(1) amortized), then finds
 * the lowest set bit within it. Marks the frame USED (bit=0) and decrements
 * the total_free counter. Frame 0 is never returned (permanently marked USED).
 *
 * Context: May be called from any non-ISR context. Not ISR-safe.
 * @return: 4 KiB-aligned physical address of the allocated frame,
 *          or 0 on OOM (no free frames remain).
 */
uint64_t pmm_alloc_frame(void);

/**
 * pmm_free_frame() - Return a physical frame to the free pool.
 * @phys: Physical address of the frame to free (must be 4 KiB-aligned).
 *
 * Marks the frame FREE (bit=1) and increments total_free. Guards against
 * double-free: if the frame is already free, the call is a no-op.
 * Frame 0 must never be freed (permanently reserved).
 */
void pmm_free_frame(uint64_t phys);

/**
 * pmm_free_count() - Query the number of currently free physical frames.
 *
 * Returns the total_free counter maintained incrementally by pmm_alloc_frame
 * and pmm_free_frame. O(1) — does not scan the bitmap.
 *
 * @return: Number of free 4 KiB frames available for allocation.
 */
uint64_t pmm_free_count(void);

/**
 * pmm_total_count() - Query the total number of physical frames discovered at boot.
 *
 * Returns total_frames, incremented once per frame during pmm_init().
 * Does not change after boot. O(1).
 *
 * @return: Total 4 KiB frame count (all usable RAM, including kernel-reserved frames).
 */
uint64_t pmm_total_count(void);

/**
 * pmm_alloc_contiguous() - Allocate @n_frames physically contiguous 4 KiB frames.
 * @n_frames: Number of frames required (must be > 0 and a power of 2 for DMA use).
 *
 * Linear bitmap scan — O(n). Suitable for infrequent DMA allocations (virtqueues,
 * DMA buffers). For single-frame allocations prefer pmm_alloc_frame().
 *
 * @return: Physical address of the first frame (PAGE_SIZE-aligned), or 0 on failure.
 */
uint64_t pmm_alloc_contiguous(size_t n_frames);

/**
 * pmm_free_contiguous() - Return @n_frames contiguous frames to the free pool.
 * @phys:     Physical address of the first frame (PAGE_SIZE-aligned).
 * @n_frames: Number of frames to release.
 */
void pmm_free_contiguous(uint64_t phys, size_t n_frames);

/**
 * pmm_ref_frame() - Add an extra owner to an already-allocated frame.
 * @phys: Physical address of the frame (must have been returned by
 *        pmm_alloc_frame(), which starts every frame's refcount at 1).
 *
 * Used when a frame becomes shared between two page tables (fork CoW: a
 * read-only text page or a CoW-pending writable page shared between parent
 * and child). Increments the frame's refcount; does not touch the bitmap.
 */
void pmm_ref_frame(uint64_t phys);

/**
 * pmm_unref_frame() - Drop one owner of a frame; frees it at refcount 0.
 * @phys: Physical address of the frame.
 *
 * Decrements the frame's refcount. If it reaches 0, the frame is returned
 * to the free pool exactly as pmm_free_frame() would. Safe to call on a
 * frame with refcount 1 (behaves like pmm_free_frame()) — this is the normal
 * case for private (non-CoW-shared) pages.
 */
void pmm_unref_frame(uint64_t phys);

#endif /* _MINIOS_MM_PMM_H_ */
