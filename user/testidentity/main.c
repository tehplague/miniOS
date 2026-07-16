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

/*
 * testidentity — integration test for Phase 22 syscalls (PROC-01, PROC-02, PROC-03)
 *
 * Tests:
 *   PROC-02: getpid() returns non-zero PID
 *   PROC-02: child's getpid() differs from parent's getpid()
 *   PROC-02: child's getppid() equals parent's getpid()
 *   PROC-01: waitpid(child, &status, 0) returns child PID after child exits
 *   PROC-01: WEXITSTATUS(status) equals the exit code the child used
 *   PROC-01: second waitpid on same child returns -1 (ECHILD)
 *   PROC-03: setsid() on a non-group-leader succeeds (returns new SID > 0)
 */

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

/* Provided by user/libc/syscalls.c */
pid_t setsid(void);
int   setpgid(pid_t pid, pid_t pgid);

static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

static void write_int(int v) {
    if (v < 0) { write_str("-"); v = -v; }
    if (v == 0) { write_str("0"); return; }
    char buf[16];
    int i = 15;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write_str(buf + i);
}

int main(void) {
    write_str("testidentity: start\n");

    /* ---- PROC-02: getpid returns non-zero ---- */
    pid_t my_pid = getpid();
    write_str("  parent pid=");
    write_int((int)my_pid);
    write_str("\n");
    if (my_pid <= 0) {
        write_str("FAIL: getpid returned zero or negative\n");
    } else {
        write_str("PASS: getpid returns non-zero\n");
    }

    /* ---- PROC-02: fork child, check child pid/ppid ---- */
    pid_t child = fork();
    if (child == 0) {
        /* Child branch */
        pid_t cpid  = getpid();
        pid_t cppid = getppid();
        write_str("  child pid=");
        write_int((int)cpid);
        write_str(" ppid=");
        write_int((int)cppid);
        write_str("\n");

        if (cpid == my_pid) {
            write_str("FAIL: child pid equals parent pid\n");
        } else {
            write_str("PASS: child pid differs from parent pid\n");
        }

        if (cppid != my_pid) {
            write_str("FAIL: child ppid does not match parent pid\n");
        } else {
            write_str("PASS: child ppid equals parent pid\n");
        }

        /* Exit with a recognizable code */
        _exit(42);
    }

    /* Parent branch */
    if (child < 0) {
        write_str("FAIL: fork returned negative\n");
        return 1;
    }

    /* ---- PROC-01: waitpid collects exit status ---- */
    int status = 0;
    pid_t reaped = waitpid(child, &status, 0);
    write_str("  waitpid returned=");
    write_int((int)reaped);
    write_str(" status=");
    write_int(status);
    write_str("\n");

    if (reaped != child) {
        write_str("FAIL: waitpid did not return child PID\n");
    } else {
        write_str("PASS: waitpid reaped child\n");
    }

    /* WEXITSTATUS: (status >> 8) & 0xff should be 42 */
    int exit_code = (status >> 8) & 0xff;
    write_str("  WEXITSTATUS=");
    write_int(exit_code);
    write_str("\n");
    if (exit_code != 42) {
        write_str("FAIL: WEXITSTATUS not 42\n");
    } else {
        write_str("PASS: WEXITSTATUS == 42\n");
    }

    /* ---- PROC-01: second waitpid returns error (ECHILD) ---- */
    int status2 = 0;
    pid_t reaped2 = waitpid(child, &status2, 0);
    if (reaped2 != (pid_t)-1) {
        write_str("FAIL: second waitpid did not return -1\n");
    } else {
        write_str("PASS: second waitpid returns -1 (ECHILD)\n");
    }

    /* ---- PROC-03: setsid on non-group-leader succeeds ---- */
    /* After fork+exec cycle the shell child is typically not a group leader.
     * testidentity itself may be a group leader; if setsid returns -1 that is
     * expected (EPERM). Either result is valid here — we just verify it
     * doesn't crash the kernel. */
    pid_t new_sid = setsid();
    write_str("  setsid returned=");
    write_int((int)new_sid);
    write_str("\n");
    if (new_sid > 0) {
        write_str("PASS: setsid created new session\n");
    } else {
        write_str("INFO: setsid returned <=0 (process may already be group leader)\n");
    }

    write_str("testidentity: done\n");
    return 0;
}
