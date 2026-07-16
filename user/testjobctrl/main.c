// MIT License
// Copyright (c) 2026 Christian Spoo

/*
 * testjobctrl — SIGSTOP/SIGCONT job-control primitives
 *
 * Tests, self-contained (no shell/job-control dependency):
 *   jobctrl_stop_reported:      parent SIGSTOPs a busy-looping child; waitpid
 *                                (WUNTRACED) reports WIFSTOPPED/WSTOPSIG==SIGSTOP
 *                                without reaping the child.
 *   jobctrl_resume_and_exit:    parent SIGCONTs the child; child resumes its
 *                                (deterministic, iteration-count-bounded) loop
 *                                and exits; a second waitpid reports the real
 *                                exit status.
 */

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static void write_str(const char *s) { write(1, s, strlen(s)); }

int main(void) {
    pid_t child = fork();
    if (child < 0) {
        write_str("FAIL: jobctrl: fork() failed\n");
        return 1;
    }

    if (child == 0) {
        write_str("child: alive\n");
        /* Deterministic (iteration-bounded, not time-bounded) work: however
         * long the parent keeps this process SIGSTOPped, this loop makes
         * exactly the same amount of progress once resumed. */
        for (volatile long i = 0; i < 200000000L; i++) { }
        write_str("child: done looping, exiting\n");
        _exit(42);
    }

    /* Give the child a moment to start its loop before stopping it. */
    for (volatile long i = 0; i < 2000000L; i++) { }

    if (kill(child, SIGSTOP) != 0) {
        write_str("FAIL: jobctrl: kill(SIGSTOP) failed\n");
        return 1;
    }

    int status = 0;
    pid_t r = waitpid(child, &status, WUNTRACED);
    if (r != child || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        write_str("FAIL: jobctrl: waitpid(WUNTRACED) did not report a stopped child\n");
        return 1;
    }
    write_str("PASS: jobctrl_stop_reported\n");

    if (kill(child, SIGCONT) != 0) {
        write_str("FAIL: jobctrl: kill(SIGCONT) failed\n");
        return 1;
    }

    status = 0;
    r = waitpid(child, &status, 0);
    if (r != child || !WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        write_str("FAIL: jobctrl: child did not exit cleanly after SIGCONT\n");
        return 1;
    }
    write_str("PASS: jobctrl_resume_and_exit\n");

    return 0;
}
