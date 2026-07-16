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

#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static int g_failures = 0;

static void write_str(const char *s) {
    size_t len = 0;

    while (s[len] != '\0') len++;
    write(1, s, len);
}

static void write_hex_byte(unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[2];

    buf[0] = hex[(value >> 4) & 0x0F];
    buf[1] = hex[value & 0x0F];
    write(1, buf, sizeof(buf));
}

static void mark_result(const char *name, int pass) {
    write_str(pass ? "PASS: " : "FAIL: ");
    write_str(name);
    write_str("\n");
    if (!pass) {
        g_failures++;
    }
}

static int run_winsize_test(void) {
    struct winsize ws;
    int ok;

    ws.ws_row = 0;
    ws.ws_col = 0;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    ok = (ioctl(1, TIOCGWINSZ, &ws) == 0 &&
          ws.ws_row == 25 &&
          ws.ws_col == 80);
    mark_result("winsize", ok);
    return ok;
}

static int run_canonical_test(void) {
    char buf[32];
    ssize_t nread;
    int ok;

    write_str("canonical: type ok then Enter\n");
    nread = read(0, buf, sizeof(buf));
    ok = (nread == 3 && buf[0] == 'o' && buf[1] == 'k' && buf[2] == '\n');
    mark_result("canonical", ok);
    return ok;
}

static int run_raw_test(void) {
    struct termios saved;
    struct termios raw;
    unsigned char ch = 0;
    ssize_t nread;
    int ok = 1;

    if (tcgetattr(0, &saved) != 0) {
        mark_result("raw", 0);
        return 0;
    }

    raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(0, TCSANOW, &raw) != 0) {
        mark_result("raw", 0);
        return 0;
    }

    write_str("raw: type Z\n");
    nread = read(0, &ch, 1);
    write_str("raw: byte=0x");
    write_hex_byte(ch);
    write_str("\n");

    if (tcsetattr(0, TCSANOW, &saved) != 0) {
        ok = 0;
    }

    ok = ok && (nread == 1) && (ch == 'Z');
    mark_result("raw", ok);
    return ok;
}

static int run_escape_test(void) {
    static const char seq[] = "\x1b[31mRED\x1b[0m \x1b[1D!\n";

    write_str("escape: ");
    write(1, seq, sizeof(seq) - 1);
    mark_result("escape", 1);
    return 1;
}

int main(void) {
    write_str("testtty: start\n");

    run_winsize_test();
    run_canonical_test();
    run_raw_test();
    run_escape_test();

    if (g_failures == 0) {
        write_str("testtty: ALL TESTS PASSED\n");
    } else {
        write_str("testtty: SOME TESTS FAILED\n");
    }

    return g_failures;
}
