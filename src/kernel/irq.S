#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/context.h>
#include <miniOS/arch/x86_64/segment.h>
#if 0
.globl irq_wrapper_array
.globl exception_wrapper_array

.globl irq_install
.extern IDT
irq_install:


.irp level,       \
    IRQ_TIMER,    \
    IRQ_KEYBOARD, \
    SLAVE_PIC,    \
    IRQ_COM2,     \
    IRQ_COM1,     \
    IRQ_LPT2,     \
    IRQ_FLOPPY,   \
    IRQ_LPT1

.irq_wrapper\level:
        pushq $0

        save_context
        restore_system_segments

        movb $PIC_EOI, %al
        outb %al, $PIC1_CMD

        pushq %rsp
        leaq irq_handler_array, %rdi
        callq *\level*4(%rdi)
        add  $8, %rsp

        pushq %rsp
        callq update_tss_ldt
        add  $8, %rsp

        restore_context
        iretq
.endr

.irp    level, \
        IRQ_RT_CLOCK, \
        MASTER_PIC, \
        IRQ_AVAILABLE_1, \
        IRQ_AVAILABLE_2, \
        IRQ_PS2_MOUSE, \
        IRQ_COPROCESSOR, \
        IRQ_PRIMARY_IDE, \
        IRQ_SECONDARY_IDE

.irq_wrapper\level:
        pushq $0

        save_context
        restore_system_segments

        movb $PIC_EOI, %al
        outb %al, $PIC2_CMD
        outb %al, $PIC1_CMD

        pushq %rsp
        leaq irq_handler_array, %rdi
        callq *\level*4(%rdi)
        add  $8, %rsp

        pushq %rsp
        callq update_tss_ldt
        add  $8, %rsp

        restore_context

        iretq
.endr

.irp level,                                 \
        EXCEPT_DIVIDE_ERROR,                \
        EXCEPT_DEBUG,                       \
        EXCEPT_NMI_INTERRUPT,               \
        EXCEPT_BREAKPOINT,                  \
        EXCEPT_OVERFLOW,                    \
        EXCEPT_BOUND_RANGE_EXCEDEED,        \
        EXCEPT_INVALID_OPCODE,              \
        EXCEPT_DEVICE_NOT_AVAILABLE,        \
        EXCEPT_COPROCESSOR_SEGMENT_OVERRUN, \
        EXCEPT_INTEL_RESERVED_1,            \
        EXCEPT_FLOATING_POINT_ERROR,        \
        EXCEPT_MACHINE_CHECK,               \
        EXCEPT_INTEL_RESERVED_2,            \
        EXCEPT_INTEL_RESERVED_3,            \
        EXCEPT_INTEL_RESERVED_4,            \
        EXCEPT_INTEL_RESERVED_5,            \
        EXCEPT_INTEL_RESERVED_6,            \
        EXCEPT_INTEL_RESERVED_7,            \
        EXCEPT_INTEL_RESERVED_8,            \
        EXCEPT_INTEL_RESERVED_9,            \
        EXCEPT_INTEL_RESERVED_10,           \
        EXCEPT_INTEL_RESERVED_11,           \
        EXCEPT_INTEL_RESERVED_12,           \
        EXCEPT_INTEL_RESERVED_13,           \
        EXCEPT_INTEL_RESERVED_14

.exception_wrapper\level:
        pushq $0

        save_context
        restore_system_segments

        pushq %rsp
        leaq exception_handler_array, %rdi
        callq *\level*4(%rdi)
        add  $8, %rsp

        pushq %rsp
        callq update_tss_ldt
        add  $8, %rsp

        restore_context

        iretq
.endr

.irp level,                         \
        EXCEPT_INVALID_TSS,         \
        EXCEPT_SEGMENT_NOT_PRESENT, \
        EXCEPT_STACK_SEGMENT_FAULT, \
        EXCEPT_GENERAL_PROTECTION,  \
        EXCEPT_PAGE_FAULT,          \
        EXCEPT_ALIGNMENT_CHECK

.exception_wrapper\level:
        save_context
        restore_system_segments

        pushq %rsp
        leaq exception_handler_array, %rdi
        callq *\level*4(%rdi)
        add  $8, %rsp

        pushq %rsp
        callq update_tss_ldt
        add  $8, %rsp

        iretq
.endr

// Prevent triple faults by halting the CPU at double faults
.irp level, EXCEPT_DOUBLE_FAULT
.exception_wrapper\level:
1:      hlt
        jmp 1b
.endr

irq_wrapper_array:
.irp level, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
.long .irq_wrapper\level
.endr

exception_wrapper_array:
.irp level, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, \
           16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
.long .exception_wrapper\level
.endr
#endif
