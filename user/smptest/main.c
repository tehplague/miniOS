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

/* smptest — SMP verification binary for miniOS Phase 32 (SYS-02).
 * Forks N CPU-bound worker tasks; each reports which CPU it ran on.
 * Demonstrates parallel execution across CPUs and observable work-stealing.
 *
 * Usage: smptest [-c N]
 *   -c N  number of workers (default: 4; max: 16 to fit thread_pool)
 *
 * Expected output with -smp 4, N=4:
 *   smptest: starting 4 workers
 *   Worker PID=2 CPU=0 starting
 *   Worker PID=3 CPU=1 starting
 *   Worker PID=4 CPU=2 starting
 *   Worker PID=5 CPU=3 starting
 *   ... (migration events if any)
 *   Worker PID=2 CPU=0 exiting
 *   smptest: all workers exited
 *   PASS: smptest completed
 */

#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

static int sched_getcpu(void) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "0"(318L) : "rcx", "r11", "memory");
    return (int)ret;
}

/* write_str / write_int: safe output without printf (avoids buffering issues
 * in forked children before _exit). Writes directly to fd 1 (stdout). */
static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

static void write_int(int v) {
    if (v == 0) { write_str("0"); return; }
    if (v < 0)  { write_str("-"); v = -v; }
    char buf[16];
    int i = 15;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    write_str(buf + i);
}

static int parse_worker_count(int argc, char **argv) {
    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'c') {
        int n = 0;
        const char *p = argv[2];
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (n > 0 && n <= 16) return n;
    }
    return 4;  /* default: 4 workers */
}

int main(int argc, char **argv) {
    int num_workers = parse_worker_count(argc, argv);

    write_str("smptest: starting ");
    write_int(num_workers);
    write_str(" workers\n");

    for (int i = 0; i < num_workers; i++) {
        pid_t child = fork();
        if (child < 0) {
            write_str("FAIL: fork returned negative\n");
            return 1;
        }
        if (child == 0) {
            /* Worker child: report startup CPU, run tight loop, report exit CPU */
            int cpu = sched_getcpu();
            pid_t my_pid = getpid();

            write_str("Worker PID=");
            write_int((int)my_pid);
            write_str(" CPU=");
            write_int(cpu);
            write_str(" starting\n");

            /* CPU-bound loop: 1 billion iterations.
             * volatile prevents compiler from optimizing the loop away (-O2).
             * Periodic sched_getcpu() checks detect task migration. */
            volatile uint64_t count = 0;
            for (count = 0; count < 1000000000ULL; count++) {
                if ((count % 100000000ULL) == 0 && count > 0) {
                    int cpu_now = sched_getcpu();
                    if (cpu_now != cpu) {
                        write_str("Worker PID=");
                        write_int((int)my_pid);
                        write_str(" migrated to CPU=");
                        write_int(cpu_now);
                        write_str("\n");
                        cpu = cpu_now;
                    }
                }
            }

            cpu = sched_getcpu();
            write_str("Worker PID=");
            write_int((int)my_pid);
            write_str(" CPU=");
            write_int(cpu);
            write_str(" exiting\n");

            _exit(0);
        }
        /* Parent: continue forking */
    }

    /* Parent: wait for all N workers */
    int workers_exited = 0;
    for (int i = 0; i < num_workers; i++) {
        int status;
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped > 0) workers_exited++;
    }

    write_str("smptest: all workers exited\n");
    if (workers_exited == num_workers) {
        write_str("PASS: smptest completed\n");
        return 0;
    }
    write_str("FAIL: not all workers exited\n");
    return 1;
}
