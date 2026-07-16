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

#ifndef _MINIOS_ARCH_X86_64_IRQ_H_
#define _MINIOS_ARCH_X86_64_IRQ_H_

#ifndef ASM_SOURCE

#include <miniOS/types.h>

struct idt_entry_t {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t irq_stack_index:3;
    uint8_t zero1:5;
    uint8_t type:4;
    uint8_t zero2:1;
    uint8_t dpl:2;
    uint8_t present:1;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero3;
} __attribute__((packed));

struct idt_descriptor_t {
    uint16_t size;
    struct idt_entry_t *offset;
} __attribute__((packed));

typedef void idt_handler_func(void *interrupt_stack_frame);

void idt_init();
void idt_reload(void);  /* reload IDTR from global IDT[] — call on each AP after boot */
void idt_set_handler(uint8_t type, addr_t handler_addr, unsigned int dpl, uint8_t gate_type);
void irq_set_handler(uint8_t irq, idt_handler_func *fn);

#endif

// ********************************************************
//   Interrupt controller flags
// ********************************************************

#define PIC1_CMD  0x20 // IO base address for master PIC
#define PIC2_CMD  0xA0 // IO base address for secondary PIC
#define PIC1_DATA 0x21 // Master PIC data port
#define PIC2_DATA 0xA1 // Secondary PIC data port
#define PIC_EOI   0x20 // End-of-interrupt command code

/* Number of standard IRQ levels on a PC. */
#define NR_IRQS 16

/* List of standard IRQ levels on a PC. Keep in mind that pin #2 is connected
   to the slave PIC, and that pin #9 is connected to the master PIC. */
#define IRQ_TIMER            0
#define IRQ_KEYBOARD         1
#define SLAVE_PIC            2
#define IRQ_COM2             3
#define IRQ_COM1             4
#define IRQ_LPT2             5
#define IRQ_FLOPPY           6
#define IRQ_LPT1             7
#define IRQ_RT_CLOCK         8
#define MASTER_PIC           9
#define IRQ_AVAILABLE_1     10
#define IRQ_AVAILABLE_2     11
#define IRQ_PS2_MOUSE       12
#define IRQ_COPROCESSOR     13
#define IRQ_PRIMARY_IDE     14
#define IRQ_SECONDARY_IDE   15

// ********************************************************
//   CPU exceptions
// ********************************************************

/* Number of standard exceptions on a PC. */
#define NR_EXCEPTIONS 32

/* List of standard Intel x86 exceptions.
   See Intel x86 doc vol 3, section 5.12. */
#define EXCEPT_DIVIDE_ERROR                  0         // No error code
#define EXCEPT_DEBUG                         1         // No error code
#define EXCEPT_NMI_INTERRUPT                 2         // No error code
#define EXCEPT_BREAKPOINT                    3         // No error code
#define EXCEPT_OVERFLOW                      4         // No error code
#define EXCEPT_BOUND_RANGE_EXCEDEED          5         // No error code
#define EXCEPT_INVALID_OPCODE                6         // No error code
#define EXCEPT_DEVICE_NOT_AVAILABLE          7         // No error code
#define EXCEPT_DOUBLE_FAULT                  8         // Yes (Zero)
#define EXCEPT_COPROCESSOR_SEGMENT_OVERRUN   9         // No error code
#define EXCEPT_INVALID_TSS                  10         // Yes
#define EXCEPT_SEGMENT_NOT_PRESENT          11         // Yes
#define EXCEPT_STACK_SEGMENT_FAULT          12         // Yes
#define EXCEPT_GENERAL_PROTECTION           13         // Yes
#define EXCEPT_PAGE_FAULT                   14         // Yes
#define EXCEPT_INTEL_RESERVED_1             15         // No
#define EXCEPT_FLOATING_POINT_ERROR         16         // No
#define EXCEPT_ALIGNMENT_CHECK              17         // Yes (Zero)
#define EXCEPT_MACHINE_CHECK                18         // No
#define EXCEPT_INTEL_RESERVED_2             19         // No
#define EXCEPT_INTEL_RESERVED_3             20         // No
#define EXCEPT_INTEL_RESERVED_4             21         // No
#define EXCEPT_INTEL_RESERVED_5             22         // No
#define EXCEPT_INTEL_RESERVED_6             23         // No
#define EXCEPT_INTEL_RESERVED_7             24         // No
#define EXCEPT_INTEL_RESERVED_8             25         // No
#define EXCEPT_INTEL_RESERVED_9             26         // No
#define EXCEPT_INTEL_RESERVED_10            27         // No
#define EXCEPT_INTEL_RESERVED_11            28         // No
#define EXCEPT_INTEL_RESERVED_12            29         // No
#define EXCEPT_INTEL_RESERVED_13            30         // No
#define EXCEPT_INTEL_RESERVED_14            31         // No

#define IDT_CALL_GATE       (0b1100)
#define IDT_INTERRUPT_GATE  (0b1110)
#define IDT_TRAP_GATE       (0b1111)

#endif
