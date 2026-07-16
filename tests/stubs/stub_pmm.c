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

/* stub_pmm.c — fake PMM for host-native unit tests of the heap.
 * pmm_alloc_frame returns sequential dummy frame addresses (non-zero).
 * The heap only passes the returned phys to vmm_map_page which is a no-op,
 * so the value does not need to point to real memory. */

#include <miniOS/mm/pmm.h>

static uint64_t stub_frame_counter = 1;

uint64_t pmm_alloc_frame(void) {
    /* Return a unique non-zero dummy physical address per call.
     * The heap just forwards this to vmm_map_page which ignores it. */
    return (stub_frame_counter++) * 0x1000ULL;
}

void pmm_free_frame(uint64_t phys) {
    (void)phys;
    /* no-op for testing */
}

void pmm_ref_frame(uint64_t phys) {
    (void)phys;
    /* no-op for testing */
}

void pmm_unref_frame(uint64_t phys) {
    (void)phys;
    /* no-op for testing */
}

uint64_t pmm_free_count(void) {
    return 256 - stub_frame_counter;
}

/* Reset helper for tests that want a fresh PMM state.
 * Called indirectly through heap setUp (heap_init resets heap_top). */
void stub_pmm_reset(void) {
    stub_frame_counter = 1;
}
