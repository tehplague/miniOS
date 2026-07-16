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

#include <miniOS/mm/heap.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/types.h>
#include <miniOS/arch/x86_64/spinlock.h>

/* Heap virtual address range — allow test builds to override via -D or pre-include */
#ifndef HEAP_START
#define HEAP_START  0xFFFF820000000000ULL
#endif
#ifndef HEAP_MAX
#define HEAP_MAX    (HEAP_START + 64ULL * 1024 * 1024)  /* 64MB limit */
#endif

/* Block layout: [header 16B][payload][footer 8B]
 * header: size (8B) + used flag (1B) + 7B padding
 * footer: size (8B) only
 * size field = payload size (not including header+footer overhead) */

#define HDR_SIZE  16ULL   /* sizeof aligned header */
#define FTR_SIZE  8ULL    /* sizeof footer */
#define OVERHEAD  (HDR_SIZE + FTR_SIZE)
#define MIN_BLOCK 32ULL   /* minimum allocation unit (8 bytes of usable payload) */

struct heap_hdr {
    size_t   size;   /* payload size */
    uint8_t  used;   /* 1 = allocated, 0 = free */
    uint8_t  _pad[7];
};

struct heap_ftr {
    size_t size;     /* payload size (mirrors header) */
};

static uint64_t heap_top = 0;   /* next unmapped virtual address */

/* Lock ordering: heap_lock is always acquired BEFORE pmm_lock.
 * heap_expand() calls pmm_alloc_frame() which acquires pmm_lock.
 * Never acquire heap_lock while holding pmm_lock. */
static spinlock_t heap_lock = SPINLOCK_INIT;

/* Return pointer to footer of block whose header is hdr */
static inline struct heap_ftr *blk_ftr(struct heap_hdr *hdr) {
    return (struct heap_ftr *)((uint8_t *)hdr + HDR_SIZE + hdr->size);
}

/* Return pointer to header of block that starts right after hdr's block */
static inline struct heap_hdr *blk_next(struct heap_hdr *hdr) {
    return (struct heap_hdr *)((uint8_t *)hdr + HDR_SIZE + hdr->size + FTR_SIZE);
}

/* Return payload pointer for a header */
static inline void *blk_payload(struct heap_hdr *hdr) {
    return (void *)((uint8_t *)hdr + HDR_SIZE);
}

/* Return header from a payload pointer */
static inline struct heap_hdr *hdr_from_payload(void *ptr) {
    return (struct heap_hdr *)((uint8_t *)ptr - HDR_SIZE);
}

/* Initialise a block at addr with given payload size and used flag */
static void blk_init(uint64_t addr, size_t payload_size, uint8_t used) {
    struct heap_hdr *hdr = (struct heap_hdr *)addr;
    hdr->size = payload_size;
    hdr->used = used;
    hdr->_pad[0] = hdr->_pad[1] = hdr->_pad[2] = hdr->_pad[3] = 0;
    hdr->_pad[4] = hdr->_pad[5] = hdr->_pad[6] = 0;
    struct heap_ftr *ftr = blk_ftr(hdr);
    ftr->size = payload_size;
}

/* Map one new page and add it as a free block at heap_top.
 * Coalesces with the preceding block if it is free.
 * Returns pointer to the resulting free block, or NULL on OOM. */
static struct heap_hdr *heap_expand(void) {
    if (heap_top >= HEAP_MAX) return NULL;

    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) return NULL;

    int r = vmm_map_page(heap_top, phys, PAGE_PRESENT | PAGE_WRITE);
    if (r != 0) {
        pmm_free_frame(phys);
        return NULL;
    }

    /* Initialise new page as one large free block */
    size_t new_payload = PAGE_SIZE - OVERHEAD;
    blk_init(heap_top, new_payload, 0);

    struct heap_hdr *new_blk = (struct heap_hdr *)heap_top;
    heap_top += PAGE_SIZE;

    /* Try to coalesce with the block immediately before this new page.
     * The footer of the previous block ends exactly at new_blk. */
    if ((uint64_t)new_blk > HEAP_START) {
        struct heap_ftr *prev_ftr = (struct heap_ftr *)((uint8_t *)new_blk - FTR_SIZE);
        struct heap_hdr *prev_hdr =
            (struct heap_hdr *)((uint8_t *)new_blk - FTR_SIZE - prev_ftr->size - HDR_SIZE);
        if (prev_hdr->used == 0) {
            /* Merge: extend prev_hdr to include new_blk */
            size_t merged = prev_hdr->size + OVERHEAD + new_blk->size;
            prev_hdr->size = merged;
            blk_ftr(prev_hdr)->size = merged;
            new_blk = prev_hdr;
        }
    }

    return new_blk;
}

