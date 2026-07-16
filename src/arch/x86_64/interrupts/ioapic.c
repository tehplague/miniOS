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

#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/io.h>

/* irq_wrapper_array is defined in isr.asm — array of 16 function pointers */
extern addr_t irq_wrapper_array[NR_IRQS];

/* C-level IRQ dispatch table (registered by drivers) */
static idt_handler_func *irq_handlers[NR_IRQS] = {0};

void irq_set_handler(uint8_t irq, idt_handler_func *fn) {
    irq_handlers[irq] = fn;
}

/* Called from irq_wrapper stubs in isr.asm after saving context */
void irq_dispatch(uint8_t irq, void *frame) {
    if (irq_handlers[irq])
        irq_handlers[irq](frame);
    /* EOI is sent by the wrapper in isr.asm via lapic_eoi_asm() */
}

static volatile uint32_t *ioapic = (volatile uint32_t *)IOAPIC_BASE;

static uint32_t ioapic_read(uint8_t reg) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_IOWIN / 4];
}

static void ioapic_write(uint8_t reg, uint32_t val) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_IOWIN / 4] = val;
}

void ioapic_mask_irq(uint8_t irq) {
    uint32_t lo = ioapic_read(IOAPIC_REDTBL_LO(irq));
    ioapic_write(IOAPIC_REDTBL_LO(irq), lo | IOAPIC_RTE_MASKED);
}

void ioapic_unmask_irq(uint8_t irq) {
    uint32_t lo = ioapic_read(IOAPIC_REDTBL_LO(irq));
    ioapic_write(IOAPIC_REDTBL_LO(irq), lo & ~IOAPIC_RTE_MASKED);
}

void ioapic_init(void) {
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint8_t max_rte = (ver >> 16) & 0xFF;
    printk("IOAPIC: version=0x%x max_rte=%u\n", ver & 0xFF, max_rte);

    /* Route ISA IRQs 0-15 to IDT vectors IRQ_VECTOR_BASE+0 .. IRQ_VECTOR_BASE+15.
       Delivery mode: fixed (000), destination mode: physical, destination: LAPIC 0.
       All start masked; individual drivers unmask their IRQ when ready.
       ISA IRQs are edge-triggered, active-high by default. */
    for (uint8_t i = 0; i < NR_IRQS; i++) {
        /* High word: destination LAPIC ID = 0 (APIC ID 0 in bits 56-59 of 64-bit RTE,
           which is bits 24-27 of the high 32-bit word) */
        ioapic_write(IOAPIC_REDTBL_HI(i), 0 << 24);
        /* Low word: vector = IRQ_VECTOR_BASE + i, edge-triggered, active-high, masked */
        ioapic_write(IOAPIC_REDTBL_LO(i),
                     IOAPIC_RTE_MASKED | (IRQ_VECTOR_BASE + i));
    }

    /* Install IDT entries for vectors 32-47 pointing to irq_wrapper stubs */
    for (uint8_t i = 0; i < NR_IRQS; i++) {
        idt_set_handler(IRQ_VECTOR_BASE + i, irq_wrapper_array[i],
                        KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);
    }

    printk("IOAPIC: IRQs 0-15 routed to vectors %u-%u (all masked)\n",
           IRQ_VECTOR_BASE, IRQ_VECTOR_BASE + NR_IRQS - 1);
}
