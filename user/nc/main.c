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
 * @file user/nc/main.c
 * @brief Minimal TCP client that bridges stdin/stdout to a socket.
 */

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NC_BUF_SIZE 512

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <ipv4> <port>\n", prog);
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int parse_port(const char *text)
{
    char *end = NULL;
    long port = strtol(text, &end, 10);
    if (!text[0] || (end && *end != '\0') || port < 1 || port > 65535) {
        return -1;
    }
    return (int)port;
}

static unsigned short net_htons(unsigned short v)
{
    return (unsigned short)((v >> 8) | (v << 8));
}

static int parse_ipv4(const char *text, in_addr_t *addr_out)
{
    unsigned long octets[4];
    char tail;

    if (sscanf(text, "%lu.%lu.%lu.%lu%c",
               &octets[0], &octets[1], &octets[2], &octets[3], &tail) != 4) {
        return -1;
    }

    for (int i = 0; i < 4; i++) {
        if (octets[i] > 255) {
            return -1;
        }
    }

    *addr_out = ((in_addr_t)octets[0]) |
                ((in_addr_t)octets[1] << 8) |
                ((in_addr_t)octets[2] << 16) |
                ((in_addr_t)octets[3] << 24);
    return 0;
}

int main(int argc, char *argv[])
{
    int sockfd;
    int port;
    char buf[NC_BUF_SIZE];
    struct sockaddr_in dest;

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    port = parse_port(argv[2]);
    if (port < 0) {
        fprintf(stderr, "nc: invalid port '%s'\n", argv[2]);
        return 1;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = net_htons((unsigned short)port);
    if (parse_ipv4(argv[1], &dest.sin_addr.s_addr) != 0) {
        fprintf(stderr, "nc: invalid IPv4 address '%s'\n", argv[1]);
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("nc: socket");
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("nc: connect");
        close(sockfd);
        return 1;
    }

    for (;;) {
        int saw_newline = 0;
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("nc: read stdin");
            close(sockfd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        if (write_all(sockfd, buf, (size_t)n) < 0) {
            perror("nc: write socket");
            close(sockfd);
            return 1;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                saw_newline = 1;
                break;
            }
        }
        if (saw_newline) {
            break;
        }
    }

    for (;;) {
        ssize_t n = read(sockfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("nc: read socket");
            close(sockfd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
            perror("nc: write stdout");
            close(sockfd);
            return 1;
        }
    }

    close(sockfd);
    return 0;
}
