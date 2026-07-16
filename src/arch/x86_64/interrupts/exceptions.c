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

#include <miniOS/arch/x86_64/context.h>
#include <miniOS/arch/x86_64/exceptions.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/io.h>
#include <miniOS/ipi/ipi.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/sched/sched.h>

static int is_mapped_range(uint64_t base, uint64_t len) {
    for (uint64_t off = 0; off < len; off++) {
        if (vmm_virt_to_phys(base + off) == 0) {
            return 0;
        }
    }
    return 1;
}

extern addr_t exception_wrapper_array[NR_EXCEPTIONS];
exception_handler_t exception_handler_array[NR_EXCEPTIONS] = { NULL};

void exception_set_handler(unsigned int num, exception_handler_t fn) {
    addr_t isr;
    exception_handler_array[num] = fn;
    isr = (fn == NULL) ? (addr_t)NULL : (addr_t)exception_wrapper_array[num];

    idt_set_handler(num, isr, KERN_PRIVILEGE_LEVEL, IDT_INTERRUPT_GATE);
}

static void handle_exception(const char *s, struct task_cpu_context *context) {
    printk("+------------------------------------------------------------\n");
    printk("| EXCEPTION: ");
    printk("%s", s);
    printk("\n");
    printk("| RAX=0x%016llx RBX=0x%016llx\n", context->rax, context->rbx);
    printk("| RCX=0x%016llx RDX=0x%016llx\n", context->rcx, context->rdx);
    printk("| RBP=0x%016llx\n", context->rbp);
    printk("| R8 =0x%016llx R9 =0x%016llx\n", context->r8, context->r9);
    printk("| R10=0x%016llx R11=0x%016llx\n", context->r10, context->r11);
    printk("| R12=0x%016llx R13=0x%016llx\n", context->r12, context->r13);
    printk("| R14=0x%016llx R15=0x%016llx\n", context->r14, context->r15);
    printk("| RSP=0x%04llx:%016llx\n", context->ss, context->rsp);
    printk("| RSI=0x%016llx RDI=0x%016llx\n", context->rsi, context->rdi);
    printk("| RIP=0x%04llx:%016llx\n", context->cs, context->rip);
    printk("| CS=0x%04llx SS=0x%04llx\n", context->cs, context->ss);
    printk("| RSP=0x%016llx\n", context->rsp);
    printk("| RFLAGS=0x%016llx (TF=%llu IF=%llu)\n",
           context->rflags,
           (context->rflags >> 8) & 1,
           (context->rflags >> 9) & 1);

    if (context->cs == USER_CS_SEL) {
        uint64_t rip = context->rip;
        uint64_t rip_dump = (rip >= 8) ? (rip - 8) : rip;
        if (is_mapped_range(rip_dump, 16)) {
            uint8_t *bytes = (uint8_t *)(uintptr_t)rip_dump;
            printk("| RIP bytes @0x%016llx:", rip_dump);
            for (uint64_t i = 0; i < 16; i++) {
                printk(" %02x", bytes[i]);
            }
            printk("\n");
        }

        if (context->rbx >= 0x400000 && context->rbx < KERNEL_VMA &&
            is_mapped_range(context->rbx + 0x38, 32)) {
            uint64_t *fp = (uint64_t *)(uintptr_t)(context->rbx + 0x38);
            printk("| [RBX+0x38]=0x%016llx [RBX+0x40]=0x%016llx\n", fp[0], fp[1]);
            printk("| [RBX+0x48]=0x%016llx [RBX+0x50]=0x%016llx\n", fp[2], fp[3]);
        }
    }

    uint64_t dr6_val;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6_val));
    printk("| DR6=0x%016llx (BS=%llu = 1 means single-step)\n",
           dr6_val, (dr6_val >> 14) & 1);
    printk("+------------------------------------------------------------\n");
    ipi_panic_halt();  /* broadcast halt to all APs, then halt self — does not return */
    __builtin_unreachable();
}

static void divide_error_exception(struct task_cpu_context *context, uint64_t error_code) {
    handle_exception("Divide Error Exception", context);
}

