.section .text

.global _start
.type _start, @function
_start:
	xorq %rbp, %rbp           /* mark outermost frame */
	movq %rsp, %rdi            /* arg1: entry_stack (initial stack pointer) */
	leaq main(%rip), %rsi      /* arg2: main */
	andq $-16, %rsp            /* align stack before call */
	call __mlibc_entry
	ud2                        /* unreachable */

/* Called by the kernel as the return address from a signal handler.
   Invokes SYS_sigreturn (15) so the kernel restores saved user context. */
.global signal_trampoline
.type signal_trampoline, @function
signal_trampoline:
	movl $15, %eax
	syscall
	ud2

.section .note.GNU-stack,"",@progbits
