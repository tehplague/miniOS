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

#include <miniOS/mm/vmm.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/ipi/ipi.h>
#include <miniOS/sched/sched.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/types.h>
#include <string.h>

#ifndef PROT_WRITE
#define PROT_WRITE 2
#endif


/* Returns the virtual address of the kernel PML4 table.
 * At boot, cr3 holds the physical address of page_table_l4.
 * Since 0..1GB physical is mapped at KERNEL_VMA, virt = phys + KERNEL_VMA. */
static inline uint64_t *virt_to_pml4_virt(void)
{
    uint64_t phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(phys));
    return (uint64_t *)(phys + KERNEL_VMA);
}

/* Walk one level of the page table hierarchy.
 * If the entry at table_virt[index] is present, return its child table virtual address.
 * Otherwise allocate a new frame via pmm_alloc_frame(), zero it, install the entry,
 * and return the new table's virtual address. Returns NULL on OOM. */
static uint64_t *get_or_create_entry(uint64_t *table_virt, int index, uint64_t flags)
{
    if (table_virt[index] & PAGE_PRESENT) {
        return (uint64_t *)((table_virt[index] & PTE_ADDR_MASK) + KERNEL_VMA);
    }

    uint64_t frame_phys = pmm_alloc_frame();
    if (frame_phys == 0) {
        return NULL; /* OOM */
    }

    /* Zero the new page table page */
    memset((void *)(frame_phys + KERNEL_VMA), 0, PAGE_SIZE);

    /* Install the entry: physical address | caller flags | present | writable */
    table_virt[index] = frame_phys | flags | PAGE_PRESENT | PAGE_WRITE;

    return (uint64_t *)(frame_phys + KERNEL_VMA);
}

/**
 * vmm_map_page_in() - Install a PTE mapping virt -> phys in an explicit PML4.
 * @pml4_phys: Physical address of the target PML4 (not necessarily live in CR3).
 *
 * Same walk as vmm_map_page(), but against @pml4_phys via the KERNEL_VMA
 * direct-map alias rather than the live CR3 — lets a process's page tables be
 * built (fork) or inspected (ptrace PEEK/POKE, once implemented) without ever
 * loading that process's CR3. Still issues invlpg/shootdown for @virt; those
 * are only meaningful if @pml4_phys happens to be live somewhere, but are
 * cheap no-ops otherwise.
 */
int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
{
    int pml4_i = (virt >> PML4_SHIFT) & PT_INDEX_MASK;
    int pml3_i = (virt >> PDPT_SHIFT) & PT_INDEX_MASK;
    int pml2_i = (virt >>   PD_SHIFT) & PT_INDEX_MASK;
    int pml1_i = (virt >>   PT_SHIFT) & PT_INDEX_MASK;

    uint64_t *pml4 = (uint64_t *)(pml4_phys + KERNEL_VMA);

    uint64_t *pml3 = get_or_create_entry(pml4, pml4_i, flags);
    if (!pml3) return -1;

    uint64_t *pml2 = get_or_create_entry(pml3, pml3_i, flags);
    if (!pml2) return -1;

    uint64_t *pml1 = get_or_create_entry(pml2, pml2_i, flags);
    if (!pml1) return -1;

    /* Install the final PTE */
    pml1[pml1_i] = (phys & PTE_ADDR_MASK) | flags | PAGE_PRESENT;

    /* Flush TLB locally and on all other CPUs (SMP TLB coherency).
     * Over-broad if @pml4_phys isn't live anywhere, but harmless. */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    ipi_tlb_shootdown(virt);

    return 0;
}

/**
 * vmm_map_page() - Install a PTE mapping virt -> phys in the live address space.
 * @virt: Virtual address (>= KERNEL_VMA for kernel; user VA for ring-3 pages).
 * @phys: Physical frame address (4 KiB-aligned).
 * @flags: PTE attribute bits (PAGE_PRESENT, PAGE_WRITE, PAGE_USER).
 *
 * Thin wrapper over vmm_map_page_in() using the live CR3.
 *
 * @return: 0 on success, -1 on OOM (pmm_alloc_frame returned 0).
 */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return vmm_map_page_in(cr3, virt, phys, flags);
}

