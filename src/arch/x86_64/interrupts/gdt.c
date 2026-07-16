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

#include <miniOS/arch/x86_64/gdt.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/arch/x86_64/tss.h>
#include <string.h>

extern void *interrupt_stack_top;

struct tss_struct tss;

struct segment_descriptor GDT[] = {
    // NULL descriptor
    { 0, },

    // Kernel code segment
    BUILD_4KB_SEG_DESC(0, 0x000FFFFF, GDT_CS_TYPE, 1, 0),

    // Kernel data segment
    BUILD_4KB_SEG_DESC(0, 0x000FFFFF, GDT_DS_TYPE, 0, 1),

    // User Mode data segment (index 3 — before UserCS for SYSRET STAR formula)
    BUILD_4KB_SEG_DESC(0, 0x000FFFFF, USER_DS_TYPE, 0, 1),

    // User Mode code segment (index 4)
    BUILD_4KB_SEG_DESC(0, 0x000FFFFF, USER_CS_TYPE, 1, 0),

    // Task state segment descriptor
    { 0, },

    // Local descriptor table segment descriptor
    { 0, }
};


void gdt64_load(void *gdt_desc, uint16_t cs, uint16_t ds);

void gdt_init()
{
    uint16_t tr;
    memset(&tss, 0, sizeof(struct tss_struct));

    struct {
        uint16_t size;
        uint64_t addr;
    } __attribute__((__packed__)) gdt_desc;

    gdt_desc.size = sizeof(GDT) - 1;
    gdt_desc.addr = (uint64_t)GDT;

    // Load the GDT
    gdt64_load(&gdt_desc, GDT_CS, GDT_DS);

    for (int i = 0; i < 7; i++) {
        tss.interrupt_stack_table[i] = (uint64_t)(&interrupt_stack_top);
    }

    /* In x86_64, system descriptors (TSS, LDT) are 16 bytes — two consecutive
     * GDT slots.  The lower 8 bytes hold base[31:0] in the standard format;
     * the upper 8 bytes hold base[63:32] in bits [31:0] (bits [63:32] must
     * be zero).
     *
     * The (uint32_t) cast used here deliberately truncates: we pass only
     * bits [31:0] of the TSS virtual address to BUILD_SEG_DESC (which only
     * has fields for a 32-bit base).  The upper 32 bits are stored separately
     * in the next GDT slot (GDT_LDT_INDEX) so the CPU sees the full 64-bit
     * base address and can read RSP0 from the TSS even after the identity map
     * (PML4[0]) is torn down in kernel_main.
     *
     * Without this fix the CPU used 0x00012f560 (truncated) as the TSS base.
     * After pml4[0]=0 that VA is unmapped → TSS read faults → double fault →
     * triple fault on every exception from ring 3 (e.g. SYSCALL, timer IRQ). */
    uint64_t tss_base = (uint64_t)&tss;
    GDT[GDT_TSS_INDEX] = BUILD_SEG_DESC((uint32_t)tss_base, sizeof(tss), GDT_TSS_TYPE, 0, 0);
    /* Upper 64-bit extension slot: base[63:32] in the low 32 bits, reserved high 32 bits = 0 */
    *(uint64_t *)&GDT[GDT_LDT_INDEX] = (tss_base >> 32) & 0xFFFFFFFFULL;
    tr = SEG_REG_VAL(KERN_PRIVILEGE_LEVEL, false, GDT_TSS_INDEX);
    __asm__ volatile("ltr %0" :: "r"(tr));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.privileged_stack_table[0] = rsp0;
}
