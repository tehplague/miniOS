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

; ssize_t write(int fd, const void *buf, size_t count)
; Linux x86-64 ABI: rdi=fd, rsi=buf, rdx=count
global write
write:
    mov rax, 1          ; SYS_write
    syscall
    ret

; ssize_t read(int fd, void *buf, size_t count)
; Linux x86-64 ABI: rdi=fd, rsi=buf, rdx=count
global read
read:
    mov rax, 0          ; SYS_read
    syscall
    ret

; void exit(int code) - noreturn
; Linux x86-64 ABI: rdi=code
global exit
exit:
    mov rax, 60         ; SYS_exit
    syscall
    hlt

; pid_t fork(void)
global fork
fork:
    mov rax, 57         ; SYS_fork
    syscall
    ret

; int exec(const char *path) -- rdi already has path
global exec
exec:
    mov rax, 59         ; SYS_execve (reuse number; kernel will handle as exec)
    syscall
    ret

; pid_t _libc_wait(int *status)
; Note: 'wait' is a reserved NASM mnemonic (FWAIT); use %define to create alias.
; Kept for API compatibility — see waitpid() for the correct child-specific call.
global _libc_wait
_libc_wait:
    mov rax, 61         ; SYS_wait4 (reuse; kernel maps to our SYS_wait)
    syscall
    ret

; pid_t waitpid(pid_t pid, int *status, int options)
; Linux x86-64 ABI: rdi=pid, rsi=status ptr, rdx=options
; Kernel SYS_wait4: arg1=child_tid — rdi passes pid directly as arg1.
global waitpid
waitpid:
    mov rax, 61         ; SYS_wait4
    syscall
    ret

; int open(const char *path) -- rdi=path, flags defaulted to O_RDONLY in kernel
global open
open:
    xor rsi, rsi        ; flags = O_RDONLY = 0
    xor rdx, rdx        ; mode = 0
    mov rax, 2          ; SYS_open
    syscall
    ret

; int close(int fd) -- rdi=fd
global close
close:
    mov rax, 3          ; SYS_close
    syscall
    ret
