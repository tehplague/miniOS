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
default rel

extern syscall_dispatch
extern syscall_save_user_regs
extern syscall_save_user_callee_regs
extern syscall_get_saved_user_callee_regs
extern syscall_get_saved_user_ctx
extern syscall_get_saved_user_rdi_rsi
extern syscall_get_saved_user_rdi_rsi_rdx
extern signal_dispatch
extern sched_current
global syscall_entry
global int80_entry
; cpu_t GS-relative offsets (must match struct cpu_t in smp.h).
; GS.base = &g_cpus[cpu_id] on every CPU (set by per_cpu_init via wrmsr_gs_base).
; These fields are per-CPU so concurrent syscalls on different CPUs are isolated.
; kstack_top (gs:64)       — kernel stack top for this CPU's current thread.
;                            Updated by per_cpu_update_rsp0() on every sched_schedule.
; user_rsp_scratch (gs:72) — scratch slot to park user RSP before the stack switch.
CPU_T_KSTACK_TOP       equ 64
CPU_T_USER_RSP_SCRATCH equ 72

; syscall_kernel_rsp_storage — kept as a global symbol for backward compatibility
; with any object that still has an extern reference, but it is NO LONGER USED
; by syscall_entry.  The per-CPU gs:CPU_T_KSTACK_TOP value is used instead.
global syscall_kernel_rsp_storage

section .data
syscall_kernel_rsp_storage: dq 0    ; legacy — no longer read by syscall_entry

section .text

