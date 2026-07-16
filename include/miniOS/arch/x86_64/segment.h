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

#ifndef _MINIOS_ARCH_X86_64_SEGMENT_H_
#define _MINIOS_ARCH_X86_64_SEGMENT_H_

#ifndef ASM_SOURCE

#include <miniOS/types.h>

// ********************************************************
//   CPU privilege levels
// ********************************************************

#define KERN_PRIVILEGE_LEVEL 0
#define USER_PRIVILEGE_LEVEL 3

// ********************************************************
//   x86 segment types
// ********************************************************

#define GDT_CS_TYPE  0x9A // present, system, Ring 0, r-x
#define GDT_DS_TYPE  0x92 // present, system, Ring 0, rw-
#define GDT_TSS_TYPE 0x89 // present, system, Ring 0, 32-bit TSS
#define GDT_LDT_TYPE 0x82 // present, system, Ring 0, LDT

#define USER_CS_TYPE  0xFA // present, non-system, Ring 3, r-x
#define USER_DS_TYPE  0xF2 // present, non-system, Ring 3, rw-

// ********************************************************
//   Global Descriptor Tables
// ********************************************************

// The NULL descriptor doesn't get a number ;-)
#define GDT_CS_INDEX  1 // Kernel code segment
#define GDT_DS_INDEX  2 // Kernel data segment
#define USER_DS_INDEX  3 // User mode data segment  (before UserCS for SYSRET STAR formula)
#define USER_CS_INDEX  4 // User mode code segment
#define GDT_TSS_INDEX 5 // Task state selector segment
#define GDT_LDT_INDEX 6 // Local Descriptor Table

// ********************************************************
//   Precomputed segment register values (selector values)
// ********************************************************

/* Kernel selectors: RPL=0, TI=0 (GDT) */
#define KERNEL_CS_SEL  SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_CS_INDEX)  /* 0x08 */
#define KERNEL_SS_SEL  SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_DS_INDEX)  /* 0x10 */

/* User selectors: RPL=3, TI=0 (GDT).                                       *
 * USER_DS_INDEX=3 (0x1B) placed before USER_CS_INDEX=4 (0x23) in GDT for  *
 * SYSRETQ STAR formula: CS=STAR[63:48]+16, SS=STAR[63:48]+8.              */
#define USER_CS_SEL    SEG_REG_VAL(USER_PRIVILEGE_LEVEL, 0, USER_CS_INDEX)  /* 0x23 */
#define USER_DS_SEL    SEG_REG_VAL(USER_PRIVILEGE_LEVEL, 0, USER_DS_INDEX)  /* 0x1B */

// ********************************************************
//   RFLAGS constants
// ********************************************************

/* RFLAGS_IF: RFLAGS value with only IF (bit 1) and reserved bit 1 set.     *
 * Used as initial rflags for all kernel and user threads so that            *
 * hlt/sti sequences work correctly and user code runs with interrupts on.  */
#define RFLAGS_IF  0x202ULL

#define GDT_CS  SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_CS_INDEX)
#define GDT_DS  SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_DS_INDEX)
#define GDT_TSS SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_TSS_INDEX)
#define GDT_LDT SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, 0, GDT_LDT_INDEX)

// ********************************************************
//   Local Descriptor Table
// ********************************************************

#define NR_LDT_ENTRIES 2

#define LDT_CS_INDEX 0 // Process code segment
#define LDT_DS_INDEX 1 // Process data segment

/* Structure is also used in IA-32e mode, however in that case all addresses can be set to 0 */
struct segment_descriptor {
    uint16_t limit_15_0;
    uint16_t base_addr_15_0;
    uint8_t base_addr_23_16;

    uint8_t access;
    uint8_t limit_19_16:4;
    uint8_t u:1;
    uint8_t long_mode:1;
    uint8_t size:1;
    uint8_t granularity:1; // length byte (0=1B..1MB, 1=4K..4GB)
    uint8_t base_addr_31_24;
} __attribute__((__packed__));

#define __BUILD_SEG_DESC(base_addr, limit, _access, _long_mode, _size, _granularity)      \
    ((struct segment_descriptor) {                     \
        .limit_15_0 = (limit) & 0xffff,                \
        .limit_19_16 = ((limit) >> 16) & 0xf,          \
        .base_addr_15_0 = (base_addr) & 0xffff,        \
        .base_addr_23_16 = ((base_addr) >> 16) & 0xff, \
        .base_addr_31_24 = ((base_addr) >> 24) & 0xff, \
        .u = 0, \
        .long_mode = (_long_mode), \
        .size = (_size), \
        .granularity = (_granularity),                \
        .access = (_access)                              \
    })

/* Build a byte granular segment descriptor. */
#define BUILD_SEG_DESC(base_addr, limit, access, long_mode, size) \
    __BUILD_SEG_DESC(base_addr, limit, access, long_mode, size, 0)

/* Build a 4KB granular segment descriptor. */
#define BUILD_4KB_SEG_DESC(base_addr, limit, access, long_mode, size) \
    __BUILD_SEG_DESC(base_addr, (limit >> 12), access, long_mode, size, 1)

#define SEG_ADDR(seg_desc)               \
  (((seg_desc)->base_addr_31_24 << 24) | \
  ((seg_desc)->base_addr_23_16 << 16) |  \
   (seg_desc)->base_15_0)

#define SEG_SIZE(seg_desc)                                             \
  ((seg_desc)->granularity ?                                                     \
  ((((seg_desc)->limit_19_16 << 16) | (seg_desc)->limit_15_0) << 12) : \
   (((seg_desc)->limit_19_16 << 16) | (seg_desc)->limit_15_0))

#endif

// Construct segment register value
#define SEG_REG_VAL(privilege, in_ldt, segment_index)           \
  (((segment_index) << 3) | ((in_ldt) << 2) | (privilege))

// Return Request Privilege level of the specified segment selector
#define SEG_REG_RPL(seg_reg_val) ((seg_reg_val) & 0x3)

#define restore_system_segments \
  pushq %rax;                    \
  mov $GDT_DS, %ax;             \
  mov %ax, %ds;                 \
  mov %ax, %es;                 \
  mov %ax, %fs;                 \
  mov %ax, %gs;                 \
  popq %rax;

#endif
