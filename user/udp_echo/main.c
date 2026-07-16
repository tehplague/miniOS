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
 * @file user/udp_echo/main.c
 * @brief UDP echo smoke test — sends a datagram to QEMU's built-in echo service
 *        (10.0.2.2:7) and prints the round-trip result.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

/* Minimal inline helpers — Newlib bare-metal lacks arpa/inet.h implementations */
static inline unsigned short net_htons(unsigned short v)
{
    return (unsigned short)((v >> 8) | (v << 8));
}

/* inet_addr("10.0.2.2") = 0x0202000a in network byte order */
#define QEMU_GW_ADDR 0x0202000aU  /* 10.0.2.2 in network byte order */

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { puts("udp_echo: socket failed"); return 1; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = net_htons(10007);
    dest.sin_addr.s_addr = QEMU_GW_ADDR;

    const char msg[] = "miniOS UDP echo test";
    ssize_t sent = sendto(fd, msg, sizeof(msg) - 1, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) { puts("udp_echo: sendto failed"); close(fd); return 1; }
    printf("udp_echo: sent %zd bytes\n", sent);

    char buf[64];
    ssize_t rcvd = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (rcvd < 0) { puts("udp_echo: recvfrom failed (timeout?)"); close(fd); return 1; }
    buf[rcvd] = '\0';
    printf("udp_echo: recv %zd bytes: %s\n", rcvd, buf);

    close(fd);
    puts("udp_echo: PASS");
    return 0;
}
