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

/* stub_keyboard.c — fake keyboard driver for host-native unit tests.
 * Tests can set stub_keyboard_char to control what keyboard_read_char() returns. */

#include <miniOS/drivers/keyboard.h>
#include <miniOS/drivers/tty.h>
#include <string.h>

/* Global that tests can set to control the returned character */
char stub_keyboard_char = 'x';
struct minios_termios stub_tty_termios = {
    .c_lflag = TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | TTY_LFLAG_ISIG,
    .c_cc = { 0x03, 0x04 },
};
char stub_tty_read_char = 'x';
char stub_tty_write_buf[32];
uint32_t stub_tty_write_len = 0;
uint32_t stub_tty_read_calls = 0;
uint32_t stub_tty_write_calls = 0;

char keyboard_read_char(void) {
    return stub_keyboard_char;
}

void keyboard_init(void) {
    /* no-op for test builds */
}

void tty_init(void) {
    /* no-op for test builds */
}

void tty_keyboard_input(char c) {
    stub_keyboard_char = c;
    stub_tty_read_char = c;
}

int tty_read(void *buf, uint32_t len, int nonblock) {
    (void)nonblock;
    if (!buf || len == 0)
        return 0;
    ((char *)buf)[0] = stub_keyboard_char;
    stub_tty_read_calls++;
    return 1;
}

int tty_write(const void *buf, uint32_t len) {
    uint32_t n = (len < sizeof(stub_tty_write_buf)) ? len : (uint32_t)sizeof(stub_tty_write_buf);
    memcpy(stub_tty_write_buf, buf, n);
    stub_tty_write_len = n;
    stub_tty_write_calls++;
    return (int)len;
}

int tty_ioctl(uint64_t cmd, uint64_t arg) {
    switch (cmd) {
        case TIOCGWINSZ: {
            struct minios_winsize *ws = (struct minios_winsize *)(uintptr_t)arg;
            if (!ws) return -22;
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
        case TCGETS: {
            struct minios_termios *termios = (struct minios_termios *)(uintptr_t)arg;
            if (!termios) return -22;
            *termios = stub_tty_termios;
            return 0;
        }
        case TCSETS:
        case TCSETSW: {
            const struct minios_termios *termios = (const struct minios_termios *)(uintptr_t)arg;
            if (!termios) return -22;
            stub_tty_termios = *termios;
            return 0;
        }
        case TIOCSCTTY:
        case TIOCNOTTY:
            return 0;
        default:
            return -25;
    }
}

int tty_isatty_fd(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

int tty_has_data(void) { return 0; }