/**
 * vmm_unmap_page_in() - Clear a leaf PTE in an explicit PML4 and flush TLB.
 */
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt)
{
    int pml4_i = (virt >> PML4_SHIFT) & PT_INDEX_MASK;
    int pml3_i = (virt >> PDPT_SHIFT) & PT_INDEX_MASK;
    int pml2_i = (virt >>   PD_SHIFT) & PT_INDEX_MASK;
    int pml1_i = (virt >>   PT_SHIFT) & PT_INDEX_MASK;

    uint64_t *pml4 = (uint64_t *)(pml4_phys + KERNEL_VMA);

    if (!(pml4[pml4_i] & PAGE_PRESENT)) return;
    uint64_t *pml3 = (uint64_t *)((pml4[pml4_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml3[pml3_i] & PAGE_PRESENT)) return;
    uint64_t *pml2 = (uint64_t *)((pml3[pml3_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml2[pml2_i] & PAGE_PRESENT)) return;
    uint64_t *pml1 = (uint64_t *)((pml2[pml2_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    /* Clear the PTE */
    pml1[pml1_i] = 0;

    /* Flush TLB locally and on all other CPUs (SMP TLB coherency) */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    ipi_tlb_shootdown(virt);
}

/**
 * vmm_unmap_page() - Clear a leaf PTE and flush TLB.
 * @virt: Virtual address of the page to unmap.
 *
 * Walks all four page table levels. Writes 0 to the leaf PT entry (clears
 * present bit). Issues invlpg on @virt. Does not free intermediate levels
 * or the physical frame.
 */
void vmm_unmap_page(uint64_t virt)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_unmap_page_in(cr3, virt);
}

/**
 * vmm_virt_to_phys_in() - Walk an explicit PML4 to resolve a virtual address.
 */
uint64_t vmm_virt_to_phys_in(uint64_t pml4_phys, uint64_t virt)
{
    int pml4_i = (virt >> PML4_SHIFT) & PT_INDEX_MASK;
    int pml3_i = (virt >> PDPT_SHIFT) & PT_INDEX_MASK;
    int pml2_i = (virt >>   PD_SHIFT) & PT_INDEX_MASK;
    int pml1_i = (virt >>   PT_SHIFT) & PT_INDEX_MASK;

    uint64_t *pml4 = (uint64_t *)(pml4_phys + KERNEL_VMA);

    if (!(pml4[pml4_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml3 = (uint64_t *)((pml4[pml4_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml3[pml3_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml2 = (uint64_t *)((pml3[pml3_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml2[pml2_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml1 = (uint64_t *)((pml2[pml2_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml1[pml1_i] & PAGE_PRESENT)) return 0;
    return pml1[pml1_i] & PTE_ADDR_MASK;
}

/**
 * vmm_virt_to_phys() - Walk page tables to resolve a virtual address.
 * @virt: Virtual address to translate.
 *
 * Thin wrapper over vmm_virt_to_phys_in() using the live CR3.
 *
 * @return: Physical frame address (4 KiB-aligned), or 0 if not mapped.
 */
uint64_t vmm_virt_to_phys(uint64_t virt)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return vmm_virt_to_phys_in(cr3, virt);
}

/* Returns the raw PTE for @virt in an explicit PML4, or 0 if not present. */
uint64_t vmm_virt_to_pte_in(uint64_t pml4_phys, uint64_t virt)
{
    int pml4_i = (virt >> PML4_SHIFT) & PT_INDEX_MASK;
    int pml3_i = (virt >> PDPT_SHIFT) & PT_INDEX_MASK;
    int pml2_i = (virt >>   PD_SHIFT) & PT_INDEX_MASK;
    int pml1_i = (virt >>   PT_SHIFT) & PT_INDEX_MASK;

    uint64_t *pml4 = (uint64_t *)(pml4_phys + KERNEL_VMA);

    if (!(pml4[pml4_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml3 = (uint64_t *)((pml4[pml4_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml3[pml3_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml2 = (uint64_t *)((pml3[pml3_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml2[pml2_i] & PAGE_PRESENT)) return 0;
    uint64_t *pml1 = (uint64_t *)((pml2[pml2_i] & PTE_ADDR_MASK) + KERNEL_VMA);

    if (!(pml1[pml1_i] & PAGE_PRESENT)) return 0;
    return pml1[pml1_i];
}

/* Returns the raw PTE for @virt, or 0 if not present. Callers can test
 * PAGE_WRITE to distinguish writable (data/BSS) from read-only (text). */
uint64_t vmm_virt_to_pte(uint64_t virt)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return vmm_virt_to_pte_in(cr3, virt);
}

/**
 * vmm_new_address_space() - Allocate a fresh per-process PML4.
 *
 * Copies PML4 entries 256..511 (the entire canonical higher half) by value
 * from the currently-live PML4 into the new one, so every kernel mapping
 * (kernel image, heap, DMA bounce buffer, GOP framebuffer, LAPIC/IOAPIC MMIO)
 * is identically visible regardless of which process's PML4 is loaded in
 * CR3 — all process PML4s share the same underlying PDPT/PD/PT chain for
 * these entries, so kernel-side growth (e.g. heap.c mapping a new page)
 * automatically stays visible to every process without re-syncing.
 *
 * Invariant this relies on: every kernel top-level region (indices 256, 260,
 * 262, 264 — KERNEL_VMA/VGA, HEAP_START, DMA_BUFFER_VA, GOP_FB_VA) must
 * already have its first page mapped by the time the first process is
 * created, so its PML4 entry exists to copy. True today — all of them are
 * touched during early boot, before the first sched_create_user_task() call.
 *
 * Entries 0..255 (user low half) are left zero — the caller builds those
 * fresh via vmm_map_page_in().
 *
 * @return: Physical address of the new PML4, or 0 on OOM.
 */
uint64_t vmm_new_address_space(void)
{
    uint64_t new_phys = pmm_alloc_frame();
    if (!new_phys)
        return 0;

    uint64_t *new_pml4 = (uint64_t *)(new_phys + KERNEL_VMA);
    memset(new_pml4, 0, PAGE_SIZE);

    uint64_t *live_pml4 = virt_to_pml4_virt();
    for (int i = 256; i < 512; i++)
        new_pml4[i] = live_pml4[i];

    return new_phys;
}

/**
 * vmm_free_address_space() - Tear down a process's private page tables.
 * @pml4_phys: PML4 physical address returned by vmm_new_address_space().
 *
 * Walks only the user low half (PML4 indices 0..255 — the per-process
 * range; indices 256..511 are the shared kernel half and are never touched
 * or freed here). For every present leaf PTE, calls pmm_unref_frame() —
 * private pages (refcount 1) are freed; CoW-shared pages still held by
 * another process are just decremented. Also frees the intermediate
 * PDPT/PD/PT frames (always private to this address space; never shared
 * across processes), then the PML4 frame itself.
 */
void vmm_free_address_space(uint64_t pml4_phys)
{
    uint64_t *pml4 = (uint64_t *)(pml4_phys + KERNEL_VMA);

    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[i4] & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)(pdpt_phys + KERNEL_VMA);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[i3] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)(pd_phys + KERNEL_VMA);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PAGE_PRESENT)) continue;
                uint64_t pt_phys = pd[i2] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)(pt_phys + KERNEL_VMA);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(pt[i1] & PAGE_PRESENT)) continue;
                    pmm_unref_frame(pt[i1] & PTE_ADDR_MASK);
                }
                pmm_free_frame(pt_phys);
            }
            pmm_free_frame(pd_phys);
        }
        pmm_free_frame(pdpt_phys);
    }
    pmm_free_frame(pml4_phys);
}

int vmm_resolve_user_fault(uint64_t cr2, uint64_t error_code)
{
    /* Protection violation: check for CoW before giving up. */
    if (error_code & 0x1) {
        uint64_t page_va = cr2 & ~(PAGE_SIZE - 1ULL);
        uint64_t pte     = vmm_virt_to_pte(page_va);
        if (!(pte & PAGE_COW))
            return -1;  /* genuine protection fault — kill task */
        uint64_t old_phys = pte & PTE_ADDR_MASK;
        uint64_t new_phys = pmm_alloc_frame();
        if (!new_phys) return -1;
        uint8_t *src = (uint8_t *)(old_phys + KERNEL_VMA);
        uint8_t *dst = (uint8_t *)(new_phys + KERNEL_VMA);
        for (uint64_t b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];
        if (vmm_map_page(page_va, new_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) < 0) {
            pmm_free_frame(new_phys);
            return -1;
        }
        /* This thread no longer references old_phys — drop its share. Frees
         * it if we were the last owner, or just decrements if a sibling
         * process/thread from an earlier fork still shares it. */
        pmm_unref_frame(old_phys);
        return 0;
    }

    uint64_t page_va = cr2 & ~(PAGE_SIZE - 1ULL);

    /* SMP guard: another CPU may have already faulted this page in. */
    if (vmm_virt_to_phys(page_va) != 0)
        return 0;

    struct thread *t = sched_current();

    /* --- Lazy brk (heap) --- */
    uint64_t brk_base = *THREAD_BRK_BASE_PTR(t);
    uint64_t brk      = *THREAD_BRK_PTR(t);
    if (brk_base != 0 && page_va >= brk_base && page_va < brk) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0)
            return -1;
        uint8_t *kva = (uint8_t *)(phys + KERNEL_VMA);
        for (uint64_t b = 0; b < PAGE_SIZE; b++)
            kva[b] = 0;
        if (vmm_map_page(page_va, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_frame(phys);
            return -1;
        }
        return 0;
    }

    /* --- Lazy mmap regions (anonymous and file-backed) --- */
    mmap_region_t *mmapr = THREAD_MMAPR(t);

    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        mmap_region_t *r = &mmapr[i];
        if (r->virt_addr == 0 || !r->lazy)
            continue;
        if (page_va < r->virt_addr || page_va >= r->virt_addr + r->len)
            continue;

        uint64_t phys = pmm_alloc_frame();
        if (phys == 0)
            return -1;

        uint8_t *kva = (uint8_t *)(phys + KERNEL_VMA);

        if (r->region_type == MMAP_REGION_FILE && r->file_ops && r->file_ops->read) {
            /* Read file content into the page. */
            uint64_t page_idx  = (page_va - r->virt_addr) / PAGE_SIZE;
            uint64_t file_off  = r->file_offset + page_idx * PAGE_SIZE;
            int nr = 0;
            if (file_off < r->file_size) {
                uint32_t to_read = PAGE_SIZE;
                if (file_off + to_read > r->file_size)
                    to_read = (uint32_t)(r->file_size - file_off);
                nr = r->file_ops->read(r->file_inode, file_off, kva, to_read);
                if (nr < 0) nr = 0;
            }
            /* Zero-pad the rest of the page. */
            for (uint64_t b = (uint64_t)nr; b < PAGE_SIZE; b++)
                kva[b] = 0;
        } else {
            for (uint64_t b = 0; b < PAGE_SIZE; b++)
                kva[b] = 0;
        }

        uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
        if (r->prot & PROT_WRITE)
            pte_flags |= PAGE_WRITE;

        if (vmm_map_page(page_va, phys, pte_flags) != 0) {
            pmm_free_frame(phys);
            return -1;
        }
        return 0;
    }

    return -1;  /* no covering lazy region — unresolvable */
}