; ---------------------------------------------------------------------------
; SYSCALL entry point — loaded into IA32_LSTAR
;
; On SYSCALL entry (hardware):
;   RCX = saved user RIP (instruction after syscall)
;   R11 = saved user RFLAGS
;   RSP still points at USER stack
;   IF cleared by SFMASK
;
; Linux syscall ABI: RAX=nr, RDI=arg1, RSI=arg2, RDX=arg3
; C dispatch ABI:    RDI=nr, RSI=arg1, RDX=arg2, RCX=arg3
; ---------------------------------------------------------------------------
syscall_entry:
    ; 1. Switch to kernel stack (per-CPU via GS to avoid SMP races).
    ;    GS.base = &g_cpus[cpu_id] throughout kernel AND user execution on this CPU
    ;    (SYSRET does not modify GS, and miniOS never calls swapgs).
    ;    user_rsp_scratch (gs:CPU_T_USER_RSP_SCRATCH) is a per-CPU slot — safe
    ;    because only one thread runs per CPU at any time.
    mov  gs:[CPU_T_USER_RSP_SCRATCH], rsp   ; park user RSP per-CPU
    mov  rsp, gs:[CPU_T_KSTACK_TOP]          ; load this CPU's kernel stack

    ; Preserve user volatile GPRs exactly as the x86_64 SYSCALL ABI expects.
    ; User-space inline syscall wrappers assume only RCX and R11 are clobbered.
    push r10
    push r9
    push r8
    push rdx
    push rsi
    push rdi
    push rax                             ; syscall number / saved user RAX

    ; 1a. Reload DS/ES — SYSRET zeros segment registers on AMD; without this,
    ;     any memory access using DS after returning from ring 3 causes #GP.
    mov  ax, 0x10           ; kernel data segment selector
    mov  ds, ax
    mov  es, ax

    ; 2. Save user-mode RIP/RFLAGS/RSP into current thread's ctx for fork()/exec().
    ;    syscall_save_user_regs(user_rip, user_rflags, user_rsp)
    mov  rdi, rcx                        ; user RIP  → arg1
    mov  rsi, r11                        ; user RFLAGS → arg2
    mov  rdx, gs:[CPU_T_USER_RSP_SCRATCH] ; user RSP  → arg3 (per-CPU scratch)
    call syscall_save_user_regs

    ; 2b. Save callee-saved user registers into thread struct BEFORE pushing them onto
    ;     the kernel stack.  At this point rbp/rbx/r12-r15 still hold USER values
    ;     (SYSCALL does not touch them; syscall_save_user_regs is callee-save-safe).
    ;     fork_child_trampoline reads these fields to restore the child's stack frame.
    mov  rdi, rbp
    mov  rsi, rbx
    mov  rdx, r12
    mov  rcx, r13
    mov  r8,  r14
    mov  r9,  r15
    call syscall_save_user_callee_regs

    ; 3. Save callee-saved registers
    push r15
    push r14
    push r13
    push r12
    push rbx
    push rbp

    ; 4. Dispatch: syscall_dispatch(nr, arg1, arg2, arg3)
    ;    Re-enable interrupts: SYSCALL entry clears IF via SFMASK, so blocking
    ;    syscalls (e.g. SYS_read waiting on keyboard IRQ1) would hlt forever.
    ;
    ;    SMAP note: clac/stac are only valid on CPUs that report SMAP support
    ;    (CPUID leaf 7 EBX bit 20).  QEMU -cpu Haswell does not include SMAP,
    ;    so those instructions raise #UD.  Use pushfq/popfq to clear/set the
    ;    AC flag (RFLAGS bit 18) instead — valid on all x86-64 CPUs.  When
    ;    SMAP is disabled the AC bit is ignored; when enabled, this correctly
    ;    gates user-space access.
    pushfq
    and qword [rsp], ~(1 << 18)         ; clear AC (SMAP: no user-mem access yet)
    popfq
    sti
    mov  rdi, [rsp + 48]                 ; nr   (saved user RAX)
    mov  rsi, [rsp + 56]                 ; arg1 (user RDI — addr for mmap)
    mov  rdx, [rsp + 64]                 ; arg2 (user RSI — len for mmap)
    mov  rcx, [rsp + 72]                 ; arg3 (user RDX — prot for mmap)
    mov  r8,  [rsp + 96]                 ; arg4 (user R10 — Linux arg4: flags for mmap; dest_addr for sendto)
    mov  r9,  [rsp + 80]                 ; arg5 (user R8  — Linux arg5: fd for mmap; addrlen for sendto)
    push qword [rsp + 88]               ; arg6 (user R9  — Linux arg6: offset for mmap); 7th C param on stack
    pushfq
    or qword [rsp], (1 << 18)           ; set AC — allow dispatch to access user-space buffers
    popfq
    call syscall_dispatch     ; return value → rax
    pushfq
    and qword [rsp], ~(1 << 18)         ; clear AC — revoke user-space access after dispatch
    popfq
    add  rsp, 8               ; clean up pushed arg6
    cli                       ; disable interrupts before sysretq

    ; 4b. Deliver pending signals before returning to userspace.
    ;     signal_dispatch() checks pending_signals bitmask; delivers SIGKILL/SIGTERM
    ;     by calling sched_exit_current (does not return); invokes registered handlers
    ;     by adjusting saved_user_rip/rsp (picked up in step 6).
    ;     Must run AFTER cli so signal delivery is atomic wrt timer ISR.
    ;     Preserve rax (syscall return value) across the call.
    push rax
    call signal_dispatch
    pop  rax

    ; 5. Restore callee-saved registers
    pop rbp
    pop rbx
    pop r12
    pop r13
    pop r14
    pop r15

    ; 6. Reload user RIP, RFLAGS, RSP, and callee-saved registers from
    ;    thread->saved_user_* before SYSRET.
    ;    These dedicated per-thread fields survive preemptive context switches:
    ;    context_switch_asm saves/restores ctx.rcx with the KERNEL rcx value,
    ;    so using ctx.rcx here would return to a stale/garbage address after any
    ;    preemption while blocked inside a syscall (e.g. SYS_read, SYS_fork).
    ;    exec() overwrites saved_user_rip with the new ELF entry point.
    ;    Reading RSP from the thread struct (not the global syscall_user_rsp
    ;    scratch) is critical: SYS_fork blocks the parent while the child runs,
    ;    and the child's own SYS_execve/SYS_write/SYS_exit calls all overwrite
    ;    the global before the parent resumes.
    ;    syscall_get_saved_user_ctx(uint64_t *out_rip, uint64_t *out_rfl, uint64_t *out_rsp)
    push rax                  ; preserve return value
    sub  rsp, 72              ; [0]=r15 [8]=r14 [16]=r13 [24]=r12 [32]=rbx [40]=rbp [48]=rsp [56]=rfl [64]=rip
    lea  rdi, [rsp + 64]      ; out_rip
    lea  rsi, [rsp + 56]      ; out_rfl
    lea  rdx, [rsp + 48]      ; out_rsp
    call syscall_get_saved_user_ctx
    lea  rdi, [rsp + 40]      ; out_rbp
    lea  rsi, [rsp + 32]      ; out_rbx
    lea  rdx, [rsp + 24]      ; out_r12
    lea  rcx, [rsp + 16]      ; out_r13
    lea  r8,  [rsp + 8]       ; out_r14
    lea  r9,  [rsp + 0]       ; out_r15
    call syscall_get_saved_user_callee_regs
    mov  rcx, [rsp + 64]      ; user RIP  → rcx (sysretq jumps here)
    mov  r11, [rsp + 56]      ; user RFLAGS → r11
    mov  rdx, [rsp + 48]      ; user RSP  → rdx (scratch)
    mov  rbp, [rsp + 40]      ; restore user callee-saved regs
    mov  rbx, [rsp + 32]
    mov  r12, [rsp + 24]
    mov  r13, [rsp + 16]
    mov  r14, [rsp + 8]
    mov  r15, [rsp + 0]
    mov  gs:[CPU_T_USER_RSP_SCRATCH], rdx ; stash final user RSP per-CPU for SYSRET path
    add  rsp, 72              ; free all scratch slots
    mov  r10, [rsp + 56]      ; restore user volatile regs preserved at entry
    mov  r9,  [rsp + 48]
    mov  r8,  [rsp + 40]
    mov  rdx, [rsp + 32]
    mov  rsi, [rsp + 24]
    mov  rdi, [rsp + 16]
    pop  rax                  ; restore return value
    add  rsp, 48              ; discard saved user rdi/rsi/rdx/r8/r9/r10

    ; 6b. Override rdi/rsi/rdx with saved_user_rdi/rsi/rdx from thread struct.
    ;     For SYS_execve these hold argc, argv pointer, and envp pointer; for all other
    ;     syscalls they are 0 (cleared after first use by syscall_get_saved_user_rdi_rsi_rdx).
    ;     IMPORTANT: only apply the override when saved_user_rsi is non-zero.
    ;     Forked children and normal syscall returns must NOT have their rdi/rsi/rdx
    ;     clobbered — step 6 already restored the correct user values from the kernel stack.
    ;
    ;     We save rdi/rsi/rdx from step 6 BEFORE calling the C function (which will clobber
    ;     rdi/rsi/rdx to pass output pointers), so we can restore them if the override is
    ;     not needed.  Layout after pushes + sub:
    ;       [rsp +  0] = out_rdi slot
    ;       [rsp +  8] = out_rsi slot
    ;       [rsp + 16] = out_rdx slot
    ;       [rsp + 24] = step-6 rdx
    ;       [rsp + 32] = step-6 rsi
    ;       [rsp + 40] = step-6 rdi
    push rdx                  ; save step-6 rdx
    push rsi                  ; save step-6 rsi
    push rdi                  ; save step-6 rdi
    sub  rsp, 24              ; allocate output slots
    mov  rdi, rsp             ; out_rdi → [rsp+0]
    lea  rsi, [rsp + 8]       ; out_rsi → [rsp+8]
    lea  rdx, [rsp + 16]      ; out_rdx → [rsp+16]
    push rax
    push rcx
    push r11
    call syscall_get_saved_user_rdi_rsi_rdx
    pop  r11
    pop  rcx
    pop  rax
    ; Apply override when either saved_user_rdi OR saved_user_rsi is non-zero.
    ; saved_user_rdi is set for signal delivery (sig number) and execve (argv ptr).
    ; saved_user_rsi is set only for execve.  Both are cleared after reading above.
    mov  rdx, [rsp]           ; out_rdi (rdx is safe to clobber — saved on stack)
    or   rdx, [rsp + 8]       ; | out_rsi
    jz   .no_override_args
    ; Apply saved_user_{rdi,rsi,rdx} — discard saved step-6 values
    mov  rdi, [rsp]
    mov  rsi, [rsp + 8]
    mov  rdx, [rsp + 16]
    add  rsp, 24 + 24         ; free slots + 3 saved step-6 regs
    jmp  .execve_args_done
