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

global long_mode_start
extern kernel_main

KERNEL_VMA equ 0xFFFF800000000000

section .text
bits 64

; Entered at the physical address by the far jump in main.asm.
; Identity mapping is active, so execution works here.
; Immediately perform an absolute jump to the higher-half virtual address.
long_mode_start:
    mov rax, higher_half_entry
    jmp rax

higher_half_entry:
    ; RIP is now in the higher half. Fix up RSP too so the stack
    ; pointer is also a virtual address (important if identity map
    ; is removed later).
    mov rax, KERNEL_VMA
    add rsp, rax

    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rbx        ; pass Multiboot 2 info physical address as first argument
    call kernel_main
.loop:
    hlt
    jmp .loop