static void debug_exception(struct task_cpu_context *context) {
    /* #DB pushes no error code, so context->cs/rip/rflags are the real fault
     * fields directly (unlike #GP/#PF, which shift them — see those handlers). */
    if (context->cs == USER_CS_SEL) {
        struct thread *t = sched_current();
        if (t->ptrace_traced) {
            /* PTRACE_SINGLESTEP stop. Clear TF now — it only re-arms if the
             * tracer sends another PTRACE_SINGLESTEP; a plain PTRACE_CONT
             * should let the thread run freely. ptrace_trap_ctx lets the
             * tracer's PTRACE_CONT/PTRACE_SINGLESTEP (running in a different
             * thread) reach back and set/clear TF on this exact frame when
             * resuming — see its doc comment in sched.h. */
            context->rflags &= ~(1ULL << 8);
            t->ptrace_trap_ctx = context;
            sched_ptrace_stop(PTRACE_STOP_SINGLESTEP);
            t->ptrace_trap_ctx = NULL;
            return;
        }
        /* Not traced: default action for SIGTRAP is process termination
         * (no core dump support). */
        printk("Debug exception: user task %d killed (rip=0x%llx)\n", t->tid, context->rip);
        sched_exit_current(128 + SIGTRAP);
        __builtin_unreachable();
    }
    handle_exception("Debug Exception", context);
}

static void nmi_interrupt_exception(struct task_cpu_context *context) {
    handle_exception("NMI Interrupt Exception", context);
}

static void breakpoint_exception(struct task_cpu_context *context) {
    handle_exception("Breakpoint Exception", context);
}

static void overflow_exception(struct task_cpu_context *context) {
    handle_exception("Overflow Exception", context);
}

static void bound_range_exceeded_exception(struct task_cpu_context *context) {
    handle_exception("Bound Range Exceeded Exception", context);
}

static void invalid_opcode_exception(struct task_cpu_context *context) {
    handle_exception("Invalid Opcode Exception", context);
}

static void device_not_available_exception(struct task_cpu_context *context) {
    handle_exception("Device Not Available Exception", context);
}

static void coprocessor_segment_overrun_exception(struct task_cpu_context *context) {
    handle_exception("Coprocessor Segment Overrun Exception", context);
}

static void invalid_tss_exception(struct task_cpu_context *context) {
    handle_exception("Invalid TSS Exception", context);
}

static void segment_not_present_exception(struct task_cpu_context *context) {
    handle_exception("Segment Not Present Exception", context);
}

static void stack_segment_fault_exception(struct task_cpu_context *context) {
    handle_exception("Stack Segment Fault Exception", context);
}

static void general_protection_exception(struct task_cpu_context *context) {
    /* #GP pushes an error_code before the iretq frame; save_context's 15-register
     * push shifts context->rip to hold error_code and context->cs to hold fault RIP.
     * Extract the real fields from the raw stack layout (uint64_t array offsets). */
    uint64_t *raw = (uint64_t *)context;
    uint64_t error_code = raw[15];
    uint64_t fault_rip  = raw[16];
    uint64_t fault_cs   = raw[17];
    uint64_t fault_rsp  = raw[19];
    printk("+------------------------------------------------------------\n");
    printk("| EXCEPTION: General Protection Exception (error=0x%llx)\n", error_code);
    printk("| RAX=0x%016llx RBX=0x%016llx\n", context->rax, context->rbx);
    printk("| RCX=0x%016llx RDX=0x%016llx\n", context->rcx, context->rdx);
    printk("| RBP=0x%016llx\n", context->rbp);
    printk("| R8 =0x%016llx R9 =0x%016llx\n", context->r8, context->r9);
    printk("| R10=0x%016llx R11=0x%016llx\n", context->r10, context->r11);
    printk("| R12=0x%016llx R13=0x%016llx\n", context->r12, context->r13);
    printk("| R14=0x%016llx R15=0x%016llx\n", context->r14, context->r15);
    printk("| RSI=0x%016llx RDI=0x%016llx\n", context->rsi, context->rdi);
    printk("| RIP=0x%04llx:%016llx\n", fault_cs, fault_rip);
    printk("| RSP=0x%016llx\n", fault_rsp);
    printk("+------------------------------------------------------------\n");

    /* User-space #GP: kill the task rather than halting the system */
    if (fault_cs == USER_CS_SEL) {
        struct thread *t = sched_current();
        printk("GPF: user task %d killed (rip=0x%llx, rsp=0x%llx, error=0x%llx)\n",
               t->tid, fault_rip, fault_rsp, error_code);
        sched_exit_current(-11);  /* SIGSEGV */
        __builtin_unreachable();
    }
    ipi_panic_halt();
    __builtin_unreachable();
}