.no_override_args:
    ; Restore step-6 values from saved slots
    add  rsp, 24              ; free output slots
    pop  rdi
    pop  rsi
    pop  rdx
.execve_args_done:

    ; 6c. Restore user data segments (DS/ES) to USER_DS_SEL (0x1B).
    ;     These were set to KERNEL_SS_SEL (0x10) at syscall entry.
    ;     SYSRET does not restore them.
    ;     NOTE: We are still on the kernel stack here (RSP switches at step 7),
    ;     so push/pop rax is safe.  'mov ax, imm16' would corrupt the low 16
    ;     bits of rax, destroying the syscall return value.
    push rax
    mov  ax, 0x1B
    mov  ds, ax
    mov  es, ax
    pop  rax

    ; 7. Switch to user stack (per-CPU per-thread saved RSP) and sysretq
    mov  rsp, gs:[CPU_T_USER_RSP_SCRATCH]
    o64 sysret                ; RIP=rcx, RFLAGS=r11 → user mode

; ---------------------------------------------------------------------------
; INT 0x80 entry point — installed in IDT at vector 0x80 with DPL=3
; ---------------------------------------------------------------------------
int80_entry:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov  rcx, rdx
    mov  rdx, rsi
    mov  rsi, rdi
    mov  rdi, rax
    call syscall_dispatch

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp

    iretq

