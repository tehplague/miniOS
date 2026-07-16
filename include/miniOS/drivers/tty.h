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

/**
 * @file tty.h
 * @defgroup tty TTY Line Discipline
 * @brief Kernel TTY: line discipline, canonical/raw input, termios, and ioctl.
 *
 * The TTY layer sits between the keyboard driver and userspace. It implements
 * a POSIX-like line discipline: canonical buffering with echo, SIGINT on ^C
 * (ISIG), and raw mode (ICANON cleared). Exposes the /dev/tty device via
 * tty_read()/tty_write() and processes TCGETS/TCSETS/TIOCGWINSZ via tty_ioctl().
 * @{
 */

#ifndef _MINIOS_DRIVERS_TTY_H_
#define _MINIOS_DRIVERS_TTY_H_

#include <miniOS/types.h>

#define TTY_LFLAG_ISIG   0x0001u
#define TTY_LFLAG_ICANON 0x0002u
#define TTY_LFLAG_ECHO   0x0008u

#define VINTR 0
#define VEOF  1
#define TTY_NCCS 8

#define TCGETS    0x5401UL
#define TCSETS    0x5402UL
#define TCSETSW   0x5403UL
#define TCSETSF   0x5404UL
#define TIOCSCTTY 0x540EUL
#define TIOCGPGRP 0x540FUL   /* get foreground process group id */
#define TIOCSPGRP 0x5410UL   /* set foreground process group id */
#define TIOCGWINSZ 0x5413UL
#define TIOCNOTTY 0x5422UL

struct minios_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[TTY_NCCS];
};

struct minios_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/**
 * tty_init() - Initialise the TTY subsystem.
 *
 * @brief Sets up the canonical input ring buffer, initialises the default termios
 * (c_lflag = ICANON|ECHO|ISIG, VINTR=^C, VEOF=^D), and sets the window size
 * to SCREEN_ROWS x SCREEN_COLS. Must be called once during kernel boot
 * before tty_keyboard_input() or tty_read() are used.
 */
void tty_init(void);

/**
 * tty_keyboard_input() - Feed a raw character from the keyboard ISR into the TTY.
 * @param c Raw character byte from the PS/2 keyboard driver.
 *
 * @brief In canonical mode (ICANON set): appends to the line buffer; on newline or
 * VEOF flushes the complete line to the read buffer. In raw mode: writes
 * directly to the read buffer. Echoes @c to vt_write_byte() if ECHO is set.
 * Sends SIGINT to the foreground process group if ISIG is set and @c == VINTR.
 * Called from the keyboard interrupt handler (IRQ1).
 */
void tty_keyboard_input(char c);

/**
 * tty_inject_escape() - Inject a multi-byte escape sequence directly into the
 *                       TTY read buffer, bypassing echo and canonical buffering.
 * @seq: Byte sequence to inject (e.g. "\x1b[A" for cursor-up).
 * @len: Number of bytes to push.
 *
 * Use for special keys (arrow keys, function keys) whose escape sequences must
 * never be echoed to the screen and must bypass ICANON line buffering.
 */
void tty_inject_escape(const char *seq, int len);

/**
 * tty_has_data() - Return non-zero if the TTY input ring buffer has unread bytes.
 * Used by poll() to report POLLIN readiness for stdin.
 */
int tty_has_data(void);

/**
 * tty_read() - Read bytes from the TTY input buffer.
 * @param buf Destination buffer.
 * @param len Maximum number of bytes to read.
 * @param nonblock Non-zero to return immediately if no data is available (O_NONBLOCK).
 *
 * @brief In canonical mode: blocks (or returns -EAGAIN if nonblock) until a complete
 * line is available, then copies up to @len bytes. In raw mode: returns as many
 * bytes as are immediately available up to @len.
 *
 * @return Number of bytes read, 0 on EOF (VEOF character), or -1 on error.
 */
int tty_read(void *buf, uint32_t len, int nonblock);

/**
 * tty_write() - Write bytes to the TTY output (VGA terminal).
 * @param buf Source buffer of bytes to write.
 * @param len Number of bytes.
 *
 * @brief Forwards each byte to vt_write_byte(), which handles ANSI/VT escape
 * sequence parsing and VGA cell rendering.
 *
 * @return Number of bytes written (@len on success, -1 on error).
 */
int tty_write(const void *buf, uint32_t len);

/**
 * tty_ioctl() - Handle terminal ioctl commands.
 * @param cmd Ioctl command code (TCGETS=0x5401, TCSETS=0x5402, TCSETSW=0x5403,
 *            TIOCSCTTY=0x540E, TIOCGWINSZ=0x5413, TIOCNOTTY=0x5422).
 * @param arg User-space pointer to a struct minios_termios or struct minios_winsize,
 *            depending on @cmd.
 *
 * @brief TCGETS: copies current termios to *arg.
 * TCSETS/TCSETSW: copies termios from *arg into the kernel TTY state.
 * TIOCGWINSZ: copies the window size (SCREEN_ROWS x SCREEN_COLS) to *arg.
 * TIOCSCTTY/TIOCNOTTY: accepted (no-op in single-TTY kernel).
 *
 * @return 0 on success, -EINVAL for unknown @cmd.
 */
int tty_ioctl(uint64_t cmd, uint64_t arg);

/**
 * tty_isatty_fd() - Test whether a file descriptor refers to the TTY device.
 * @param fd File descriptor to test.
 *
 * @brief Returns 1 if the fd's vfs_file entry has ftype == VFS_FILE_TYPE_CHAR and
 * device_type == VFS_DEVICE_TTY, 0 otherwise. Used by the isatty(3) Newlib stub.
 *
 * @return 1 if @fd is the TTY, 0 if not.
 */
int tty_isatty_fd(int fd);

/** @} */

#endif /* _MINIOS_DRIVERS_TTY_H_ */
