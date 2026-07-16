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
 * testpipe — integration tests for Phase 24 SYS_pipe (PIPE-01) + SYS_dup/dup2 (PIPE-02)
 *
 * Tests:
 *   pipe_write_read:           pipe() creates two fds; write to write-end readable from read-end
 *   pipe_eof_on_write_close:   close(write_fd) causes read to return 0 (EOF)
 *   dup_close_independence:    dup(rfd) creates alias; close(rfd) leaves dup fd functional
 *   dup2_redirect:             dup2(pipe_rfd, 10) makes fd 10 read from the pipe
 */

#include <unistd.h>
#include <sys/types.h>

/* pipe() and dup() are provided by user/libc/syscalls.c */
int pipe(int fds[2]);
int dup(int oldfd);

static void write_str(const char *s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
}

static void test_pipe_write_read(void) {
    int fds[2];
    if (pipe(fds) != 0) { write_str("FAIL: test_pipe_write_read: pipe() failed\n"); return; }

    const char msg[] = "hello";
    write(fds[1], msg, 5);

    char buf[8] = {0};
    int n = (int)read(fds[0], buf, 8);
    if (n == 5 && buf[0]=='h' && buf[1]=='e' && buf[2]=='l' && buf[3]=='l' && buf[4]=='o') {
        write_str("PASS: test_pipe_write_read\n");
    } else {
        write_str("FAIL: test_pipe_write_read: data mismatch\n");
    }
    close(fds[0]);
    close(fds[1]);
}

static void test_pipe_eof_on_write_close(void) {
    int fds[2];
    if (pipe(fds) != 0) { write_str("FAIL: test_pipe_eof_on_write_close: pipe() failed\n"); return; }

    /* Write data, then close write end */
    write(fds[1], "x", 1);
    close(fds[1]);

    /* Read the buffered byte */
    char buf[4] = {0};
    int n1 = (int)read(fds[0], buf, 4);
    /* Then EOF */
    int n2 = (int)read(fds[0], buf, 4);

    if (n1 == 1 && n2 == 0) {
        write_str("PASS: test_pipe_eof_on_write_close\n");
    } else {
        write_str("FAIL: test_pipe_eof_on_write_close: expected 1 then 0\n");
    }
    close(fds[0]);
}

static void test_dup_close_independence(void) {
    int fds[2];
    if (pipe(fds) != 0) { write_str("FAIL: test_dup_close_independence: pipe() failed\n"); return; }

    int rfd2 = dup(fds[0]);
    if (rfd2 < 0) { write_str("FAIL: test_dup_close_independence: dup() failed\n"); return; }

    /* Write data, close original read end */
    write(fds[1], "d", 1);
    close(fds[0]);

    /* Read via dup'd fd — should still work */
    char buf[4] = {0};
    int n = (int)read(rfd2, buf, 4);
    if (n == 1 && buf[0] == 'd') {
        write_str("PASS: test_dup_close_independence\n");
    } else {
        write_str("FAIL: test_dup_close_independence: read failed after close\n");
    }
    close(rfd2);
    close(fds[1]);
}

static void test_dup2_redirect(void) {
    int fds[2];
    if (pipe(fds) != 0) { write_str("FAIL: test_dup2_redirect: pipe() failed\n"); return; }

    /* Redirect fd=10 to read end of pipe */
    int r = dup2(fds[0], 10);
    if (r != 10) { write_str("FAIL: test_dup2_redirect: dup2() returned wrong fd\n"); return; }

    write(fds[1], "r", 1);

    char buf[4] = {0};
    int n = (int)read(10, buf, 4);
    if (n == 1 && buf[0] == 'r') {
        write_str("PASS: test_dup2_redirect\n");
    } else {
        write_str("FAIL: test_dup2_redirect: read from redirected fd failed\n");
    }
    close(fds[0]);
    close(fds[1]);
    close(10);
}

int main(void) {
    write_str("=== testpipe ===\n");
    test_pipe_write_read();
    test_pipe_eof_on_write_close();
    test_dup_close_independence();
    test_dup2_redirect();
    write_str("=== testpipe done ===\n");
    return 0;
}
