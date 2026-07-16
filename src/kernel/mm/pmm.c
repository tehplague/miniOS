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

#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/types.h>
#include <miniOS/arch/x86_64/spinlock.h>

/* -----------------------------------------------------------------------
 * Multiboot 2 structure definitions (inline — no external MB2 header)
 * --------------------------------------------------------------------- */

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct mb2_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

#define MB2_TAG_MMAP            6
#define MULTIBOOT_MEMORY_AVAILABLE 1

/* -----------------------------------------------------------------------
 * Bitmap physical frame allocator
 *
 * Bitmap is placed in physical RAM immediately after the kernel image at
 * runtime, sized to cover only the detected physical address space.
 * Each bit = one 4 KiB frame.  bit==1 means FREE, bit==0 means USED.
 * --------------------------------------------------------------------- */

static uint8_t  *g_pmm_bitmap  = NULL;
static uint64_t  g_max_frames  = 0;   /* total frames covered by bitmap */
static uint64_t  g_bitmap_bytes = 0;  /* byte length of bitmap */

/* One refcount byte per frame, placed in physical RAM right after the bitmap
 * (same "within the boot identity map" trick pmm_init uses for the bitmap
 * itself). Index 0 unused (frame 0 is never allocated). A frame's count is
 * set to 1 by pmm_alloc_frame(); pmm_ref_frame()/pmm_unref_frame() let CoW
 * fork share a frame between two page tables without pmm_free_frame() being
 * called while the other side still uses it. */
static uint8_t  *g_pmm_refcount = NULL;
static uint64_t  g_refcount_bytes = 0;

static uint64_t total_free   = 0;
static uint64_t total_frames = 0;

static spinlock_t pmm_lock = SPINLOCK_INIT;

/* -----------------------------------------------------------------------
 * Alignment helper
 * --------------------------------------------------------------------- */
static inline uint64_t align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

/* -----------------------------------------------------------------------
 * Internal: mark a single frame as free
 * --------------------------------------------------------------------- */
static void pmm_set_free(uint64_t frame_idx)
{
    if (frame_idx >= g_max_frames)
        return;
    g_pmm_bitmap[frame_idx / 8] |= (uint8_t)(1u << (frame_idx % 8));
}

/* -----------------------------------------------------------------------
 * Internal: mark a single frame as used
 * --------------------------------------------------------------------- */
static void pmm_set_used(uint64_t frame_idx)
{
    if (frame_idx >= g_max_frames)
        return;
    g_pmm_bitmap[frame_idx / 8] &= (uint8_t)~(1u << (frame_idx % 8));
}

/* -----------------------------------------------------------------------
 * Internal: free all 4 KiB-aligned frames in [base, base+length)
 * --------------------------------------------------------------------- */
static void pmm_free_range(uint64_t base, uint64_t length)
{
    uint64_t start = align_up(base, PAGE_SIZE);
    uint64_t end   = (base + length) & ~(PAGE_SIZE - 1);

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint64_t idx = addr >> 12;
        if (idx >= g_max_frames)
            break;
        pmm_set_free(idx);
        total_free++;
        total_frames++;
    }
}

/* -----------------------------------------------------------------------
 * Linker-provided symbol: end of kernel image (virtual address)
 * --------------------------------------------------------------------- */
extern uint64_t __image_end;

/**
 * pmm_init() - Parse Multiboot 2 memory map and initialise the bitmap.
 * @mb_info_phys: Physical address of MB2 struct; KERNEL_VMA added internally.
 *
 * Two-pass init:
 *   Pass 1 — scan mmap to find the highest available physical address,
 *             then size and place the bitmap in physical RAM right after
 *             the kernel image (within the boot identity map, so the
 *             virtual address phys+KERNEL_VMA is immediately valid).
 *   Pass 2 — mark available regions free; mark kernel+bitmap frames used.
 *
 * For a 64 MiB VM the bitmap is ~2 KiB instead of the 128 KiB that a
 * static 4 GiB-ceiling array would require.
 */
