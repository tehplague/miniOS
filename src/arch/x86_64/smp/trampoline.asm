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

; AP startup trampoline: 16-bit real mode -> 32-bit protected -> 64-bit long mode
;
; Copied at runtime to physical 0x8000 (SIPI vector 0x08 = 0x8000 / 0x1000).
; All internal absolute addresses are computed as TRAMP_BASE + offset-within-section
; so that they evaluate to pure integer constants (no ELF relocations) and are
; correct after the trampoline is copied to physical 0x8000.
;
; Pointer table written by smp_boot.c BEFORE the SIPI is sent:
;   Physical 0x8FE8 (8 bytes) = AP bootstrap stack top (per-AP, written each loop)
;   Physical 0x8FF0 (8 bytes) = BSP cr3 physical address
;   Physical 0x8FF8 (8 bytes) = ap_entry() virtual address

; Physical base where trampoline will be copied at runtime
TRAMP_BASE equ 0x8000

BITS 16
section .trampoline

global trampoline_start
global trampoline_end

trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Load embedded GDT descriptor using its physical address (pure constant).
    lgdt [TRAMP_BASE + (tramp_gdt_desc - trampoline_start)]

    ; Enable protected mode (CR0.PE = bit 0)
    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ; Far jump to 32-bit code segment (CS=0x08).
    ; Physical address of tramp32 = TRAMP_BASE + offset-within-section.
    ; 'dword' prefix encodes a 32-bit offset in the far-jump.
    jmp dword 0x08:(TRAMP_BASE + (tramp32 - trampoline_start))

; The 32-bit and 64-bit sections use absolute physical addresses (0x8FF0, 0x8FF8)
; for the pointer table; DEFAULT ABS suppresses the NASM implicit-abs warning.
DEFAULT ABS

BITS 32
align 4
tramp32:
    mov ax, 0x10     ; data segment selector (index 2 * 8 = 0x10)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; Load BSP cr3 from physical 0x8FF0
    mov eax, [0x8FF0]
    mov cr3, eax

    ; Enable long mode: set EFER.LME (bit 8) in IA32_EFER MSR (0xC0000080)
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)
    wrmsr

    ; Enable paging + protected mode (CR0.PG = bit 31, CR0.PE = bit 0)
    mov eax, cr0
    or  eax, (1 << 31) | 1
    mov cr0, eax

    ; Far jump to 64-bit code segment (CS=0x18, selector index 3).
    ; This reloads CS with the 64-bit descriptor (L=1, D=0), switching the
    ; CPU from IA-32e compatibility mode into true 64-bit long mode.
    jmp 0x18:(TRAMP_BASE + (tramp64 - trampoline_start))

BITS 64
align 8
tramp64:
    ; Now in true 64-bit long mode. RIP is a physical/low address.
    ; Set up the AP bootstrap stack before any C code (including ap_entry).
    ; RSP after CPU reset is 0; calling C with RSP=0 triple-faults immediately.
    ; smp_boot_aps() writes each AP's stack top to physical 0x8FE8 before SIPI.
    mov rsp, [0x8FE8]   ; load per-AP bootstrap stack top
    mov rax, [0x8FF8]   ; load ap_entry() virtual address
    jmp rax             ; jump to ap_entry() in the higher-half kernel

; ---- embedded GDT for trampoline transitions ----
align 8
tramp_gdt:
    dq 0                           ; 0x00: null descriptor
    dq 0x00CF9A000000FFFF          ; 0x08: 32-bit code: base=0, limit=4GB, ring 0 (16->32 jump)
    dq 0x00CF92000000FFFF          ; 0x10: 32-bit data: base=0, limit=4GB, ring 0
    dq 0x00AF9A000000FFFF          ; 0x18: 64-bit code: L=1, D=0, ring 0 (32->64 jump)
tramp_gdt_end:

; GDT pseudo-descriptor: 2-byte limit + 4-byte base (physical address, pure constant)
tramp_gdt_desc:
    dw tramp_gdt_end - tramp_gdt - 1                  ; limit = sizeof(GDT) - 1
    dd TRAMP_BASE + (tramp_gdt - trampoline_start)    ; base = physical address

trampoline_end:
