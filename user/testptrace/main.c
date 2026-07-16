// MIT License
// Copyright (c) 2026 Christian Spoo

/*
 * testptrace — ptrace(2) primitives (PTRACE_TRACEME only, no PTRACE_ATTACH)
 *
 * Tests:
 *   ptrace_exec_stop:      child does TRACEME + execve; parent's first
 *                          wait4() catches the post-exec SIGTRAP stop.
 *   ptrace_peekdata:       parent PEEKDATAs at the child's own entry point
 *                          (via GETREGS' rip) — must not fault.
 *   ptrace_singlestep:     one PTRACE_SINGLESTEP produces exactly one more
 *                          SIGTRAP stop before the child makes any progress.
 *   ptrace_syscall_trace:  PTRACE_SYSCALL to completion counts a plausible
 *                          number of syscall-stops, ending in a real exit.
 */

#include <sys/wait.h>
#include <sys/ptrace.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static void write_str(const char *s) { write(1, s, strlen(s)); }

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        write_str("FAIL: ptrace: fork() failed\n");
        return 1;
    }

    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
            write_str("FAIL: ptrace: child TRACEME failed\n");
            _exit(1);
        }
        execve("/test/bin/hello", (char *[]){ "hello", 0 }, (char *[]){ 0 });
        write_str("FAIL: ptrace: child execve failed\n");
        _exit(1);
    }

    /* --- exec stop --- */
    int status = 0;
    pid_t r = wait4(child, &status, 0, 0);
    if (r != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        write_str("FAIL: ptrace: no exec stop reported\n");
        return 1;
    }
    write_str("PASS: ptrace_exec_stop\n");

    /* --- peekdata at the child's own entry point --- */
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, child, 0, &regs) != 0) {
        write_str("FAIL: ptrace: GETREGS failed\n");
        return 1;
    }
    long word = 0;
    if (ptrace(PTRACE_PEEKDATA, child, (void *)(unsigned long)regs.rip, &word) != 0) {
        write_str("FAIL: ptrace: PEEKDATA at child rip failed\n");
        return 1;
    }
    write_str("PASS: ptrace_peekdata\n");

    /* --- single-step: exactly one more SIGTRAP stop --- */
    if (ptrace(PTRACE_SINGLESTEP, child, 0, 0) != 0) {
        write_str("FAIL: ptrace: SINGLESTEP request failed\n");
        return 1;
    }
    status = 0;
    r = wait4(child, &status, 0, 0);
    if (r != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        write_str("FAIL: ptrace: singlestep did not produce a SIGTRAP stop\n");
        return 1;
    }
    write_str("PASS: ptrace_singlestep\n");

    /* --- PTRACE_SYSCALL to completion --- */
    int syscall_stops = 0;
    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, 0, 0) != 0) {
            write_str("FAIL: ptrace: PTRACE_SYSCALL request failed\n");
            return 1;
        }
        status = 0;
        r = wait4(child, &status, 0, 0);
        if (r != child) {
            write_str("FAIL: ptrace: wait4 lost the child\n");
            return 1;
        }
        if (WIFEXITED(status))
            break;
        if (!WIFSTOPPED(status)) {
            write_str("FAIL: ptrace: unexpected wait4 status\n");
            return 1;
        }
        syscall_stops++;
        if (syscall_stops > 10000) {
            write_str("FAIL: ptrace: syscall-stop loop did not terminate\n");
            return 1;
        }
    }

    if (syscall_stops < 2) {
        write_str("FAIL: ptrace: too few syscall stops observed\n");
        return 1;
    }
    write_str("PASS: ptrace_syscall_trace\n");

    return 0;
}
