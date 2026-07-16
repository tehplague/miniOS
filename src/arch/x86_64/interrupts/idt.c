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

#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/io.h>
#include <string.h>

/* IDT must cover all 256 hardware vectors (0-255).
   NR_EXCEPTIONS (32) only covers CPU exceptions; LAPIC uses vectors up to 0xFF. */
#define IDT_NUM_VECTORS 256
struct idt_entry_t IDT[IDT_NUM_VECTORS] __attribute__((aligned(4)));
idt_handler_func *irq_handlers[NR_IRQS] = {0};
idt_handler_func *exception_handlers[NR_EXCEPTIONS] = {0};

void idt_reload(void);

void idt_init() {
    memset(IDT, 0, sizeof(IDT));

    idt_reload();
}

/* idt_reload() — load the IDT from the global IDT[] array.
   Called by idt_init() on BSP and by per_cpu_init() on each AP.
   APs cannot use sidt to read the IDT (their IDTR is in CPU reset state
   after the trampoline; sidt would return base=0), so they must call this
   function directly to load the correct IDTR pointing at the shared IDT[]. */
void idt_reload(void) {
    struct idt_descriptor_t idt_descriptor;
    idt_descriptor.size = sizeof(IDT) - 1;
    idt_descriptor.offset = IDT;

    __asm__ volatile("lidt %0" : : "m"(idt_descriptor));
}

void idt_set_handler(uint8_t type, addr_t handler_addr, unsigned int dpl, uint8_t gate_type) {
    struct idt_entry_t *idt_entry = &IDT[type];

    memset(idt_entry, 0, sizeof(struct idt_entry_t));
    idt_entry->offset_low = handler_addr & 0xFFFF;
    idt_entry->offset_middle = (handler_addr >> 16) & 0xFFFF;
    idt_entry->offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
    idt_entry->type = gate_type;
    idt_entry->dpl = dpl;
    idt_entry->present = 1;
    idt_entry->selector = GDT_CS;
}
