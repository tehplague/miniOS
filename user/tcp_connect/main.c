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
 * @file user/tcp_connect/main.c
 * @brief TCP connect smoke test — connects to QEMU's echo service (10.0.2.2:7).
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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { puts("tcp_connect: socket failed"); return 1; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = net_htons(10007);
    dest.sin_addr.s_addr = QEMU_GW_ADDR;

    if (connect(fd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        puts("tcp_connect: connect failed"); close(fd); return 1;
    }
    puts("tcp_connect: connected");

    const char msg[] = "hello from miniOS";
    ssize_t sent = write(fd, msg, sizeof(msg) - 1);
    if (sent < 0) { puts("tcp_connect: write failed"); close(fd); return 1; }
    printf("tcp_connect: sent %zd bytes\n", sent);

    char buf[64];
    ssize_t rcvd = read(fd, buf, sizeof(buf) - 1);
    if (rcvd < 0) { puts("tcp_connect: read failed (timeout?)"); close(fd); return 1; }
    buf[rcvd] = '\0';
    printf("tcp_connect: recv %zd bytes: %s\n", rcvd, buf);

    close(fd);
    puts("tcp_connect: PASS");
    return 0;
}
