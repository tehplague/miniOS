; MIT License
;
; Copyright (c) 2026 Christian Spoo
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in all
; copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.

default rel

section .text
bits 64
extern IDT
extern irq_handlers
extern exception_handler_array
global exception_wrapper_array

NR_EXCEPTIONS   equ     32
NR_IRQS         equ     16

%define EXCEPT_DIVIDE_ERROR                   0         ; No error code
%define EXCEPT_DEBUG                          1         ; No error code
%define EXCEPT_NMI_INTERRUPT                  2         ; No error code
%define EXCEPT_BREAKPOINT                     3         ; No error code
%define EXCEPT_OVERFLOW                       4         ; No error code
%define EXCEPT_BOUND_RANGE_EXCEDEED           5         ; No error code
%define EXCEPT_INVALID_OPCODE                 6         ; No error code
%define EXCEPT_DEVICE_NOT_AVAILABLE           7         ; No error code
%define EXCEPT_DOUBLE_FAULT                   8         ; Yes (Zero)
%define EXCEPT_COPROCESSOR_SEGMENT_OVERRUN    9         ; No error code
%define EXCEPT_INVALID_TSS                   10         ; Yes
%define EXCEPT_SEGMENT_NOT_PRESENT           11         ; Yes
%define EXCEPT_STACK_SEGMENT_FAULT           12         ; Yes
%define EXCEPT_GENERAL_PROTECTION            13         ; Yes
%define EXCEPT_PAGE_FAULT                    14         ; Yes
%define EXCEPT_INTEL_RESERVED_1              15         ; No
%define EXCEPT_FLOATING_POINT_ERROR          16         ; No
%define EXCEPT_ALIGNMENT_CHECK               17         ; Yes (Zero)
%define EXCEPT_MACHINE_CHECK                 18         ; No
%define EXCEPT_INTEL_RESERVED_2              19         ; No
%define EXCEPT_INTEL_RESERVED_3              20         ; No
%define EXCEPT_INTEL_RESERVED_4              21         ; No
%define EXCEPT_INTEL_RESERVED_5              22         ; No
%define EXCEPT_INTEL_RESERVED_6              23         ; No
%define EXCEPT_INTEL_RESERVED_7              24         ; No
%define EXCEPT_INTEL_RESERVED_8              25         ; No
%define EXCEPT_INTEL_RESERVED_9              26         ; No
%define EXCEPT_INTEL_RESERVED_10             27         ; No
%define EXCEPT_INTEL_RESERVED_11             28         ; No
%define EXCEPT_INTEL_RESERVED_12             29         ; No
%define EXCEPT_INTEL_RESERVED_13             30         ; No
%define EXCEPT_INTEL_RESERVED_14             31         ; No

%macro save_context 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rcx
    push rdx
    push rax
%endmacro
%macro restore_context 0
    pop rax
    pop rdx
    pop rcx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

%macro exception_wrapper 1
exception_wrapper%1:
    save_context

    lea rdi, [rsp]
    mov rsi, rax
    lea rax, [exception_handler_array]
    call [rax + 8*%1]

;    push rsp
;    call update_tss_ldt
;    add rsp, 8

    restore_context
    add rsp, 8          ; skip CPU-pushed error code (exceptions 8,10-14,17 push one)
    iretq
%endmacro

%macro exception_wrapper_noctx 1
exception_wrapper%1:
    save_context

    lea rdi, [rsp]
    mov rsi, rax
    lea rax, [exception_handler_array]
    call [rax + 8*%1]

;    push rsp
;    call update_tss_ldt
;    add rsp, 8

    restore_context
    iretq
%endmacro

%macro exception_wrapper_hlt 1
exception_wrapper%1:
.loop:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    hlt
    jmp .loop
%endmacro

    exception_wrapper_noctx EXCEPT_DIVIDE_ERROR
    exception_wrapper_noctx EXCEPT_DEBUG
    exception_wrapper_noctx EXCEPT_NMI_INTERRUPT
    exception_wrapper_noctx EXCEPT_BREAKPOINT
    exception_wrapper_noctx EXCEPT_OVERFLOW
    exception_wrapper_noctx EXCEPT_BOUND_RANGE_EXCEDEED
    exception_wrapper_noctx EXCEPT_INVALID_OPCODE
    exception_wrapper_noctx EXCEPT_DEVICE_NOT_AVAILABLE
    exception_wrapper_hlt   EXCEPT_DOUBLE_FAULT
    exception_wrapper_noctx EXCEPT_COPROCESSOR_SEGMENT_OVERRUN
    exception_wrapper       EXCEPT_INVALID_TSS
    exception_wrapper       EXCEPT_SEGMENT_NOT_PRESENT
    exception_wrapper       EXCEPT_STACK_SEGMENT_FAULT
    exception_wrapper       EXCEPT_GENERAL_PROTECTION
    exception_wrapper       EXCEPT_PAGE_FAULT
    exception_wrapper       EXCEPT_ALIGNMENT_CHECK
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_1
    exception_wrapper_noctx EXCEPT_FLOATING_POINT_ERROR
    exception_wrapper_noctx EXCEPT_MACHINE_CHECK
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_2
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_3
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_4
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_5
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_6
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_7
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_8
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_9
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_10
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_11
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_12
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_13
    exception_wrapper_noctx EXCEPT_INTEL_RESERVED_14

