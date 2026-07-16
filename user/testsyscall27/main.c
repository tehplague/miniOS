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
 * testsyscall27 — integration tests for all five Phase 27 syscalls:
 *   Test 1: fcntl F_GETFL on a pipe fd returns flags
 *   Test 2: fcntl F_SETFL O_NONBLOCK persists (verified via F_GETFL)
 *   Test 3: poll POLLIN on a readable pipe returns 1
 *   Test 4: lstat("/") returns 0 (no symlink dereference needed)
 *   Test 5: readlink("/") returns -1 with errno=EINVAL (not a symlink)
 *
 * Exits 0 if all tests pass, 1 if any test fails.
 */

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

/* struct pollfd / POLLIN — poll.h is not in the Newlib sysroot; define inline */
struct pollfd {
    int   fd;
    short events;
    short revents;
};
#define POLLIN  0x0001

/* Forward declarations for miniOS syscall wrappers (not in Newlib sysroot headers) */
int      poll(struct pollfd *fds, unsigned int nfds, int timeout);
int      lstat(const char *path, struct stat *st);
ssize_t  readlink(const char *path, char *buf, size_t bufsiz);

/* Write a NUL-terminated string to stdout without stdio.h */
static void puts_raw(const char *s) {
    int len = 0;
    while (s[len]) len++;
    write(1, s, (size_t)len);
}

int main(void) {
    int pass = 0;
    int fail = 0;

    /* --- Test 1: fcntl F_GETFL on a new pipe fd --- */
    {
        int fds[2];
        if (pipe(fds) == 0) {
            int flags = fcntl(fds[0], F_GETFL);
            if (flags >= 0) {
                puts_raw("[PASS] fcntl F_GETFL returns flags\n");
                pass++;
            } else {
                puts_raw("[FAIL] fcntl F_GETFL returned error\n");
                fail++;
            }
            close(fds[0]);
            close(fds[1]);
        } else {
            puts_raw("[FAIL] pipe() failed in fcntl test\n");
            fail++;
        }
    }

    /* --- Test 2: fcntl F_SETFL O_NONBLOCK --- */
    {
        int fds[2];
        if (pipe(fds) == 0) {
            int r = fcntl(fds[0], F_SETFL, O_NONBLOCK);
            if (r == 0) {
                int flags = fcntl(fds[0], F_GETFL);
                if (flags & O_NONBLOCK) {
                    puts_raw("[PASS] fcntl F_SETFL O_NONBLOCK persists\n");
                    pass++;
                } else {
                    puts_raw("[FAIL] fcntl F_SETFL O_NONBLOCK not reflected in F_GETFL\n");
                    fail++;
                }
            } else {
                puts_raw("[FAIL] fcntl F_SETFL returned error\n");
                fail++;
            }
            close(fds[0]);
            close(fds[1]);
        } else {
            puts_raw("[FAIL] pipe() failed in fcntl F_SETFL test\n");
            fail++;
        }
    }

    /* --- Test 3: poll on readable pipe --- */
    {
        int fds[2];
        if (pipe(fds) == 0) {
            const char byte = 'Z';
            write(fds[1], &byte, 1);

            struct pollfd pfd;
            pfd.fd      = fds[0];
            pfd.events  = POLLIN;
            pfd.revents = 0;

            int r = poll(&pfd, 1, 0);
            if (r == 1 && (pfd.revents & POLLIN)) {
                puts_raw("[PASS] poll POLLIN on readable pipe\n");
                pass++;
            } else {
                puts_raw("[FAIL] poll did not detect readable pipe\n");
                fail++;
            }
            close(fds[0]);
            close(fds[1]);
        } else {
            puts_raw("[FAIL] pipe() failed in poll test\n");
            fail++;
        }
    }

    /* --- Test 4: lstat on "/" returns 0 --- */
    {
        struct stat st;
        int r = lstat("/", &st);
        if (r == 0) {
            puts_raw("[PASS] lstat(\"/\") returned 0\n");
            pass++;
        } else {
            puts_raw("[FAIL] lstat(\"/\") returned error\n");
            fail++;
        }
    }

    /* --- Test 5: readlink on "/" returns EINVAL --- */
    {
        char buf[64];
        ssize_t r = readlink("/", buf, sizeof(buf));
        if (r == -1 && errno == EINVAL) {
            puts_raw("[PASS] readlink on non-symlink returns EINVAL\n");
            pass++;
        } else {
            puts_raw("[FAIL] readlink did not return EINVAL for non-symlink\n");
            fail++;
        }
    }

    /* --- Summary --- */
    if (fail == 0) {
        puts_raw("[testsyscall27] all tests passed\n");
        return 0;
    } else {
        puts_raw("[testsyscall27] SOME TESTS FAILED\n");
        return 1;
    }
}