; ---------------------------------------------------------------------------
; fork_child_trampoline — pure-asm implementation
;
; Replaces the C inline-asm version in syscall.c to avoid the GCC register
; clobber bug: when the C version used plain "r" constraints with only
; "memory" in the clobber list, GCC assigned r11 as the holding register for
; user_r14; the asm then wrote user_rfl into r11 before loading r14, so the
; child's r14 received RFLAGS (0x246) instead of the path pointer.
;
; This pure-asm version loads every field directly from the thread struct into
; its target register with no compiler involvement, then sysretq.
;
; struct thread saved_user_* offsets (computed from sched.h):
;   saved_user_rsp = 0x0F8
;   saved_user_rip = 0x100 (→ rcx for sysretq)
;   saved_user_rfl = 0x108 (→ r11 for sysretq)
;   saved_user_rbp = 0x110
;   saved_user_rbx = 0x118
;   saved_user_r12 = 0x120
;   saved_user_r13 = 0x128
;   saved_user_r14 = 0x130
;   saved_user_r15 = 0x138
;   saved_user_rdi = 0x140 (restored before sysretq; set from parent's rdi at fork)
;
; These offsets are guarded by _Static_assert in syscall.c.
; If struct thread changes, update both this file and the assert.
; ---------------------------------------------------------------------------
global fork_child_trampoline
fork_child_trampoline:
    ; Deliver any pending signals before entering user space.
    ; Still on the child's kernel stack (set up by sched_fork/sched_schedule).
    ; Mirrors the signal_dispatch call in the normal syscall return path.
    ; If SIGKILL/SIGTERM is pending, signal_dispatch calls sched_exit_current
    ; and does not return. If a handler is installed, signal_dispatch adjusts
    ; saved_user_rip to the handler address (picked up by rcx load below).
    call signal_dispatch

    ; Get current thread pointer into rax.
    ; sched_current() is callee-save-safe (preserves rbx/rbp/r12-r15).
    call sched_current              ; rax = struct thread *t

    ; Restore user FS.base (TLS pointer) from ctx.fs_base (thread+0xA8).
    ; ctx is at thread+8, ctx.fs_base is at ctx+160, so thread+168=0xA8.
    ; Use rcx as scratch — it will be overwritten by saved_user_rip below.
    mov  rcx, [rax + 0xA8]         ; ctx.fs_base = parent's TLS pointer
    wrfsbase rcx                   ; set FS.base for the fork child

    ; Load user callee-saved registers directly from thread struct.
    ; Order matters: load into r11 (for rfl) BEFORE any instruction overwrites rax.
    ; Load everything into its final register before we touch rsp/rcx/r11.
    mov  rbp, [rax + 0x110]        ; saved_user_rbp
    mov  rbx, [rax + 0x118]        ; saved_user_rbx
    mov  r12, [rax + 0x120]        ; saved_user_r12
    mov  r13, [rax + 0x128]        ; saved_user_r13
    mov  r14, [rax + 0x130]        ; saved_user_r14
    mov  r15, [rax + 0x138]        ; saved_user_r15
    ; Now load the sysretq-critical registers.
    ; rcx and r11 are NOT callee-saved (ABI caller-saved), so they still hold
    ; whatever sched_current left there — safe to overwrite.
    mov  rcx, [rax + 0x100]        ; saved_user_rip  → sysretq jumps here
    mov  r11, [rax + 0x108]        ; saved_user_rfl  → sysretq restores RFLAGS

    ; Switch to user stack before DS/ES restore — rax (thread ptr) must still be
    ; valid here; mov ax, 0x1B below clobbers the low 16 bits of rax.
    mov  rsp, [rax + 0x0F8]        ; saved_user_rsp
    mov  rdi, [rax + 0x140]        ; saved_user_rdi — restore caller's rdi before rax clobber

    ; Restore user data segments (DS/ES) to USER_DS_SEL (0x1B).
    ; rax (thread ptr) is no longer needed; clobber is safe.
    mov  ax, 0x1B
    mov  ds, ax
    mov  es, ax
    ; Zero caller-saved regs for a clean child environment.
    xor  eax, eax                  ; fork() returns 0 to child
    xor  edx, edx
    xor  esi, esi
    xor  r8d,  r8d
    xor  r9d,  r9d
    xor  r10d, r10d
    o64 sysret                      ; RIP=rcx, RFLAGS=r11 → ring 3