void pmm_init(uint64_t mb_info_phys)
{
    uint64_t mb_info_virt = mb_info_phys + KERNEL_VMA;

    /* ---- Pass 1: find highest available physical address -------------- */
    uint64_t highest_avail = 0;
    {
        uint8_t *ptr = (uint8_t *)(mb_info_virt + 8);
        while (1) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0)
                break;
            if (tag->type == MB2_TAG_MMAP) {
                struct mb2_tag_mmap *mt = (struct mb2_tag_mmap *)tag;
                uint32_t esize = mt->entry_size;
                uint8_t *ep = (uint8_t *)tag + sizeof(struct mb2_tag_mmap);
                uint8_t *ee = (uint8_t *)tag + tag->size;
                while (ep < ee) {
                    struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)ep;
                    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                        uint64_t end = e->base_addr + e->length;
                        if (end > highest_avail)
                            highest_avail = end;
                    }
                    ep += esize;
                }
            }
            ptr += align_up(tag->size, 8);
        }
    }

    /* ---- Size the bitmap and place it after the kernel image ---------- */
    g_max_frames   = align_up(highest_avail, PAGE_SIZE) / PAGE_SIZE;
    g_bitmap_bytes = align_up((g_max_frames + 7) / 8, PAGE_SIZE);

    /* bitmap_phys is within the first 1 GiB identity map, so
     * phys + KERNEL_VMA is a valid virtual address right now. */
    uint64_t kernel_end_phys = (uint64_t)&__image_end - KERNEL_VMA;
    uint64_t bitmap_phys     = align_up(kernel_end_phys, PAGE_SIZE);
    g_pmm_bitmap             = (uint8_t *)(bitmap_phys + KERNEL_VMA);

    /* Zero-init: all frames start as USED (bit==0). */
    for (uint64_t i = 0; i < g_bitmap_bytes; i++)
        g_pmm_bitmap[i] = 0;

    /* Refcount array immediately follows the bitmap, same placement trick. */
    g_refcount_bytes         = align_up(g_max_frames, PAGE_SIZE);
    uint64_t refcount_phys   = align_up(bitmap_phys + g_bitmap_bytes, PAGE_SIZE);
    g_pmm_refcount           = (uint8_t *)(refcount_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < g_refcount_bytes; i++)
        g_pmm_refcount[i] = 0;

    /* ---- Pass 2: mark available regions free -------------------------- */
    {
        uint8_t *ptr = (uint8_t *)(mb_info_virt + 8);
        while (1) {
            struct mb2_tag *tag = (struct mb2_tag *)ptr;
            if (tag->type == 0)
                break;
            if (tag->type == MB2_TAG_MMAP) {
                struct mb2_tag_mmap *mt = (struct mb2_tag_mmap *)tag;
                uint32_t esize = mt->entry_size;
                uint8_t *ep = (uint8_t *)tag + sizeof(struct mb2_tag_mmap);
                uint8_t *ee = (uint8_t *)tag + tag->size;
                while (ep < ee) {
                    struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)ep;
                    if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
                        pmm_free_range(e->base_addr, e->length);
                    ep += esize;
                }
            }
            ptr += align_up(tag->size, 8);
        }
    }

    /* Mark frame 0 as used (prevent null-pointer aliasing). */
    if (g_pmm_bitmap[0] & 1) {
        g_pmm_bitmap[0] &= ~(uint8_t)1;
        total_free--;
    }

    /* Mark kernel image + bitmap + refcount array pages as used in one pass. */
    uint64_t reserved_end = align_up(refcount_phys + g_refcount_bytes, PAGE_SIZE);
    for (uint64_t addr = 0; addr < reserved_end; addr += PAGE_SIZE) {
        uint64_t idx = addr >> 12;
        if (idx >= g_max_frames)
            break;
        if (g_pmm_bitmap[idx / 8] & (uint8_t)(1u << (idx % 8))) {
            g_pmm_bitmap[idx / 8] &= (uint8_t)~(1u << (idx % 8));
            if (total_free > 0)
                total_free--;
        }
    }
}

/**
 * pmm_alloc_frame() - Return the physical address of a free frame.
 *
 * Linear scan of g_pmm_bitmap looking for any non-zero byte. Within the
 * found byte, bit-scans to the lowest set bit, clears it (USED), and
 * returns frame_idx * PAGE_SIZE.
 *
 * @return: 4 KiB-aligned physical address, or 0 on OOM.
 */
uint64_t pmm_alloc_frame(void)
{
    unsigned long flags;
    spinlock_irqsave(&pmm_lock, &flags);

    uint64_t result = 0;
    for (uint64_t i = 0; i < g_bitmap_bytes; i++) {
        if (g_pmm_bitmap[i] == 0)
            continue;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (g_pmm_bitmap[i] & (uint8_t)(1u << bit)) {
                g_pmm_bitmap[i] &= (uint8_t)~(1u << bit);
                if (total_free > 0)
                    total_free--;
                result = (i * 8 + bit) * PAGE_SIZE;
                g_pmm_refcount[i * 8 + bit] = 1;
                goto pmm_alloc_done;
            }
        }
    }
