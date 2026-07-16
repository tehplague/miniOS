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

#ifndef _MINIOS_FS_ELF_H_
#define _MINIOS_FS_ELF_H_

#include <miniOS/types.h>

/* ELF magic */
#define ELFMAG "\177ELF"

/* e_type values */
#define ET_EXEC  2
#define ET_DYN   3   /* position-independent executable (PIE) */

/* e_machine values */
#define EM_X86_64  62

/* p_type values */
#define PT_LOAD    1
#define PT_DYNAMIC 2

/* Dynamic section tag values */
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9

/* Relocation type */
#define R_X86_64_RELATIVE 8
#define ELF64_R_TYPE(i) ((uint32_t)(i))

/* Load base for PIE binaries — kernel maps ET_DYN at this VA */
#define ELF_PIE_BASE  0x400000ULL

/* p_flags bit masks */
#define PF_X  1   /* Execute */
#define PF_W  2   /* Write */
#define PF_R  4   /* Read */

/* ELF64 executable header — only fields needed for ET_EXEC loading */
typedef struct {
    uint8_t   e_ident[16];   /* Magic number and other info */
    uint16_t  e_type;        /* Object file type (ET_EXEC = 2) */
    uint16_t  e_machine;     /* Architecture (EM_X86_64 = 62) */
    uint32_t  e_version;     /* Object file version */
    uint64_t  e_entry;       /* Entry point virtual address */
    uint64_t  e_phoff;       /* Program header table file offset */
    uint64_t  e_shoff;       /* Section header table file offset */
    uint32_t  e_flags;       /* Processor-specific flags */
    uint16_t  e_ehsize;      /* ELF header size in bytes */
    uint16_t  e_phentsize;   /* Program header table entry size */
    uint16_t  e_phnum;       /* Program header table entry count */
    uint16_t  e_shentsize;   /* Section header table entry size */
    uint16_t  e_shnum;       /* Section header table entry count */
    uint16_t  e_shstrndx;    /* Section header string table index */
} __attribute__((packed)) Elf64_Ehdr;

/* ELF64 program header — only fields needed for PT_LOAD mapping */
typedef struct {
    uint32_t  p_type;    /* Segment type (PT_LOAD = 1) */
    uint32_t  p_flags;   /* Segment flags (PF_X / PF_W / PF_R) */
    uint64_t  p_offset;  /* Segment file offset */
    uint64_t  p_vaddr;   /* Segment virtual address */
    uint64_t  p_paddr;   /* Segment physical address */
    uint64_t  p_filesz;  /* Segment size in file */
    uint64_t  p_memsz;   /* Segment size in memory */
    uint64_t  p_align;   /* Segment alignment */
} __attribute__((packed)) Elf64_Phdr;

/* ELF64 dynamic section entry */
typedef struct {
    int64_t   d_tag;
    union { uint64_t d_val; uint64_t d_ptr; } d_un;
} Elf64_Dyn;

/* ELF64 relocation with addend */
typedef struct {
    uint64_t  r_offset;
    uint64_t  r_info;
    int64_t   r_addend;
} Elf64_Rela;

/* Load a static ELF binary from an open VFS fd.
 * Maps all PT_LOAD segments into the current address space (PAGE_USER).
 * On success: *entry_out = e_entry. If image_end_out is non-NULL, receives the
 * first page-aligned address after the loaded PT_LOAD image for heap/brk init.
 * phdr_va_out (if non-NULL) receives the virtual address of the phdr table
 * (needed for AT_PHDR in the exec auxv so glibc can find PT_TLS).
 * phnum_out (if non-NULL) receives e_phnum.
 * On failure: returns -1 (bad magic, wrong type/machine, or OOM). */
int elf_load(int fd, uint64_t *entry_out, uint64_t *image_end_out,
             uint64_t *phdr_va_out, uint16_t *phnum_out);

#endif /* _MINIOS_FS_ELF_H_ */
