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

; User-mode stub: calls SYS_write then SYS_exit
; This code runs at ring 3. It is linked into the kernel binary and copied to 0x400000.
bits 64

global user_stub_start
global user_stub_end

section .rodata

user_stub_start:
    ; SYS_write(fd=1, buf=msg, count=msg_len) using SYSCALL
    mov  rax, 1                    ; SYS_write
    mov  rdi, 1                    ; fd = stdout
    lea  rsi, [rel .msg]           ; buf — RIP-relative; displacement is fixed after copy to 0x400000
    mov  rdx, .msg_len
    syscall

    ; SYS_exit(code=0)
    mov  rax, 60                   ; SYS_exit
    xor  rdi, rdi                  ; exit code 0
    syscall

    ; Should never reach here
    hlt

.msg:    db "Hello from ring 3!", 10   ; newline
.msg_len equ $ - .msg

user_stub_end:
