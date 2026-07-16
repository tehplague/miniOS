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
 * @file user/ifconfig/main.c
 * @brief Read-only ifconfig — prints eth0 inet address, MAC, and byte counters
 *        from sysfs. Per D-01, D-05.
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Direct unbuffered print — bypasses Newlib stdio entirely.
 * crt0.asm calls SYS_exit without fflush, so printf output is lost. */
static void rprint(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

/**
 * @brief Read a sysfs file into buf, strip trailing newline, null-terminate.
 *
 * @param path   Sysfs path to open.
 * @param buf    Destination buffer.
 * @param bufsiz Size of destination buffer (including null terminator space).
 * @return Number of bytes read (excluding null), or -1 on error.
 */
static int read_sysfs(const char *path, char *buf, int bufsiz)
{
    int fd = open(path, 0);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsiz - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    /* Strip trailing newline */
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    return (int)n;
}

int main(void)
{
    char inet[32], hwaddr[32], rxb[32], txb[32];

    if (read_sysfs("/sys/class/net/eth0/inet_addr", inet, sizeof(inet)) < 0) {
        rprint("ifconfig: cannot read inet_addr\n");
        return 1;
    }
    if (read_sysfs("/sys/class/net/eth0/address", hwaddr, sizeof(hwaddr)) < 0) {
        rprint("ifconfig: cannot read address\n");
        return 1;
    }
    if (read_sysfs("/sys/class/net/eth0/statistics/rx_bytes", rxb, sizeof(rxb)) < 0) {
        rprint("ifconfig: cannot read rx_bytes\n");
        return 1;
    }
    if (read_sysfs("/sys/class/net/eth0/statistics/tx_bytes", txb, sizeof(txb)) < 0) {
        rprint("ifconfig: cannot read tx_bytes\n");
        return 1;
    }

    rprint("eth0      inet addr:");
    rprint(inet);
    rprint("\n          HWaddr ");
    rprint(hwaddr);
    rprint("\n          RX bytes:");
    rprint(rxb);
    rprint("  TX bytes:");
    rprint(txb);
    rprint("\n");

    return 0;
}
