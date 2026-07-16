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
 * @file user/wget/main.c
 * @brief Minimal HTTP/1.0 GET client that prints only the response body.
 */

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WGET_REQ_BUF 512
#define WGET_IO_BUF 512

typedef struct {
    char host[32];
    char path[256];
    int port;
    in_addr_t addr;
} parsed_url_t;

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s http://<ipv4>[:port]/path\n", prog);
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

static int parse_url(const char *url, parsed_url_t *out)
{
    const char *rest;
    const char *slash;
    const char *colon;
    size_t host_len;
    size_t path_len;

    if (strncmp(url, "http://", 7) != 0) {
        return -1;
    }

    rest = url + 7;
    slash = NULL;
    for (const char *p = rest; *p; p++) {
        if (*p == '/') {
            slash = p;
            break;
        }
    }
    if (!slash) {
        return -1;
    }

    colon = NULL;
    for (const char *p = rest; p < slash; p++) {
        if (*p == ':') {
            colon = p;
            break;
        }
    }

    host_len = (size_t)((colon ? colon : slash) - rest);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return -1;
    }

    memcpy(out->host, rest, host_len);
    out->host[host_len] = '\0';
    if (parse_ipv4(out->host, &out->addr) != 0) {
        return -1;
    }

    out->port = 80;
    if (colon) {
        char port_buf[8];
        size_t port_len = (size_t)(slash - (colon + 1));
        int port;

        if (port_len == 0 || port_len >= sizeof(port_buf)) {
            return -1;
        }
        memcpy(port_buf, colon + 1, port_len);
        port_buf[port_len] = '\0';

        port = parse_port(port_buf);
        if (port < 0) {
            return -1;
        }
        out->port = port;
    }

    path_len = strlen(slash);
    if (path_len == 0 || path_len >= sizeof(out->path)) {
        return -1;
    }
    memcpy(out->path, slash, path_len + 1);
    return 0;
}

int main(int argc, char *argv[])
{
    parsed_url_t url;
    struct sockaddr_in dest;
    char req[WGET_REQ_BUF];
    char buf[WGET_IO_BUF];
    char header_buf[WGET_IO_BUF * 2];
    size_t header_len = 0;
    int header_done = 0;
    int sockfd;

    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (parse_url(argv[1], &url) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = net_htons((unsigned short)url.port);
    dest.sin_addr.s_addr = url.addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("wget: socket");
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("wget: connect");
        close(sockfd);
        return 1;
    }

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n",
             url.path, url.host);
    if (write_all(sockfd, req, strlen(req)) < 0) {
        perror("wget: write");
        close(sockfd);
        return 1;
    }

    for (;;) {
        ssize_t n = read(sockfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("wget: read");
            close(sockfd);
            return 1;
        }
        if (n == 0) {
            break;
        }

        if (!header_done) {
            size_t copy_len = (size_t)n;
            if (header_len + copy_len > sizeof(header_buf)) {
                fprintf(stderr, "wget: response headers too large\n");
                close(sockfd);
                return 1;
            }
            memcpy(header_buf + header_len, buf, copy_len);
            header_len += copy_len;

            for (size_t i = 0; i + 3 < header_len; i++) {
                if (header_buf[i] == '\r' && header_buf[i + 1] == '\n' &&
                    header_buf[i + 2] == '\r' && header_buf[i + 3] == '\n') {
                    size_t body_off = i + 4;
                    header_done = 1;
                    if (body_off < header_len &&
                        write_all(STDOUT_FILENO, header_buf + body_off,
                                  header_len - body_off) < 0) {
                        perror("wget: write stdout");
                        close(sockfd);
                        return 1;
                    }
                    break;
                }
            }
        } else if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
            perror("wget: write stdout");
            close(sockfd);
            return 1;
        }
    }

    if (!header_done) {
        fprintf(stderr, "wget: malformed HTTP response\n");
        close(sockfd);
        return 1;
    }

    close(sockfd);
    return 0;
}
