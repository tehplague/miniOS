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

#include <miniOS/syscall.h>
#include <miniOS/sched/sched.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/ipi/ipi.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/types.h>
#include <string.h>
#include "syscall_internal.h"

#ifndef MINI_OS_DEBUG_SYSCALL_MM
#define MINI_OS_DEBUG_SYSCALL_MM 0
#endif

#if MINI_OS_DEBUG_SYSCALL_MM
#define SYSCALL_MM_DEBUG_PRINT(...) printk(__VA_ARGS__)
#else
#define SYSCALL_MM_DEBUG_PRINT(...) do { } while (0)
#endif

/* PROT and MAP flag constants — match Linux x86-64 / Newlib sys/mman.h values */
#ifndef PROT_READ
#define PROT_READ    1
#define PROT_WRITE   2
#define PROT_NONE    0
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#endif

/* TSC frequency and boot TSC value — set by lapic_timer_init() in lapic.c.
   miniOS_tsc_hz is always non-zero after boot (2 GHz fallback ensures the TSC path
   is always used). */
extern uint64_t miniOS_tsc_hz;
extern uint64_t miniOS_tsc_boot;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int64_t sys_brk(uint64_t new_brk) {
    struct thread *t = sched_current();

    /* Use THREAD_BRK_PTR to support shared brk in thread groups */
    uint64_t *brk_ptr = THREAD_BRK_PTR(t);
    uint64_t old_brk = *brk_ptr;
    if (old_brk == 0)
        return -22; /* no ELF loaded; shouldn't happen */

    SYSCALL_MM_DEBUG_PRINT("sys_brk: new_brk=0x%lx current=0x%lx\n", new_brk, old_brk);

    if (new_brk == 0)
        return (int64_t)old_brk;

    if (new_brk <= old_brk) {
        uint64_t free_start = (new_brk + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
        uint64_t free_end   = (old_brk + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
        for (uint64_t pg = free_start; pg < free_end; pg += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(pg);
            if (phys) {
                vmm_unmap_page(pg);
                pmm_free_frame(phys);
                ipi_tlb_shootdown(pg);
            }
        }
        *brk_ptr = new_brk;
        return (int64_t)new_brk;
    }

    if (new_brk >= USER_STACK_TOP)
        return (int64_t)old_brk;

    /* Lazy brk: just advance the pointer; pages are demand-paged on first access. */
    *brk_ptr = new_brk;
    return (int64_t)new_brk;
}

static int64_t sys_mmap(uint64_t addr_hint, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)addr_hint;

    struct thread *t = sched_current();

    if (len == 0)
        return -22;

    if (flags & MAP_ANONYMOUS) {
        if (!(flags & MAP_PRIVATE))
            return -22;

        uint64_t rounded_len = (len + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
        if (rounded_len > 1073741824ULL)
            return -22;

        uint64_t page_count = rounded_len / PAGE_SIZE;
        uint64_t *mmap_next_ptr = THREAD_MMAP_PTR(t);
        uint64_t alloc_base = *mmap_next_ptr - rounded_len;
        if (alloc_base >= *mmap_next_ptr || alloc_base < 0x700000000ULL)
            return -12;

        mmap_region_t *anon_mmapr = THREAD_MMAPR(t);
        int slot = -1;
        for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
            if (anon_mmapr[i].virt_addr == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0)
            return -12;

        anon_mmapr[slot].virt_addr   = alloc_base;
        anon_mmapr[slot].len         = rounded_len;
        anon_mmapr[slot].prot        = (int)prot;
        anon_mmapr[slot].region_type = MMAP_REGION_ANON;
        anon_mmapr[slot].lazy        = 1;
        *mmap_next_ptr = alloc_base;
        return (int64_t)alloc_base;
    }

    if (offset != 0 || !(flags & MAP_PRIVATE))
        return -22;
    if (fd < VFS_FIRST_OPEN_FD || fd >= VFS_MAX_FDS)
        return -22;
    vfs_file_t *t_fdt = THREAD_FDT(t);
    if (!t_fdt[fd].in_use)
        return -22;

    uint64_t file_size = t_fdt[fd].size;
    uint64_t len_to_map = (len < file_size) ? len : file_size;
    if (len_to_map == 0)
        return -22;

    uint64_t rounded_len = (len_to_map + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t *file_mmap_next_ptr = THREAD_MMAP_PTR(t);
    uint64_t alloc_base  = *file_mmap_next_ptr - rounded_len;
    if (alloc_base >= *file_mmap_next_ptr || alloc_base < 0x700000000ULL)
        return -12;

    /* Lazy file mmap: record metadata; pages are demand-paged on first access. */
    mmap_region_t *file_mmapr = THREAD_MMAPR(t);
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (file_mmapr[i].virt_addr == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -12;

    file_mmapr[slot].virt_addr   = alloc_base;
    file_mmapr[slot].len         = rounded_len;
    file_mmapr[slot].prot        = (int)prot;
    file_mmapr[slot].region_type = MMAP_REGION_FILE;
    file_mmapr[slot].lazy        = 1;
    file_mmapr[slot].file_inode  = t_fdt[fd].inode;
    file_mmapr[slot].file_offset = 0;   /* offset arg validated == 0 above */
    file_mmapr[slot].file_size   = file_size;
    file_mmapr[slot].file_ops    = t_fdt[fd].ops;
    *file_mmap_next_ptr = alloc_base;
    return (int64_t)alloc_base;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    if (addr & (PAGE_SIZE - 1)) return -22;  /* EINVAL: addr not page-aligned */
    if (len == 0) return 0;

    uint64_t rounded_len = (len + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t page_count  = rounded_len / PAGE_SIZE;

    uint64_t pte_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) pte_flags |= PAGE_WRITE;

    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t page_va = addr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(page_va);
        if (phys == 0) continue;
        vmm_map_page(page_va, phys, pte_flags);
        ipi_tlb_shootdown(page_va);
    }

    /* Update prot field in any mmap region that covers this exact range */
    struct thread *t = sched_current();
    mmap_region_t *mmapr = THREAD_MMAPR(t);
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (mmapr[i].virt_addr == 0) continue;
        if (addr >= mmapr[i].virt_addr &&
            addr + rounded_len <= mmapr[i].virt_addr + mmapr[i].len) {
            mmapr[i].prot = (int)prot;
            break;
        }
    }
    return 0;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len) {
    struct thread *t = sched_current();
    mmap_region_t *mmapr = THREAD_MMAPR(t);

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
        if (mmapr[i].virt_addr == addr && mmapr[i].len == len) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -22;

    uint64_t rounded_len = (len + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t page_count  = rounded_len / PAGE_SIZE;

    for (uint64_t i = 0; i < page_count; i++) {
        uint64_t page_va = addr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys(page_va);
        if (phys != 0) {
            vmm_unmap_page(page_va);
            pmm_free_frame(phys);
            ipi_tlb_shootdown(page_va);
        }
    }

    mmapr[slot].virt_addr = 0;
    mmapr[slot].len       = 0;
    mmapr[slot].prot      = 0;
    return 0;
}

int64_t syscall_dispatch_mm(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                   uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    switch (nr) {
    case 12:
    case SYS_brk:
        return sys_brk(arg1);

    case SYS_mmap:
        return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);

    case SYS_mprotect:
        return sys_mprotect(arg1, arg2, arg3);

    case SYS_munmap:
        return sys_munmap(arg1, arg2);

    case SYS_gettimeofday: {
        typedef struct { uint64_t tv_sec; uint64_t tv_usec; } mini_timeval_t;
        mini_timeval_t *tv = (mini_timeval_t *)(uintptr_t)arg1;
        if (!tv)
            return -14;  /* EFAULT */
        if (miniOS_tsc_hz != 0) {
            uint64_t delta = rdtsc() - miniOS_tsc_boot;
            tv->tv_sec  = delta / miniOS_tsc_hz;
            tv->tv_usec = ((delta % miniOS_tsc_hz) * 1000000ULL) / miniOS_tsc_hz;
        } else {
            extern volatile uint64_t lapic_tick_count;
            uint64_t ticks = lapic_tick_count;
            tv->tv_sec  = ticks / 100;
            tv->tv_usec = (ticks % 100) * 10000ULL;
        }
        return 0;
    }

    case SYS_clock_gettime: {
        int clockid = (int)arg1;
        if (clockid != 0 && clockid != 1)
            return -22;
        typedef struct { uint64_t tv_sec; uint64_t tv_nsec; } mini_timespec_t;
        mini_timespec_t *tp = (mini_timespec_t *)(uintptr_t)arg2;
        if (!tp)
            return -22;

        if (miniOS_tsc_hz != 0) {
            uint64_t delta = rdtsc() - miniOS_tsc_boot;
            tp->tv_sec  = delta / miniOS_tsc_hz;
            tp->tv_nsec = ((delta % miniOS_tsc_hz) * 1000000000ULL) / miniOS_tsc_hz;
        } else {
            /* LAPIC fallback (100 Hz, 10ms resolution) — should never be reached
             * since miniOS_tsc_hz is always non-zero after calibration. */
            extern volatile uint64_t lapic_tick_count;
            uint64_t ticks = lapic_tick_count;
            tp->tv_sec  = ticks / 100;
            tp->tv_nsec = (ticks % 100) * 10000000ULL;
        }
        return 0;
    }

    case SYS_sysinfo: {
        /* Mirror of Linux struct sysinfo (x86-64 layout, mem_unit=1 → bytes). */
        struct {
            long           uptime;
            unsigned long  loads[3];
            unsigned long  totalram;
            unsigned long  freeram;
            unsigned long  sharedram;
            unsigned long  bufferram;
            unsigned long  totalswap;
            unsigned long  freeswap;
            unsigned short procs;
            char           _pad[6];
            unsigned long  totalhigh;
            unsigned long  freehigh;
            unsigned int   mem_unit;
            char           _f[0];
        } *si = (void *)(uintptr_t)arg1;

        if (!si)
            return -14; /* EFAULT */

        extern volatile uint64_t lapic_tick_count;
        si->uptime    = (long)(lapic_tick_count / 100);
        si->loads[0]  = si->loads[1] = si->loads[2] = 0;
        si->totalram  = pmm_total_count() * PAGE_SIZE;
        si->freeram   = pmm_free_count()  * PAGE_SIZE;
        si->sharedram = 0;
        si->bufferram = 0;
        si->totalswap = 0;
        si->freeswap  = 0;
        si->procs     = 0;
        si->totalhigh = 0;
        si->freehigh  = 0;
        si->mem_unit  = 1;
        return 0;
    }

    case 28: { /* SYS_madvise */
        uint64_t addr   = arg1;
        uint64_t len    = arg2;
        int      advice = (int)arg3;

        if (advice != 4) /* MADV_DONTNEED only */
            return -22;  /* EINVAL */

        uint64_t page_addr = addr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end_addr  = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t pg = page_addr; pg < end_addr; pg += PAGE_SIZE) {
            uint64_t phys = vmm_virt_to_phys(pg);
            if (!phys) continue;
            vmm_unmap_page(pg);
            pmm_free_frame(phys);
        }

        /* Re-enable lazy for any mmap regions covering the advised range */
        struct thread *t = sched_current();
        mmap_region_t *mmapr = THREAD_MMAPR(t);
        for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
            if (!mmapr[i].virt_addr) continue;
            uint64_t rs = mmapr[i].virt_addr;
            uint64_t re = rs + mmapr[i].len;
            if (rs < end_addr && re > page_addr)
                mmapr[i].lazy = 1;
        }
        return 0;
    }

    default:
        return SYSCALL_DISPATCH_UNHANDLED;
    }
}