void heap_init(void) {
    heap_top = HEAP_START;

    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) return;   /* no memory — kmalloc will return NULL */

    int r = vmm_map_page(HEAP_START, phys, PAGE_PRESENT | PAGE_WRITE);
    if (r != 0) {
        pmm_free_frame(phys);
        return;
    }

    size_t first_payload = PAGE_SIZE - OVERHEAD;
    blk_init(HEAP_START, first_payload, 0);
    heap_top = HEAP_START + PAGE_SIZE;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    /* Round up to 8-byte alignment */
    size = (size + 7ULL) & ~7ULL;

    /* Ensure minimum payload fits a free-list pointer (8 bytes) */
    if (size < MIN_BLOCK - OVERHEAD) size = MIN_BLOCK - OVERHEAD;

    unsigned long flags;
    spinlock_irqsave(&heap_lock, &flags);

retry:;
    struct heap_hdr *cur = (struct heap_hdr *)HEAP_START;
    uint64_t end = heap_top;

    while ((uint64_t)cur < end) {
        if (cur->used == 0 && cur->size >= size) {
            /* Found a suitable free block — split if remainder is big enough */
            size_t remainder = cur->size - size;
            if (remainder >= MIN_BLOCK) {
                /* Split: carve 'size' bytes off the front.
                 * New free block header sits after: hdr(16) + payload(size) + ftr(8) */
                blk_init((uint64_t)cur + HDR_SIZE + size + FTR_SIZE, remainder - OVERHEAD, 0);
                cur->size = size;
                blk_ftr(cur)->size = size;
            }
            cur->used = 1;
            void *ret = blk_payload(cur);
            spinlock_irqrestore(&heap_lock, flags);
            return ret;
        }
        cur = blk_next(cur);
    }

    /* No suitable block found — expand heap (calls pmm_alloc_frame -> acquires pmm_lock) */
    struct heap_hdr *new_blk = heap_expand();
    if (new_blk == NULL) {
        spinlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    goto retry;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    unsigned long flags;
    spinlock_irqsave(&heap_lock, &flags);

    struct heap_hdr *hdr = hdr_from_payload(ptr);
    hdr->used = 0;

    /* Coalesce right: check the block immediately after */
    struct heap_hdr *next = blk_next(hdr);
    if ((uint64_t)next < heap_top && next->used == 0) {
        size_t merged = hdr->size + OVERHEAD + next->size;
        hdr->size = merged;
        blk_ftr(hdr)->size = merged;
    }

    /* Coalesce left: check footer of block immediately before */
    if ((uint64_t)hdr > HEAP_START) {
        struct heap_ftr *prev_ftr = (struct heap_ftr *)((uint8_t *)hdr - FTR_SIZE);
        struct heap_hdr *prev_hdr =
            (struct heap_hdr *)((uint8_t *)hdr - FTR_SIZE - prev_ftr->size - HDR_SIZE);
        if ((uint64_t)prev_hdr >= HEAP_START && prev_hdr->used == 0) {
            size_t merged = prev_hdr->size + OVERHEAD + hdr->size;
            prev_hdr->size = merged;
            blk_ftr(prev_hdr)->size = merged;
        }
    }

    spinlock_irqrestore(&heap_lock, flags);
}

size_t heap_free_bytes(void) {
    size_t free = 0;
    struct heap_hdr *cur = (struct heap_hdr *)HEAP_START;
    while ((uint64_t)cur < heap_top) {
        if (cur->used == 0) free += cur->size;
        cur = blk_next(cur);
    }
    return free;
}
