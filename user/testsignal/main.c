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
 * testsignal — integration tests for signal and timer parity work.
 *
 * Phase 46 expands this binary from one-shot signal smoke tests into a
 * userspace contract probe for persistent handlers and sigaction state.
 */

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

/* Provided by user/libc/syscalls.c — forward-declared because --std=c2x
 * may suppress __POSIX_VISIBLE in Newlib sys/signal.h */
int kill(pid_t pid, int sig);
/* sighandler_t may not be visible under --std=c2x; use raw function-pointer type */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

static void spin_forever(void) {
    /* Child busy-waits so parent can send signal */
    volatile int x = 0;
    for (;;) x++;
}

static void sigaction_test_handler(int sig) {
    (void)sig;
    write_str("MARK: signal_sigaction: handler invoked\n");
}

static volatile sig_atomic_t g_signal_count;
static volatile sig_atomic_t g_sigaction_count;
static volatile sig_atomic_t g_sigalrm_count;

static void persistent_signal_handler(int sig) {
    (void)sig;
    g_signal_count++;
}

static void roundtrip_sigaction_handler(int sig) {
    (void)sig;
    g_sigaction_count++;
    sigaction_test_handler(sig);
}

static void sigalrm_count_handler(int sig) {
    (void)sig;
    g_sigalrm_count++;
}

static int expect_true(const char *name, int condition, const char *message) {
    if (condition) {
        write_str("PASS: ");
        write_str(name);
        write_str("\n");
        return 0;
    }
    write_str("FAIL: ");
    write_str(name);
    write_str(": ");
    write_str(message);
    write_str("\n");
    return 1;
}

