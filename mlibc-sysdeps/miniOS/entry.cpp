#include <stdint.h>
#include <stdlib.h>
#include <mlibc/elf/startup.h>
#include <bits/syscall.h>

extern "C" void __dlapi_enter(uintptr_t *);
extern "C" void signal_trampoline(void);
extern char **environ;

#define SYS_register_sigtrampoline 454

extern "C" void __mlibc_entry(uintptr_t *entry_stack,
                               int (*main_fn)(int, char *[], char *[])) {
	__syscall1(SYS_register_sigtrampoline, (long)signal_trampoline);
	__dlapi_enter(entry_stack);
	auto result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
	exit(result);
}
