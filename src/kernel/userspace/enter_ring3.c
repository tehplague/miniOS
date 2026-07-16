// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <miniOS/userspace/enter_ring3.h>
#include <miniOS/arch/x86_64/tss.h>
#include <miniOS/arch/x86_64/segment.h>
#include <miniOS/types.h>
#include <miniOS/sched/sched.h>
#include <miniOS/io.h>
#include <string.h>

int copy_string_array_to_user(struct thread *t, char *const *arr, uint64_t *user_ptr) {
    if (!t || !arr || !user_ptr) {
        return -1;
    }

    int count = 0;
    size_t total_len = 0;
    for (char *const *p = arr; *p; ++p) {
        count++;
        total_len += strlen(*p) + 1;
    }

    size_t pointers_size = (count + 1) * sizeof(char *);
    size_t total_size = total_len + pointers_size;
    total_size = (total_size + 15) & ~15; // Align to 16 bytes

    // Allocate space on the user stack
    t->user_stack_virt_top -= total_size;
    uint64_t user_base_addr = t->user_stack_virt_top;

    char *user_str_ptr = (char *)user_base_addr;
    char **user_arr_ptr = (char **)(user_base_addr + total_len);

    *user_ptr = (uint64_t)user_arr_ptr;

    // Copy strings and set up pointers
    char **current_user_arr_entry = user_arr_ptr;
    uint64_t current_user_str_addr = (uint64_t)user_str_ptr;
    for (char *const *p = arr; *p; ++p) {
        size_t len = strlen(*p) + 1;
        memcpy((void *)current_user_str_addr, *p, len);
        *current_user_arr_entry = (char *)current_user_str_addr;
        current_user_str_addr += len;
        current_user_arr_entry++;
    }
    *current_user_arr_entry = NULL;

    return 0;
}



__attribute__((noreturn))
void enter_ring3(uint64_t entry_rip, uint64_t user_rsp) {
    /* syscall_kernel_rsp_storage is set by the caller (kernel_main) before
     * invoking enter_ring3, and updated by sched_schedule on every context
     * switch.  Do NOT overwrite it here with the current boot-stack RSP — that
     * would cause the SYSCALL path to share the boot stack with the idle thread,
     * which corrupts saved register state when both threads use the same stack. */

    /* Get the current thread's context to load r15 (envp pointer) */
    struct thread *current_thread = sched_current();
    uint64_t envp_ptr = current_thread->ctx.r15;

    /* Build the iretq frame and execute iretq.
     *
     * We do this in two separate asm blocks to work around the register
     * allocator exhaustion caused by zeroing all GPRs in the same block as
     * loading the input operands.
     *
     * Block 1: push the iretq frame (SS, user_RSP, RFLAGS|IF, CS, RIP).
     *   Inputs entry_rip (rdi) and user_rsp (rsi) are still live here.
     *   USER_CS/USER_SS are small immediates — use "i" constraint (they are
     *   compile-time integer constants so GCC accepts "i" even with
     *   -mcmodel=large).
     */
    __asm__ volatile(
        /* iretq frame (high → low): SS, RSP, RFLAGS|IF, CS, RIP */
        "push %[ss]\n\t"           /* SS = USER_DS_SEL (0x1B) */
        "push %[user_rsp]\n\t"     /* user RSP */
        "mov  $0x202, %%rax\n\t"   /* RFLAGS: IF=1, bit1=1, TF=0 — explicit clean state */
        "push %%rax\n\t"           /* RFLAGS */
        "push %[cs]\n\t"           /* CS = USER_CS_SEL (0x23) */
        "push %[rip]\n\t"          /* RIP = entry_rip */
        :                           /* no outputs */
        : [rip]     "r" (entry_rip),
          [user_rsp]"r" (user_rsp),
          [cs]      "i" ((uint64_t)USER_CS_SEL),
          [ss]      "i" ((uint64_t)USER_DS_SEL)
        : "rax", "memory"
    );

    /* Block 2: zero all GPRs that user code should not inherit, then iretq.
     * The iretq frame is already on the kernel stack.
     * Load r15 with the envp pointer from the thread's context. */
    __asm__ volatile(
        "xor %%rax, %%rax\n\t"
        "xor %%rbx, %%rbx\n\t"
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "xor %%rsi, %%rsi\n\t"
        "xor %%rdi, %%rdi\n\t"
        "xor %%r8,  %%r8\n\t"
        "xor %%r9,  %%r9\n\t"
        "xor %%r10, %%r10\n\t"
        "xor %%r11, %%r11\n\t"
        "xor %%r12, %%r12\n\t"
        "xor %%r13, %%r13\n\t"
        "xor %%r14, %%r14\n\t"
        "mov  %[envp], %%r15\n\t"  /* Load envp pointer into r15 */
        "iretq\n\t"
        :                           /* no outputs */
        : [envp] "r" (envp_ptr)   /* envp pointer from thread context */
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","memory"
    );
    __builtin_unreachable();
}
