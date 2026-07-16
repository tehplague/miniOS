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

#include <miniOS/arch/x86_64/smp.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/arch/x86_64/tss.h>
#include <miniOS/mm/heap.h>
#include <miniOS/io.h>
#include <string.h>

/* BSP's GDT and TSS (from gdt.c) — APs copy and patch their own */
extern struct segment_descriptor GDT[];
extern struct tss_struct tss;  /* BSP TSS — used as template size only */

/* Per-CPU GDT and TSS storage.
   Index 0 = BSP (though BSP uses the original GDT[]/tss globals,
   we store per-CPU copies for uniformity in AP paths). */
static struct segment_descriptor percpu_gdt[MAX_CPUS][7];
static struct tss_struct           percpu_tss[MAX_CPUS];

/* wrmsr_gs_base declared in per_cpu.asm */
extern void wrmsr_gs_base(uint64_t addr);

/* lgdt_ap declared in per_cpu.asm — loads GDT and reloads CS via far-return */
extern void lgdt_ap(void *gdt_desc);

/* Global cpu_t array — defined here, declared extern in smp.h */
cpu_t g_cpus[MAX_CPUS];

/* per_cpu_update_rsp0() — atomically update RSP0 for both the CPU's TSS and
   cpu_t.kstack_top.  Called by sched_schedule() before every context switch
   to a user thread, and by kernel_main before the first ring-3 entry.
   Always updates the CALLING CPU's percpu_tss entry (not the BSP's global tss). */
void per_cpu_update_rsp0(uint64_t rsp0) {
    cpu_t *cpu = cpu_local();
    percpu_tss[cpu->cpu_id].privileged_stack_table[0] = rsp0;
    cpu->kstack_top = rsp0;
}

void per_cpu_init(uint32_t cpu_id) {
    cpu_t *cpu = &g_cpus[cpu_id];

    /* 1. Fill cpu_t fields */
    cpu->self        = cpu;   /* gs:0 self-pointer */
    cpu->cpu_id      = cpu_id;
    cpu->lapic_id    = (uint32_t)(lapic_read(LAPIC_ID) >> 24);
    /* cpu->idle_thread is set by sched_init() / sched_init_cpu() — do not zero here */

    /* 2. Allocate a 16 KiB kernel stack for this CPU.
       BSP uses its boot stack (already valid); APs need an early stack
       to run C code after the trampoline before the scheduler starts.
       We allocate for all CPUs for uniformity. */
    uint8_t *kstack = (uint8_t *)kmalloc(16384);
    if (!kstack) {
        printk("per_cpu_init: OOM for CPU %u stack\n", cpu_id);
        for (;;) __asm__ volatile("cli; hlt");
    }
    cpu->kstack_base = kstack;

    /* 3. Build a per-CPU GDT:
       Copy BSP GDT[0..6] to percpu_gdt[cpu_id][].
       Patch TSS descriptor (entries 5 + 6) with the per-CPU TSS address. */
    for (int i = 0; i < 7; i++)
        percpu_gdt[cpu_id][i] = GDT[i];

    /* Zero and init the per-CPU TSS */
    memset(&percpu_tss[cpu_id], 0, sizeof(struct tss_struct));
    /* RSP0 = top of this CPU's kernel stack.
       Will be updated on each context switch by tss_set_rsp0(). */
    percpu_tss[cpu_id].privileged_stack_table[0] =
        (uint64_t)(kstack + 16384);

    /* Patch TSS descriptor in per-CPU GDT.
       Same technique as gdt.c: 16-byte system descriptor split across
       GDT[5] (lower) and GDT[6] (upper 32 bits of base). */
    uint64_t tss_base = (uint64_t)&percpu_tss[cpu_id];
    percpu_gdt[cpu_id][GDT_TSS_INDEX] =
        BUILD_SEG_DESC((uint32_t)tss_base, sizeof(struct tss_struct) - 1,
                       GDT_TSS_TYPE, 0, 0);
    *(uint64_t *)&percpu_gdt[cpu_id][GDT_LDT_INDEX] =
        (tss_base >> 32) & 0xFFFFFFFFULL;

    /* 4. Build LGDT descriptor and load it */
    struct {
        uint16_t size;
        uint64_t addr;
    } __attribute__((packed)) gdtd;
    gdtd.size = (uint16_t)(sizeof(percpu_gdt[cpu_id]) - 1);
    gdtd.addr = (uint64_t)percpu_gdt[cpu_id];

    if (cpu_id == 0) {
        /* BSP: lgdt inline — already running with interrupts possibly on */
        __asm__ volatile("lgdt %0" :: "m"(gdtd) : "memory");
        /* Reload data segments after lgdt (CS was reloaded by gdt_init) */
        __asm__ volatile(
            "mov $0x10, %%ax\n\t"
            "mov %%ax, %%ds\n\t"
            "mov %%ax, %%es\n\t"
            "mov %%ax, %%ss\n\t"
            ::: "rax"
        );
    } else {
        /* AP: use lgdt_ap which also reloads CS via far-return trick */
        lgdt_ap(&gdtd);
        /* Reload data segments after lgdt_ap */
        __asm__ volatile(
            "mov $0x10, %%ax\n\t"
            "mov %%ax, %%ds\n\t"
            "mov %%ax, %%es\n\t"
            "mov %%ax, %%ss\n\t"
            ::: "rax"
        );
    }

    /* 5. Load TSS into TR (must be done after lgdt with new GDT active) */
    uint16_t tr = (uint16_t)((GDT_TSS_INDEX << 3) | 0);  /* selector, RPL=0 */
    __asm__ volatile("ltr %0" :: "r"(tr));

    /* 6. Load IDT: APs share the BSP's IDT.
       Use idt_reload() to load from the global IDT[] array.
       Cannot use 'sidt' here: each AP's IDTR is in CPU reset state after the
       trampoline (base=0, limit=0xffff).  sidt would read that null descriptor
       and lidt would reload it, leaving the AP with a null IDT.  When the AP's
       LAPIC timer fires, IDT[48] is at virtual address 0 which is an invalid
       or absent mapping, causing #GP → double fault → triple fault. */
    idt_reload();

    /* 7. Write GS base MSR to point at this cpu_t */
    wrmsr_gs_base((uint64_t)cpu);

    printk("per_cpu_init: CPU %u LAPIC %u GDT/TSS/GS initialized\n",
           cpu_id, cpu->lapic_id);
}
