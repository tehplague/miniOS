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

bits 64
section .text
global _start
extern main
extern __libc_init_array   ; Newlib constructor array (calls __attribute__((constructor)) functions)

_start:
    ; rdi = argc, rsi = argv, rdx = envp (set by kernel SYS_execve per System V x86-64 ABI)
    ; For the initial shell launch, envp is passed in r15 by kernel_main
    ; Stack is 16-byte aligned at entry per ABI
    ; Check if rdx is zero (no envp from execve), if so use r15 (from kernel launch)
    test rdx, rdx
    jnz .have_envp
    test r15, r15        ; Check if r15 is valid
    jz .no_envp
    mov rdx, r15         ; Use envp from kernel launch (initial shell)
.have_envp:
    ; Save argc, argv, envp across __libc_init_array call (they may be clobbered)
    push rdi
    push rsi
    push rdx
    ; Call Newlib constructor array — invokes __attribute__((constructor)) functions,
    ; including __register_sigtrampoline() in newlib-stubs.c (Phase 47 SA_RESTART)
    call __libc_init_array
    pop rdx
    pop rsi
    pop rdi
    ; Pass argc, argv, and envp to main(argc, argv, envp)
    call main
    ; Use return value as exit code
    mov rdi, rax
    mov rax, 60         ; SYS_exit
    syscall
    hlt                 ; should not reach

.no_envp:
    ; No environment variables - pass NULL as envp
    xor rdx, rdx        ; envp = NULL
    jmp .have_envp
