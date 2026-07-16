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

global start
extern long_mode_start

; Must match linker.ld KERNEL_VMA. Subtracted in 32-bit code so
; all symbol references resolve to physical addresses at link time.
KERNEL_VMA equ 0xFFFF800000000000

section .boot
bits 32
start:
    mov esp, (stack_top - KERNEL_VMA)

    ; EBX holds the Multiboot 2 info physical address set by GRUB.
    ; Save it now: cpuid (used in check_cpuid and check_long_mode) clobbers EBX.
    push ebx

    call check_multiboot
    call check_cpuid
    call check_long_mode

    ; Restore Multiboot 2 info pointer before leaving 32-bit mode.
    pop ebx

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer - KERNEL_VMA]
    jmp gdt64.code_segment:(long_mode_start - KERNEL_VMA)

    hlt

setup_page_tables:
    ; Identity map: PML4[0] -> L3 -> L2 -> physical 0..1GB
    mov eax, (page_table_l3 - KERNEL_VMA)
    or eax, 0b11
    mov [(page_table_l4 - KERNEL_VMA)], eax

    ; Higher-half map: PML4[256] -> L3_high -> same L2 -> physical 0..1GB
    ; PML4 index 256 covers 0xFFFF800000000000
    mov eax, (page_table_l3_high - KERNEL_VMA)
    or eax, 0b11
    mov [(page_table_l4 - KERNEL_VMA) + 256 * 8], eax

    mov eax, (page_table_l2 - KERNEL_VMA)
    or eax, 0b11
    mov [(page_table_l3 - KERNEL_VMA)], eax
    mov [(page_table_l3_high - KERNEL_VMA)], eax    ; reuse same L2 for higher half

    ; Map 3..4GB range via L3[3] -> page_table_l2_mmio, for LAPIC MMIO at 0xFEE00000.
    ; Identity map and higher-half map both get this.
    mov eax, (page_table_l2_mmio - KERNEL_VMA)
    or eax, 0b11
    mov [(page_table_l3 - KERNEL_VMA) + 3 * 8], eax
    mov [(page_table_l3_high - KERNEL_VMA) + 3 * 8], eax

    ; Fill 0..1GB L2 with 2MB identity-mapped huge pages
    mov ecx, 0
.loop:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011
    mov [(page_table_l2 - KERNEL_VMA) + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jb .loop

    ; Map IOAPIC MMIO (0xFEC00000) in the 3..4GB L2.
    ; 0xFEC00000 is at offset 0x3EC00000 from 3GB, which is 2MB-entry index 502.
    mov eax, 0xFEC00000
    or eax, 0b10000011         ; huge page (PS), present, writable
    mov [(page_table_l2_mmio - KERNEL_VMA) + 502 * 8], eax

    ; Map LAPIC MMIO (0xFEE00000) in the 3..4GB L2.
    ; 0xFEE00000 is at offset 0x3EE00000 from 3GB, which is 2MB-entry index 503.
    mov eax, 0xFEE00000
    or eax, 0b10000011         ; huge page (PS), present, writable
    mov [(page_table_l2_mmio - KERNEL_VMA) + 503 * 8], eax

    ret

enable_paging:
    mov eax, (page_table_l4 - KERNEL_VMA)
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ret

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "M"
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "C"
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ret
.no_long_mode:
    mov al, "L"
    jmp error

error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte [0xb800a], al
    hlt

section .bss
align 4096
global interrupt_stack_top
page_table_l4:
    times 4096 db 0
page_table_l3:
    times 4096 db 0
page_table_l3_high:
    times 4096 db 0
page_table_l2:
    times 4096 db 0
page_table_l2_mmio:
    times 4096 db 0
interrupt_stack_bottom:
    times 4096 * 1 db 0
interrupt_stack_top:
stack_bottom:
    times 4096 * 4 db 0
stack_top:

section .rodata
gdt64:
    dq 0
.code_segment:  equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