; LAPIC spurious interrupt handler — just iretq, no EOI needed for spurious vector.
global lapic_spurious_isr
lapic_spurious_isr:
    iretq

exception_wrapper_array:
%assign i 0
%rep 32
    dq exception_wrapper %+ i
%assign i i+1
%endrep

; -----------------------------------------------------------------------
; IRQ wrappers for external hardware interrupts (vectors 32-47)
; Each wrapper: saves context, calls irq_dispatch(irq_num, frame_ptr),
; sends LAPIC EOI via lapic_eoi_asm(), restores context, iretq.
; -----------------------------------------------------------------------

global irq_wrapper_array
extern irq_dispatch
extern lapic_eoi_asm

%macro irq_wrapper 1
irq_wrapper%1:
    save_context            ; pushes rax..r15
    mov  rdi, %1            ; arg1: IRQ number
    lea  rsi, [rsp]         ; arg2: pointer to saved context frame
    call irq_dispatch
    call lapic_eoi_asm      ; send LAPIC EOI before re-enabling interrupts
    restore_context
    iretq
%endmacro

%assign i 0
%rep 16
    irq_wrapper i
%assign i i+1
%endrep

irq_wrapper_array:
%assign i 0
%rep 16
    dq irq_wrapper %+ i
%assign i i+1
%endrep

; -----------------------------------------------------------------------
; LAPIC timer wrapper — calls lapic_timer_isr(frame_ptr)
; EOI is sent inside lapic_timer_isr() via lapic_eoi_asm().
; -----------------------------------------------------------------------
global lapic_timer_wrapper
extern lapic_timer_isr

lapic_timer_wrapper:
    save_context
    lea  rdi, [rsp]         ; frame pointer as first arg
    call lapic_timer_isr
    restore_context
    iretq

; -----------------------------------------------------------------------
; Scheduler-kick IPI wrapper — calls sched_kick_isr(frame_ptr)
; EOI is sent inside sched_kick_isr() via lapic_eoi_asm().
; -----------------------------------------------------------------------
global sched_kick_wrapper
extern sched_kick_isr

sched_kick_wrapper:
    save_context
    lea  rdi, [rsp]         ; frame pointer as first arg
    call sched_kick_isr
    restore_context
    iretq

; -----------------------------------------------------------------------
; TLB-shootdown IPI wrappers — one per possible initiator CPU (0..7, must
; match MAX_CPUS's default of 8; see TLB_SHOOTDOWN_VECTOR_BASE in apic.h).
; Each calls tlb_shootdown_isr(src_cpu) with its own baked-in CPU index so
; the handler knows which g_tlb_barriers[] slot to service — invlpg,
; barrier decrement, and EOI are all inside tlb_shootdown_isr().
; -----------------------------------------------------------------------
extern tlb_shootdown_isr

%macro TLB_SHOOTDOWN_WRAPPER 1
global tlb_shootdown_wrapper%1
tlb_shootdown_wrapper%1:
    save_context
    mov  edi, %1            ; src_cpu as first arg
    call tlb_shootdown_isr
    restore_context
    iretq
%endmacro

TLB_SHOOTDOWN_WRAPPER 0
TLB_SHOOTDOWN_WRAPPER 1
TLB_SHOOTDOWN_WRAPPER 2
TLB_SHOOTDOWN_WRAPPER 3
TLB_SHOOTDOWN_WRAPPER 4
TLB_SHOOTDOWN_WRAPPER 5
TLB_SHOOTDOWN_WRAPPER 6
TLB_SHOOTDOWN_WRAPPER 7

; -----------------------------------------------------------------------
; Panic-halt IPI wrapper — calls panic_halt_isr(frame_ptr)
; panic_halt_isr() executes cli; hlt and does not return.
; -----------------------------------------------------------------------
global panic_halt_wrapper
extern panic_halt_isr

panic_halt_wrapper:
    save_context
    lea  rdi, [rsp]         ; frame pointer as first arg
    call panic_halt_isr
    restore_context
    iretq
