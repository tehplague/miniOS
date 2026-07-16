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
global context_switch_asm
section .text
bits 64

; context_switch_asm(struct task_cpu_context *old_ctx, struct task_cpu_context *new_ctx)
; rdi = old_ctx, rsi = new_ctx
;
; Calling convention: System V AMD64 ABI.
; Saves all CPU registers of the outgoing thread into *old_ctx, then
; restores all CPU registers from *new_ctx and jumps to new_ctx.rip.
;
; struct task_cpu_context field offsets (all uint64_t, packed):
;   0   rax
;   8   rdx
;  16   rcx
;  24   rbx
;  32   rbp
;  40   rsi
;  48   rdi
;  56   r8
;  64   r9
;  72   r10
;  80   r11
;  88   r12
;  96   r13
; 104   r14
; 112   r15
; 120   rip
; 128   cs
; 136   rflags
; 144   rsp
; 152   ss
; 160   fs_base

context_switch_asm:
    ; --- SAVE outgoing thread state into [rdi] ---
    ; Note: rdi and rsi currently hold the arguments, so save them last after
    ; capturing their values. We save rdi/rsi BEFORE overwriting them.

    mov  [rdi + 0],   rax
    mov  [rdi + 8],   rdx
    mov  [rdi + 16],  rcx
    mov  [rdi + 24],  rbx
    mov  [rdi + 32],  rbp
    ; Save original rsi (arg2 = new_ctx ptr) into old_ctx.rsi
    mov  [rdi + 40],  rsi
    ; Save original rdi (arg1 = old_ctx ptr) into old_ctx.rdi — rdi IS the old_ctx
    ; pointer, but that is fine — on resume we will overwrite rdi from new_ctx anyway.
    mov  [rdi + 48],  rdi
    mov  [rdi + 56],  r8
    mov  [rdi + 64],  r9
    mov  [rdi + 72],  r10
    mov  [rdi + 80],  r11
    mov  [rdi + 88],  r12
    mov  [rdi + 96],  r13
    mov  [rdi + 104], r14
    mov  [rdi + 112], r15

    ; Save return address (rip of caller) — it is at [rsp] right now (call pushed it)
    mov  rax, [rsp]
    mov  [rdi + 120], rax        ; old_ctx.rip = return address

    ; Save cs
    mov  rax, cs
    mov  [rdi + 128], rax        ; old_ctx.cs

    ; Save rflags
    pushfq
    pop  rax
    mov  [rdi + 136], rax        ; old_ctx.rflags

    ; Save rsp (the value caller had, i.e. rsp+8 to skip over the return address)
    lea  rax, [rsp + 8]
    mov  [rdi + 144], rax        ; old_ctx.rsp

    ; Save ss
    mov  rax, ss
    mov  [rdi + 152], rax        ; old_ctx.ss

    ; Save user FS.base (TLS pointer)
    rdfsbase rax
    mov  [rdi + 160], rax        ; old_ctx.fs_base

    ; --- RESTORE incoming thread state from [rsi] ---
    ; Load new rsp first so we are on the new stack
    mov  rsp, [rsi + 144]        ; rsp = new_ctx.rsp

    ; Push new rip onto new stack so we can ret to it
    mov  rax, [rsi + 120]        ; rax = new_ctx.rip
    push rax                     ; push new rip for use with ret below

    ; NOTE: do NOT restore RFLAGS here.
    ; For interrupt-driven switches (timer preemption): the interrupt wrapper's
    ; iretq will restore the correct RFLAGS (including IF=1) from the saved
    ; interrupt frame.  Restoring IF=1 via popfq here would re-enable interrupts
    ; mid-switch, allowing a nested timer interrupt before ret completes — this
    ; corrupts RSI (which still points to the new_ctx struct at that moment).
    ; For cooperative switches (sched_yield): IF is preserved as-is from the
    ; caller; the resuming thread re-enables interrupts explicitly if needed.

    ; Restore general purpose registers from new_ctx.
    ; Note: restore rdi and rsi last since we are still using rsi as new_ctx ptr.
    mov  rax, [rsi + 0]
    mov  rdx, [rsi + 8]
    mov  rcx, [rsi + 16]
    mov  rbx, [rsi + 24]
    mov  rbp, [rsi + 32]
    mov  r8,  [rsi + 56]
    mov  r9,  [rsi + 64]
    mov  r10, [rsi + 72]
    mov  r11, [rsi + 80]
    mov  r12, [rsi + 88]
    mov  r13, [rsi + 96]
    mov  r14, [rsi + 104]
    mov  r15, [rsi + 112]

    ; Restore user FS.base (TLS pointer) before we lose the new_ctx pointer.
    ; Use rax as scratch (already restored above; re-load it after wrfsbase).
    mov  rax, [rsi + 160]        ; rax = new_ctx.fs_base
    wrfsbase rax                 ; set FS.base for incoming thread
    mov  rax, [rsi + 0]         ; re-restore rax from new_ctx

    ; Restore rdi and rsi last (they are used as base pointers above)
    mov  rdi, [rsi + 48]
    mov  rsi, [rsi + 40]         ; rsi = new_ctx.rsi (must be last use of rsi as ptr)

    ; Jump to new rip (was pushed onto new stack earlier)
    ret