pmm_alloc_done:
    spinlock_irqrestore(&pmm_lock, flags);
    return result; /* 0 = OOM */
}

/**
 * pmm_free_frame() - Mark a physical frame as free.
 * @phys: Physical address of frame (4 KiB-aligned).
 *
 * Guards against double-free: if the bit is already set, returns without
 * modifying total_free.
 */
void pmm_free_frame(uint64_t phys)
{
    uint64_t idx = phys >> 12;
    if (idx == 0 || idx >= g_max_frames)
        return;

    unsigned long flags;
    spinlock_irqsave(&pmm_lock, &flags);

    if (!(g_pmm_bitmap[idx / 8] & (uint8_t)(1u << (idx % 8)))) {
        g_pmm_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
        total_free++;
    }

    spinlock_irqrestore(&pmm_lock, flags);
}

/**
 * pmm_ref_frame() - Add an extra owner to an already-allocated frame.
 */
void pmm_ref_frame(uint64_t phys)
{
    uint64_t idx = phys >> 12;
    if (idx == 0 || idx >= g_max_frames)
        return;

    unsigned long flags;
    spinlock_irqsave(&pmm_lock, &flags);
    if (g_pmm_refcount[idx] < 255)
        g_pmm_refcount[idx]++;
    spinlock_irqrestore(&pmm_lock, flags);
}

/**
 * pmm_unref_frame() - Drop one owner of a frame; frees it at refcount 0.
 */
void pmm_unref_frame(uint64_t phys)
{
    uint64_t idx = phys >> 12;
    if (idx == 0 || idx >= g_max_frames)
        return;

    unsigned long flags;
    spinlock_irqsave(&pmm_lock, &flags);

    if (g_pmm_refcount[idx] > 0)
        g_pmm_refcount[idx]--;

    if (g_pmm_refcount[idx] == 0 &&
        !(g_pmm_bitmap[idx / 8] & (uint8_t)(1u << (idx % 8)))) {
        g_pmm_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
        total_free++;
    }

    spinlock_irqrestore(&pmm_lock, flags);
}

/**
 * pmm_alloc_contiguous() - Allocate n_frames physically contiguous frames.
 * @n_frames: Number of consecutive 4 KiB frames required (must be > 0).
 *
 * Linear scan for the first run of n_frames consecutive free bits.
 * O(n) in the bitmap size — suitable for infrequent DMA allocations.
 *
 * @return: Physical address of the first frame, or 0 on failure.
 */
uint64_t pmm_alloc_contiguous(size_t n_frames)
{
    if (n_frames == 0 || n_frames > g_max_frames)
        return 0;

    unsigned long flags;
    spinlock_irqsave(&pmm_lock, &flags);

    uint64_t run_start = 0;
    uint64_t run_len   = 0;
    uint64_t result    = 0;

    for (uint64_t idx = 1; idx < g_max_frames; idx++) {
        int free = (g_pmm_bitmap[idx / 8] >> (idx % 8)) & 1;
        if (free) {
            if (run_len == 0)
                run_start = idx;
            if (++run_len == n_frames) {
                for (uint64_t j = run_start; j <= idx; j++) {
                    g_pmm_bitmap[j / 8] &= (uint8_t)~(1u << (j % 8));
                    if (total_free > 0)
                        total_free--;
                    g_pmm_refcount[j] = 1;
                }
                result = run_start * PAGE_SIZE;
                break;
            }
        } else {
            run_len = 0;
        }
    }

    spinlock_irqrestore(&pmm_lock, flags);
    return result;
}

/**
 * pmm_free_contiguous() - Return n_frames contiguous frames to the free pool.
 * @phys:     Physical address of the first frame (4 KiB-aligned).
 * @n_frames: Number of frames to free.
 */
void pmm_free_contiguous(uint64_t phys, size_t n_frames)
{
    for (size_t i = 0; i < n_frames; i++)
        pmm_free_frame(phys + (uint64_t)i * PAGE_SIZE);
}

/**
 * pmm_free_count() - Return the number of free frames.
 */
uint64_t pmm_free_count(void)
{
    return total_free;
}

uint64_t pmm_total_count(void)
{
    return total_frames;
}