static void page_fault_exception(struct task_cpu_context *context) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* The CPU pushes error_code before the iretq frame; save_context pushes 15
     * registers (120 bytes), so error_code is at context[15]. */
    uint64_t error_code = ((uint64_t *)context)[15];

    /* Bit 2 of error_code is the U/S bit: 1 = fault came from user mode. */
    if (error_code & 0x04) {
        /* Try to resolve via demand paging (lazy anonymous mmap). */
        if (vmm_resolve_user_fault(cr2, error_code) == 0)
            return;  /* resolved — iretq resumes the faulting instruction */

        /* Unresolvable user fault — kill the task, keep the kernel alive. */
        struct thread *t = sched_current();
        uint64_t rip = ((uint64_t *)context)[16];
        uint64_t rsp = ((uint64_t *)context)[19];
        uint64_t rbp = ((uint64_t *)context)[4];
        printk("Page fault: user task %d killed (fault at 0x%llx, error 0x%x, rip=0x%llx, rsp=0x%llx, rbp=0x%llx)\n",
               t->tid, cr2, (unsigned int)error_code, rip, rsp, rbp);
        sched_exit_current(-14);  /* -EFAULT; does not return */
        __builtin_unreachable();
    }

    /* Kernel-mode fault — print CR2 then panic. */
    printk("Page fault: kernel fault at CR2=0x%llx error=0x%llx\n", cr2, error_code);
    handle_exception("Page Fault Exception", context);
}

static void floating_point_error_exception(struct task_cpu_context *context) {
    handle_exception("Floating-Point Error Exception", context);
}

static void alignment_check_exception(struct task_cpu_context *context) {
    handle_exception("Alignment Check Exception", context);
}

static void machine_check_exception(struct task_cpu_context *context) {
    handle_exception("Machine Check Exception", context);
}

void exceptions_init() {
    exception_set_handler(EXCEPT_DIVIDE_ERROR, (void *)(&divide_error_exception));
    exception_set_handler(EXCEPT_DEBUG, (void *)(&debug_exception));
    exception_set_handler(EXCEPT_NMI_INTERRUPT, (void *)(&nmi_interrupt_exception));
    exception_set_handler(EXCEPT_BREAKPOINT, (void *)(&breakpoint_exception));
    exception_set_handler(EXCEPT_OVERFLOW, (void *)(&overflow_exception));
    exception_set_handler(EXCEPT_BOUND_RANGE_EXCEDEED, (void *)(&bound_range_exceeded_exception));
    exception_set_handler(EXCEPT_INVALID_OPCODE, (void *)(&invalid_opcode_exception));
    exception_set_handler(EXCEPT_DEVICE_NOT_AVAILABLE, (void *)(&device_not_available_exception));
    exception_set_handler(EXCEPT_COPROCESSOR_SEGMENT_OVERRUN, (void *)(&coprocessor_segment_overrun_exception));
    exception_set_handler(EXCEPT_INVALID_TSS, (void *)(&invalid_tss_exception));
    exception_set_handler(EXCEPT_SEGMENT_NOT_PRESENT, (void *)(&segment_not_present_exception));
    exception_set_handler(EXCEPT_STACK_SEGMENT_FAULT, (void *)(&stack_segment_fault_exception));
    exception_set_handler(EXCEPT_GENERAL_PROTECTION, (void *)(&general_protection_exception));
    exception_set_handler(EXCEPT_PAGE_FAULT, (void *)(&page_fault_exception));
    exception_set_handler(EXCEPT_FLOATING_POINT_ERROR, (void *)(&floating_point_error_exception));
    exception_set_handler(EXCEPT_ALIGNMENT_CHECK, (void *)(&alignment_check_exception));
    exception_set_handler(EXCEPT_MACHINE_CHECK, (void *)(&machine_check_exception));
}
