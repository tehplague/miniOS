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
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.

bits 64
section .text

; wrmsr_gs_base(uint64_t addr) — write addr to IA32_GS_BASE MSR (0xC0000101)
; and IA32_KERNEL_GS_BASE (0xC0000102).
; Called from per_cpu_init() with RDI = address of cpu_t.
; Splits 64-bit RDI into EDX:EAX for wrmsr.
global wrmsr_gs_base
wrmsr_gs_base:
    mov rax, rdi
    mov rdx, rdi
    shr rdx, 32
    mov ecx, 0xC0000101   ; IA32_GS_BASE
    wrmsr
    mov ecx, 0xC0000102   ; IA32_KERNEL_GS_BASE (for swapgs paths)
    wrmsr
    ret

; lgdt_ap(void *gdt_desc) — load GDT from descriptor pointed to by RDI.
; Used by APs to load their per-CPU GDT and reload code/data segments.
global lgdt_ap
lgdt_ap:
    lgdt [rdi]
    ; Reload CS via far return trick: push new CS then return address, retfq
    pop  rax           ; save return address
    push qword 0x08    ; kernel CS selector
    push rax
    retfq
    ; Reload data segments (unreachable via retfq — CPU jumps to caller)
    ; The following segment reloads happen after the far return lands
    ; back in per_cpu_init() — not here. lgdt_ap returns to per_cpu_init
    ; with CS already reloaded; per_cpu_init reloads DS/ES/SS itself.
