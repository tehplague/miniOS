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

#include <miniOS/fs/elf.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/io.h>
#include <miniOS/types.h>
#include <string.h>

/* Maximum number of program headers we handle */
#define ELF_MAX_PHDRS  32

static char *const default_envp[] = {
    "TERM=xterm-256color",
    NULL,
};

/* Drain (skip) `n` bytes from the sequential vfs fd into a scratch buffer */
static int drain_bytes(int fd, uint64_t n) {
    uint8_t scratch[64];
    while (n > 0) {
        uint32_t chunk = (n > sizeof(scratch)) ? (uint32_t)sizeof(scratch) : (uint32_t)n;
        int got = vfs_read(fd, scratch, chunk);
        if (got <= 0) return -1;
        n -= (uint64_t)got;
    }
    return 0;
}

int elf_load(int fd, uint64_t *entry_out, uint64_t *image_end_out,
             uint64_t *phdr_va_out, uint16_t *phnum_out) {
    Elf64_Ehdr ehdr;
    uint64_t image_end = 0;

    /* Environment passing is still minimal in miniOS userspace, but the
     * kernel now reserves the default terminal type for new exec images. */
    (void)default_envp;

    /* Step 1: Read and validate ELF header */
    int n = vfs_read(fd, &ehdr, sizeof(Elf64_Ehdr));
    if (n != (int)sizeof(Elf64_Ehdr)) {
        printk("ELF: failed to read ELF header (got %d bytes)\n", n);
        return -1;
    }

    /* Validate magic: "\177ELF" */
    if (ehdr.e_ident[0] != 0x7f ||
        ehdr.e_ident[1] != 'E'  ||
        ehdr.e_ident[2] != 'L'  ||
        ehdr.e_ident[3] != 'F') {
        printk("ELF: bad magic bytes\n");
        return -1;
    }

    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        printk("ELF: not ET_EXEC or ET_DYN (e_type=%u)\n", (uint32_t)ehdr.e_type);
        return -1;
    }
    /* PIE (ET_DYN) binaries are loaded at ELF_PIE_BASE; all p_vaddr are offsets. */
    uint64_t base = (ehdr.e_type == ET_DYN) ? ELF_PIE_BASE : 0;

    if (ehdr.e_machine != EM_X86_64) {
        printk("ELF: not x86_64 (e_machine=%u)\n", (uint32_t)ehdr.e_machine);
        return -1;
    }

    /* Step 2: Validate program header table parameters */
    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
        printk("ELF: unexpected e_phentsize %u (expected %u)\n",
               (uint32_t)ehdr.e_phentsize, (uint32_t)sizeof(Elf64_Phdr));
        return -1;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phnum > ELF_MAX_PHDRS) {
        printk("ELF: invalid e_phnum %u\n", (uint32_t)ehdr.e_phnum);
        return -1;
    }

    /* Step 3: Seek to program header table.
     * We've consumed sizeof(Elf64_Ehdr) bytes; drain any gap to e_phoff. */
    if (ehdr.e_phoff < sizeof(Elf64_Ehdr)) {
        printk("ELF: e_phoff %llu before end of ELF header\n", ehdr.e_phoff);
        return -1;
    }

    uint64_t gap = ehdr.e_phoff - sizeof(Elf64_Ehdr);
    if (gap > 0) {
        if (drain_bytes(fd, gap) < 0) {
            printk("ELF: failed to drain gap to program headers\n");
            return -1;
        }
    }

    /* Step 4: Read all program headers */
    Elf64_Phdr phdrs[ELF_MAX_PHDRS];
    uint32_t phdrs_size = (uint32_t)ehdr.e_phnum * sizeof(Elf64_Phdr);
    n = vfs_read(fd, phdrs, phdrs_size);
    if (n != (int)phdrs_size) {
        printk("ELF: failed to read program headers (got %d, expected %u)\n",
               n, phdrs_size);
        return -1;
    }

    /* Track our current file position after reading phdrs */
    uint64_t file_pos = ehdr.e_phoff + phdrs_size;

    /* Step 5: Map each PT_LOAD segment */
    uint64_t first_load_vaddr = 0, first_load_offset = 0;
    int first_load_found = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        if (!first_load_found) {
            first_load_vaddr = ph->p_vaddr;
            first_load_offset = ph->p_offset;
            first_load_found = 1;
        }

        uint64_t load_vaddr = ph->p_vaddr + base;

        uint64_t seg_end = load_vaddr + ph->p_memsz;
        if (seg_end > image_end) {
            image_end = seg_end;
        }

        /* 5a: Compute page-aligned VA range */
        uint64_t vaddr_start = load_vaddr & ~(uint64_t)0xFFF;
        uint64_t vaddr_end   = (load_vaddr + ph->p_memsz + 0xFFF) & ~(uint64_t)0xFFF;

        /* 5c: Build page flags */
        uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
        if (ph->p_flags & PF_W) page_flags |= PAGE_WRITE;

        /* 5d: Allocate and map each page, then zero it.
         * When two adjacent PT_LOAD segments share a page (e.g. a read-only
         * header segment ending at offset 0x270 and a text segment starting
         * at the same page), skip re-allocation — the page is already mapped
         * and contains data from the earlier segment. */
        for (uint64_t page_va = vaddr_start; page_va < vaddr_end; page_va += 4096) {
            uint64_t existing = vmm_virt_to_phys(page_va);
            if (existing) {
                /* Page already mapped by an earlier segment.  If this segment
                 * requires write access (e.g. .data following .rodata on a
                 * shared page boundary), upgrade the PTE flags in place. */
                if (page_flags & PAGE_WRITE)
                    vmm_map_page(page_va, existing, page_flags);
                continue;
            }
            uint64_t phys = pmm_alloc_frame();
            if (!phys) {
                printk("ELF: OOM mapping segment at 0x%llx\n", page_va);
                return -1;
            }
            if (vmm_map_page(page_va, phys, page_flags) < 0) {
                printk("ELF: vmm_map_page failed at 0x%llx\n", page_va);
                return -1;
            }
            memset((void *)page_va, 0, 4096);
        }

        /* 5e: Copy file bytes into mapped pages.
         * Drain from current file_pos to p_offset, then read p_filesz bytes.
         * Special case: p_offset < file_pos means the segment starts inside the
         * already-consumed header area (common when p_offset=0 and the ELF header
         * is part of the first PT_LOAD segment).  Replay those bytes from our
         * stack-allocated ehdr/phdrs, then read the remainder. */
        if (ph->p_filesz > 0) {
            if (ph->p_offset < file_pos) {
                uint8_t  *dst      = (uint8_t *)(uintptr_t)load_vaddr;
                uint64_t  seg_off  = ph->p_offset;
                uint64_t  consumed = file_pos - seg_off;

                /* Replay bytes from ehdr region [0, sizeof(Elf64_Ehdr)) */
                uint64_t ehdr_end = sizeof(Elf64_Ehdr);
                uint64_t cs = (seg_off  < ehdr_end) ? seg_off  : ehdr_end;
                uint64_t ce = (file_pos < ehdr_end) ? file_pos : ehdr_end;
                for (uint64_t k = cs; k < ce; k++)
                    dst[k - seg_off] = ((const uint8_t *)&ehdr)[k];

                /* Replay bytes from phdrs region [e_phoff, e_phoff+phdrs_size) */
                uint64_t ph_start = ehdr.e_phoff;
                uint64_t ph_end   = ehdr.e_phoff + phdrs_size;
                uint64_t ps = (seg_off  < ph_start) ? ph_start : seg_off;
                uint64_t pe = (file_pos < ph_end)   ? file_pos : ph_end;
                for (uint64_t k = ps; k < pe; k++)
                    dst[k - seg_off] = ((const uint8_t *)phdrs)[k - ph_start];

                /* Read the remaining segment bytes from the current file position */
                uint64_t remaining = ph->p_filesz - consumed;
                if (remaining > 0) {
                    n = vfs_read(fd, dst + consumed, (uint32_t)remaining);
                    if (n != (int)remaining) {
                        printk("ELF: failed to read segment tail (got %d, expected %llu)\n",
                               n, remaining);
                        return -1;
                    }
                    file_pos += remaining;
                }
            } else {
                uint64_t skip = ph->p_offset - file_pos;
                if (skip > 0) {
                    if (drain_bytes(fd, skip) < 0) {
                        printk("ELF: failed to drain to segment offset\n");
                        return -1;
                    }
                    file_pos += skip;
                }

                n = vfs_read(fd, (void *)load_vaddr, (uint32_t)ph->p_filesz);
                if (n != (int)ph->p_filesz) {
                    printk("ELF: failed to read segment data (got %d, expected %llu)\n",
                           n, ph->p_filesz);
                    return -1;
                }
                file_pos += ph->p_filesz;
            }
        }

        /* 5f: Zero the BSS portion (memsz > filesz) explicitly.
         * New frames were already zeroed in 5d, but EXISTING frames (reused from a
         * previous binary on re-exec) were not.  Writing zeros here ensures BSS
         * variables like mlibc's tcb_available_flag start clean in the new process
         * regardless of what was in those pages before. */
        if (ph->p_memsz > ph->p_filesz) {
            uint64_t bss_start = load_vaddr + ph->p_filesz;
            uint64_t bss_end   = load_vaddr + ph->p_memsz;
            memset((void *)bss_start, 0, (uint32_t)(bss_end - bss_start));
        }
    }

    /* Step 6: For ET_DYN, apply R_X86_64_RELATIVE relocations via PT_DYNAMIC */
    if (ehdr.e_type == ET_DYN) {
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type != PT_DYNAMIC) continue;

            Elf64_Dyn *dyn = (Elf64_Dyn *)(base + phdrs[i].p_vaddr);
            uint64_t rela_addr = 0, rela_sz = 0, rela_ent = sizeof(Elf64_Rela);

            for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
                if (d->d_tag == DT_RELA)    rela_addr = base + d->d_un.d_ptr;
                if (d->d_tag == DT_RELASZ)  rela_sz   = d->d_un.d_val;
                if (d->d_tag == DT_RELAENT) rela_ent  = d->d_un.d_val;
            }

            if (rela_addr && rela_sz) {
                uint64_t count = rela_sz / rela_ent;
                Elf64_Rela *rela = (Elf64_Rela *)rela_addr;
                for (uint64_t j = 0; j < count; j++) {
                    if (ELF64_R_TYPE(rela[j].r_info) == R_X86_64_RELATIVE) {
                        uint64_t *target = (uint64_t *)(base + rela[j].r_offset);
                        *target = base + (uint64_t)rela[j].r_addend;
                    }
                }
            }
            break;
        }
    }

    /* Step 7: Return entry point and phdr metadata for auxv */
    *entry_out = ehdr.e_entry + base;
    if (image_end_out != NULL) {
        *image_end_out = (image_end + 0xFFFULL) & ~0xFFFULL;
    }
    if (phdr_va_out != NULL) {
        /* Compute the expected phdr VA via the first PT_LOAD mapping. */
        uint64_t phdr_va = first_load_found
            ? (base + first_load_vaddr + ehdr.e_phoff - first_load_offset)
            : 0;

        /* Verify phdr_va falls within a loaded segment.  For PIE binaries
         * where the linker doesn't include the ELF header in the first LOAD
         * (e.g. first LOAD starts at file offset 0x1000, not 0x0), the
         * computed VA would be below the mapped range and unmapped.  In that
         * case, allocate a synthetic page and copy the phdr table there. */
        int va_valid = 0;
        if (phdr_va) {
            for (uint16_t i = 0; i < ehdr.e_phnum && !va_valid; i++) {
                if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) continue;
                uint64_t seg_va  = phdrs[i].p_vaddr + base;
                uint64_t seg_end = seg_va + phdrs[i].p_memsz;
                if (phdr_va >= seg_va && phdr_va < seg_end)
                    va_valid = 1;
            }
        }

        if (!va_valid) {
            /* Place phdr copy one page below the binary base. */
            uint64_t phdr_page = base - 0x1000;
            uint64_t phys = pmm_alloc_frame();
            if (phys && vmm_map_page(phdr_page, phys, PAGE_PRESENT | PAGE_USER) >= 0) {
                memset((void *)phdr_page, 0, 4096);
                memcpy((void *)phdr_page, phdrs, (uint32_t)ehdr.e_phnum * sizeof(Elf64_Phdr));
                phdr_va = phdr_page;
                printk("ELF: phdrs remapped to 0x%llx\n", phdr_va);
            } else {
                phdr_va = 0;
            }
        }
        *phdr_va_out = phdr_va;
    }
    if (phnum_out != NULL) {
        *phnum_out = ehdr.e_phnum;
    }
    return 0;
}
