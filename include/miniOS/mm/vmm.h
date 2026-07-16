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

#ifndef _MINIOS_MM_VMM_H_
#define _MINIOS_MM_VMM_H_

#include <miniOS/types.h> 


#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_COW      (1ULL << 9)   /* software: copy-on-write pending */

/* ------------------------------------------------------------------ */
/*  Higher-half kernel virtual address layout                          */
/* ------------------------------------------------------------------ */
/* KERNEL_VMA: base of higher-half kernel window (PML4[256]).          *
 * Physical address P maps to virtual P + KERNEL_VMA.                 *
 * NOTE: Also defined as KERNEL_VMA equ in src/arch/x86_64/main64.asm *
 *       (ASM boot code cannot include C headers). Keep in sync.      */
#define KERNEL_VMA          0xFFFF800000000000ULL

/* HEAP_START: kernel heap virtual base (PML4[260]).                   *
 * src/kernel/mm/heap.c honours #ifndef HEAP_START so test builds     *
 * can override this value via -D on the compiler command line.       */
#ifndef HEAP_START
#define HEAP_START          0xFFFF820000000000ULL
#endif

/* HEAP_MAX: upper bound of heap (4 MiB above HEAP_START).            */
#ifndef HEAP_MAX
#define HEAP_MAX            (HEAP_START + 4ULL * 1024 * 1024)
#endif

/* DMA_BUFFER_VA: 4 KiB DMA bounce buffer virtual address (PML4[262]).*/
#define DMA_BUFFER_VA       0xFFFF830000000000ULL

/* GOP_FB_VA: linear framebuffer virtual base (PML4[264]).              *
 * Mapped at boot by gop_init_from_mb2() when CONSOLE_GOP is defined.  */
#define GOP_FB_VA           0xFFFF840000000000ULL

/* VGA_BUFFER_VA: text-mode VGA framebuffer virtual address.           *
 * Physical 0xB8000 mapped via the KERNEL_VMA identity window.        */
#define VGA_BUFFER_VA       (KERNEL_VMA + 0xB8000ULL)

/* ------------------------------------------------------------------ */
/*  x86_64 page size                                                   */
/* ------------------------------------------------------------------ */
/* PAGE_SIZE: 4 KiB frame size for this kernel (4-level paging).      */
#define PAGE_SIZE           4096ULL

/* ------------------------------------------------------------------ */
/*  Page-table index extraction (4-level paging, 9-bit indices)       */
/* ------------------------------------------------------------------ */
#define PML4_SHIFT      39   /* bits [47:39] index PML4 */
#define PDPT_SHIFT      30   /* bits [38:30] index PDPT */
#define PD_SHIFT        21   /* bits [29:21] index PD   */
#define PT_SHIFT        12   /* bits [20:12] index PT   */
#define PT_INDEX_MASK   0x1FFULL  /* 9-bit mask for any level index */

/* ------------------------------------------------------------------ */
/*  PTE physical address mask                                          */
/* ------------------------------------------------------------------ */
/* PTE_ADDR_MASK: bits [51:12] hold the physical frame address in a PTE. */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/**
 * vmm_map_page() - Map a 4 KiB virtual page to a physical frame.
 * @virt: Virtual address to map. Must be >= KERNEL_VMA (0xFFFF800000000000)
 *        for kernel mappings, or a valid user VA for user mappings.
 * @phys: Physical frame address (must be 4 KiB-aligned).
 * @flags: PTE flags: PAGE_PRESENT (bit 0), PAGE_WRITE (bit 1),
 *         PAGE_USER (bit 2). Combine as needed.
 *
 * Walks the 4-level page table (PML4 -> PDPT -> PD -> PT), allocating
 * missing intermediate levels via pmm_alloc_frame. All intermediate-level
 * entries are created with PAGE_PRESENT|PAGE_WRITE|PAGE_USER to allow
 * both kernel and user mappings to be installed. Flushes TLB via invlpg.
 *
 * Context: ISR-safe — pmm_alloc_frame uses spinlock_irqsave.
 * @return: 0 on success, -1 if pmm_alloc_frame returned 0 (OOM) while
 *          allocating an intermediate page table level.
 */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/**
 * vmm_unmap_page() - Remove a page mapping.
 * @virt: Virtual address of the page to unmap (4 KiB-aligned).
 *
 * Clears the leaf PTE to 0 (not-present) and flushes the TLB via invlpg.
 * Does NOT free the physical frame — that is the caller's responsibility.
 * Does NOT reclaim depopulated intermediate page table levels.
 */
void vmm_unmap_page(uint64_t virt);

/**
 * vmm_virt_to_phys() - Translate a virtual address to its physical frame address.
 * @virt: Virtual address to translate.
 *
 * Walks the 4-level page table via cr3 + KERNEL_VMA. Stops and returns 0
 * if any level's entry is not-present (bit 0 = 0).
 *
 * @return: Physical address (4 KiB-aligned) of the frame backing @virt,
 *          or 0 if the mapping does not exist at any page table level.
 */
uint64_t vmm_virt_to_phys(uint64_t virt);
uint64_t vmm_virt_to_pte(uint64_t virt);

/**
 * vmm_resolve_user_fault() - Demand-page handler for user #PF.
 * @cr2:        Faulting virtual address (from CR2 register).
 * @error_code: x86 page-fault error code pushed by CPU.
 *
 * Called from the #PF exception handler when bit 2 (U/S) of error_code is
 * set (user-mode fault). Checks whether the fault is a not-present fault
 * (bit 0 = 0) in a lazy mmap region. If so, allocates a physical frame,
 * zeros it, and installs the PTE with permissions derived from region.prot.
 * Handles SMP double-fault races by checking vmm_virt_to_phys() first.
 *
 * @return: 0 if the fault was resolved (iretq can resume execution),
 *         -1 if the fault is unresolvable (caller should deliver SIGSEGV).
 */
int vmm_resolve_user_fault(uint64_t cr2, uint64_t error_code);

/**
 * vmm_new_address_space() - Allocate a fresh per-process PML4.
 *
 * Higher half (kernel/heap/DMA/GOP/MMIO, PML4[256..511]) is copied by value
 * from the live PML4 so kernel mappings are identical in every process.
 * Low half (PML4[0..255], user code/stack/mmap) starts zeroed.
 *
 * @return: Physical address of the new PML4, or 0 on OOM.
 */
uint64_t vmm_new_address_space(void);

/**
 * vmm_free_address_space() - Free a process's private page tables.
 * @pml4_phys: PML4 physical address from vmm_new_address_space().
 *
 * Walks only the user low half, pmm_unref_frame()'ing every mapped leaf
 * page (frees private pages, decrements still-shared CoW pages) and freeing
 * every intermediate PDPT/PD/PT frame plus the PML4 itself. Never touches
 * the shared kernel high half.
 */
void vmm_free_address_space(uint64_t pml4_phys);

/* PML4-parameterized variants of the four core VMM primitives — operate on
 * an explicit address space instead of the live CR3. Used to build a new
 * process's page tables (fork) or inspect another process's memory (ptrace,
 * once implemented) without ever loading that process's CR3. */
int      vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_virt_to_phys_in(uint64_t pml4_phys, uint64_t virt);
uint64_t vmm_virt_to_pte_in(uint64_t pml4_phys, uint64_t virt);

#endif /* _MINIOS_MM_VMM_H_ */