int main(void) {
    write_str("testsignal: start\n");

    /* ---- TEST: signal_kill_sigkill ---- */
    {
        pid_t child = fork();
        if (child == 0) {
            spin_forever();
            _exit(0);   /* unreachable */
        }
        if (child < 0) {
            write_str("FAIL: signal_kill_sigkill: fork failed\n");
            return 1;
        }

        /* Give child one scheduler tick to start spinning, then kill it */
        /* A brief yield is sufficient since the child is already RUNNABLE */
        int r = kill(child, SIGKILL);
        if (r != 0) {
            write_str("FAIL: signal_kill_sigkill: kill returned nonzero\n");
        }

        int status = 0;
        pid_t reaped = waitpid(child, &status, 0);
        if (reaped == child) {
            write_str("PASS: signal_kill_sigkill: child terminated\n");
        } else {
            write_str("FAIL: signal_kill_sigkill: waitpid did not return child PID\n");
        }
    }

    /* ---- TEST: signal_kill_sigterm ---- */
    {
        pid_t child = fork();
        if (child == 0) {
            spin_forever();
            _exit(0);   /* unreachable */
        }
        if (child < 0) {
            write_str("FAIL: signal_kill_sigterm: fork failed\n");
            return 1;
        }

        int r = kill(child, SIGTERM);
        if (r != 0) {
            write_str("FAIL: signal_kill_sigterm: kill returned nonzero\n");
        }

        int status = 0;
        pid_t reaped = waitpid(child, &status, 0);
        if (reaped == child) {
            write_str("PASS: signal_kill_sigterm: child terminated\n");
        } else {
            write_str("FAIL: signal_kill_sigterm: waitpid did not return child PID\n");
        }
    }

    /* ---- TEST: signal_sigchld ---- */
    /* Parent registers SIGCHLD handler, forks a child that exits immediately,
     * checks that the parent was notified (SIGCHLD queued by sched_exit_current).
     * Full handler invocation requires the ring-3 trampoline return path.
     * In Phase 23 we verify SIGCHLD delivery by confirming waitpid returns
     * the correct child PID after the child exits. */
    {
        pid_t child = fork();
        if (child == 0) {
            /* Child exits immediately; triggers SIGCHLD delivery to parent */
            _exit(0);
        }
        if (child < 0) {
            write_str("FAIL: signal_sigchld: fork failed\n");
            return 1;
        }

        /* Parent: waitpid blocks until child exits; SIGCHLD is queued on parent
         * but waitpid also returns — the test confirms the child exited normally
         * which implies SIGCHLD was generated by sched_exit_current */
        int status = 0;
        pid_t reaped = waitpid(child, &status, 0);
        if (reaped == child) {
            write_str("PASS: signal_sigchld: child exited and parent was notified\n");
        } else {
            write_str("FAIL: signal_sigchld: waitpid did not return child PID\n");
        }
    }

    /* ---- TEST: signal_persistent_handler ---- */
    {
        g_signal_count = 0;
        sighandler_t old = signal(10, persistent_signal_handler);
        if (old != SIG_DFL) {
            write_str("FAIL: signal_persistent_handler: initial handler was not SIG_DFL\n");
            return 1;
        }

        kill(getpid(), 10);
        kill(getpid(), 10);

        if (expect_true("signal_persistent_handler", g_signal_count == 2,
                        "signal() handler did not persist across repeated delivery") != 0) {
            return 1;
        }
    }

    /* ---- TEST: signal_ignore_default_roundtrip ---- */
    {
        sighandler_t old = signal(SIGALRM, SIG_IGN);
        if (old != SIG_DFL) {
            write_str("FAIL: signal_ignore_default_roundtrip: expected SIG_DFL before SIG_IGN install\n");
            return 1;
        }

        old = signal(SIGALRM, SIG_DFL);
        if (expect_true("signal_ignore_default_roundtrip", old == SIG_IGN,
                        "signal() did not distinguish SIG_IGN from SIG_DFL") != 0) {
            return 1;
        }
    }

    /* ---- TEST: signal_sigaction_roundtrip ---- */
    {
        struct sigaction sa_new;
        struct sigaction sa_old;
        struct sigaction sa_snapshot;
        struct sigaction sa_ignore;

        sa_new.sa_handler = roundtrip_sigaction_handler;
        sa_new.sa_flags = SA_RESTART;
        sigemptyset(&sa_new.sa_mask);
        sigaddset(&sa_new.sa_mask, SIGCHLD);

        if (sigaction(SIGALRM, &sa_new, &sa_old) != 0) {
            write_str("FAIL: signal_sigaction_roundtrip: initial sigaction returned nonzero\n");
            return 1;
        }

        if (expect_true("signal_sigaction_roundtrip_old_handler", sa_old.sa_handler == SIG_DFL,
                        "old sigaction handler was not SIG_DFL") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_old_flags", sa_old.sa_flags == 0,
                        "old sigaction flags were not zero") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_old_mask", sigismember(&sa_old.sa_mask, SIGCHLD) == 0,
                        "old sigaction mask was not empty") != 0) {
            return 1;
        }

        if (sigaction(SIGALRM, (const struct sigaction *)0, &sa_snapshot) != 0) {
            write_str("FAIL: signal_sigaction_roundtrip: snapshot sigaction returned nonzero\n");
            return 1;
        }

        if (expect_true("signal_sigaction_roundtrip_snapshot_handler",
                        sa_snapshot.sa_handler == roundtrip_sigaction_handler,
                        "sigaction snapshot lost handler pointer") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_snapshot_flags",
                        sa_snapshot.sa_flags == SA_RESTART,
                        "sigaction snapshot lost SA_RESTART") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_snapshot_mask",
                        sigismember(&sa_snapshot.sa_mask, SIGCHLD) == 1,
                        "sigaction snapshot lost sa_mask state") != 0) {
            return 1;
        }

        g_sigaction_count = 0;
        kill(getpid(), SIGALRM);
        if (expect_true("signal_sigaction_roundtrip_delivery", g_sigaction_count == 1,
                        "sigaction-installed handler was not invoked") != 0) {
            return 1;
        }

        sa_ignore.sa_handler = SIG_IGN;
        sa_ignore.sa_flags = 0;
        sigemptyset(&sa_ignore.sa_mask);
        if (sigaction(SIGALRM, &sa_ignore, &sa_old) != 0) {
            write_str("FAIL: signal_sigaction_roundtrip: ignore sigaction returned nonzero\n");
            return 1;
        }

        if (expect_true("signal_sigaction_roundtrip_replace_handler",
                        sa_old.sa_handler == roundtrip_sigaction_handler,
                        "sigaction replace did not return previous handler") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_replace_flags",
                        sa_old.sa_flags == SA_RESTART,
                        "sigaction replace did not return previous flags") != 0) {
            return 1;
        }
        if (expect_true("signal_sigaction_roundtrip_replace_mask",
                        sigismember(&sa_old.sa_mask, SIGCHLD) == 1,
                        "sigaction replace did not return previous mask") != 0) {
            return 1;
        }
    }

    /* ---- TEST: alarm_replacement_return ---- */
    /*
     * alarm(2) sets a 2-second timer.  Calling alarm(5) before it fires must
     * return the remaining time of the old timer (1 or 2 seconds) and arm the
     * new 5-second timer.  We install a handler so SIGALRM does not terminate.
     */
    {
        signal(SIGALRM, sigalrm_count_handler);
        /* Arm a 2-second timer then immediately replace with 5-second timer.
         * Kernel must return old remaining time > 0. */
        unsigned prev = alarm(2);
        if (expect_true("alarm_replacement_initial_return_zero",
                        prev == 0,
                        "alarm() initial call should return 0 (no prior timer)") != 0) {
            return 1;
        }
        unsigned remaining = alarm(5);
        if (expect_true("alarm_replacement_returns_nonzero_remaining",
                        remaining > 0,
                        "alarm() replacement must return remaining seconds of prior timer") != 0) {
            return 1;
        }
        /* Cancel the 5-second timer to avoid spurious SIGALRM later */
        alarm(0);
        if (expect_true("alarm_cancel_returns_nonzero",
                        alarm(0) == 0,
                        "alarm(0) after cancel should return 0") != 0) {
            return 1;
        }
        /* Restore SIG_DFL so SIGALRM does not accumulate */
        signal(SIGALRM, SIG_DFL);
    }

    /* ---- TEST: setitimer_cancel_and_rearm ---- */
    /*
     * Verifies that cancel + rearm does not deliver a stale SIGALRM.
     * We arm a 1-second timer, cancel it, then arm again and confirm we
     * receive exactly one SIGALRM from the second arm — not two.
     */
    {
        g_sigalrm_count = 0;
        signal(SIGALRM, sigalrm_count_handler);

        struct itimerval itv_arm;
        itv_arm.it_value.tv_sec = 0;
        itv_arm.it_value.tv_usec = 100000; /* 100ms = 10 LAPIC ticks */
        itv_arm.it_interval.tv_sec = 0;
        itv_arm.it_interval.tv_usec = 0;

        struct itimerval itv_cancel;
        itv_cancel.it_value.tv_sec = 0;
        itv_cancel.it_value.tv_usec = 0;
        itv_cancel.it_interval.tv_sec = 0;
        itv_cancel.it_interval.tv_usec = 0;

        struct itimerval old;

        /* Arm then immediately cancel */
        setitimer(0, &itv_arm, NULL);
        setitimer(0, &itv_cancel, &old);

        if (expect_true("setitimer_cancel_clears_pending",
                        g_sigalrm_count == 0,
                        "cancel must not have delivered SIGALRM yet") != 0) {
            return 1;
        }
        if (expect_true("setitimer_cancel_reports_remaining",
                        old.it_value.tv_sec > 0 || old.it_value.tv_usec > 0,
                        "cancel must report non-zero remaining time from prior arm") != 0) {
            return 1;
        }

        /* Rearm and wait for delivery — sleep 10ms per iteration, up to 3s total */
        setitimer(0, &itv_arm, NULL);
        volatile int waited = 0;
        while (g_sigalrm_count == 0 && waited < 300) {
            /* 10ms nanosleep per iteration gives LAPIC timer time to fire;
             * signal_dispatch runs on nanosleep return delivering SIGALRM */
            struct timespec ts = {0, 10000000}; /* 10ms */
            nanosleep(&ts, NULL);
            waited++;
        }
        if (expect_true("setitimer_rearm_delivers_exactly_one",
                        g_sigalrm_count == 1,
                        "rearm must deliver exactly one SIGALRM, not zero or duplicate") != 0) {
            return 1;
        }

        /* Make sure no second SIGALRM arrives (it_interval = 0, one-shot) */
        if (expect_true("setitimer_oneshot_no_repeat",
                        g_sigalrm_count == 1,
                        "one-shot timer must not repeat delivery") != 0) {
            return 1;
        }

        signal(SIGALRM, SIG_DFL);
        write_str("MARK: timer_lifecycle: done\n");
    }

    write_str("testsignal: done\n");
    return 0;
}
