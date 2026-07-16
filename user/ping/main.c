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
 * @file user/ping/main.c
 * @brief ping — sends 4 ICMP echo requests via SOCK_RAW, measures RTT with
 *        clock_gettime(CLOCK_MONOTONIC), and prints per-packet timing + summary.
 *        Per D-08 through D-12.
 */

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

/* Newlib bare-metal may not define these; provide fallbacks */
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

#define PING_COUNT      4
#define ICMP_ECHO_ID    0x1234

/* Direct unbuffered print — bypasses Newlib stdio entirely.
 * crt0.asm calls SYS_exit without fflush, so printf output is lost. */
static void rprint(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

/**
 * @brief Convert a signed integer to decimal string and write via rprint.
 */
static void rprint_int(long val)
{
    char buf[24];
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    if (val == 0) buf[--i] = '0';
    while (val > 0) { buf[--i] = '0' + (int)(val % 10); val /= 10; }
    if (neg) buf[--i] = '-';
    rprint(&buf[i]);
}

/**
 * @brief Compute ones-complement Internet checksum.
 *        Verbatim copy from user/rawtest/main.c.
 */
static unsigned short icmp_checksum(const void *data, int len)
{
    const unsigned short *p = (const unsigned short *)data;
    unsigned int sum = 0;
    for (int i = 0; i < len / 2; i++)
        sum += p[i];
    if (len & 1)
        sum += ((const unsigned char *)data)[len - 1];
    sum  = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)~sum;
}

/**
 * @brief Parse "A.B.C.D" into the packed IPv4 layout used by miniOS raw sockets.
 *
 * Per T-44-02-02: rejects malformed input by returning 0 when any octet
 * exceeds 255 or fewer than 4 octets are present.
 *
 * @return Packed IPv4 uint32 compatible with rawtest/QEMU_GW_ADDR, or 0 on parse failure.
 */
static uint32_t parse_ipv4(const char *s)
{
    unsigned int octets[4];
    int part = 0;
    unsigned int cur = 0;
    int digits = 0;

    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10 + (unsigned int)(*s - '0');
            if (cur > 255) return 0; /* octet overflow */
            digits++;
        } else if (*s == '.') {
            if (digits == 0 || part >= 3) return 0; /* empty octet or too many dots */
            octets[part++] = cur;
            cur = 0;
            digits = 0;
        } else {
            return 0; /* unexpected character */
        }
    }
    if (digits == 0 || part != 3) return 0; /* incomplete address */
    octets[3] = cur;

    /* Match rawtest's QEMU_GW_ADDR layout: A | B<<8 | C<<16 | D<<24. */
    return (uint32_t)(octets[0]
                    | (octets[1] << 8)
                    | (octets[2] << 16)
                    | (octets[3] << 24));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        rprint("usage: ping <ip>\n");
        return 1;
    }

    uint32_t dest_addr = parse_ipv4(argv[1]);
    if (dest_addr == 0) {
        rprint("ping: invalid IPv4 address\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        rprint("ping: socket failed\n");
        return 1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_addr.s_addr = dest_addr;

    int sent_count    = 0;
    int received_count = 0;

    for (int seq = 1; seq <= PING_COUNT; seq++) {
        /* Build 8-byte ICMP echo request */
        unsigned char icmp[8];
        memset(icmp, 0, sizeof(icmp));
        icmp[0] = 8;                                       /* ICMP_ECHO type */
        icmp[1] = 0;                                       /* code */
        icmp[4] = 0x12;
        icmp[5] = 0x34;                                    /* id = 0x1234 (network byte order) */
        icmp[6] = (unsigned char)(seq >> 8);               /* seqno high byte */
        icmp[7] = (unsigned char)(seq & 0xff);             /* seqno low byte */

        unsigned short cksum = icmp_checksum(icmp, 8);
        icmp[2] = (unsigned char)(cksum & 0xff);
        icmp[3] = (unsigned char)(cksum >> 8);

        /* Timestamp before send — D-06 */
        struct timespec t_start;
        clock_gettime(1 /* CLOCK_MONOTONIC */, &t_start);

        ssize_t s = sendto(fd, icmp, sizeof(icmp), 0,
                           (struct sockaddr *)&dest, sizeof(dest));
        if (s < 0) {
            rprint("ping: sendto failed\n");
            sent_count++;
            if (seq < PING_COUNT) sleep(1);
            continue;
        }
        sent_count++;

        /* Receive reply — includes 20-byte IP header + ICMP */
        unsigned char buf[1500];
        ssize_t rcvd = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);

        /* Timestamp after recv — D-06 */
        struct timespec t_end;
        clock_gettime(1 /* CLOCK_MONOTONIC */, &t_end);

        if (rcvd < 28) {
            /* Too short to contain IP header (20) + ICMP (8) */
            rprint("ping: short or no reply for icmp_seq=");
            rprint_int(seq);
            rprint("\n");
            if (seq < PING_COUNT) sleep(1);
            continue;
        }

        /* D-10: skip 20-byte IP header, validate ICMP type and id */
        unsigned char icmp_type = buf[20]; /* ICMP type field */
        unsigned char icmp_id_hi = buf[24];
        unsigned char icmp_id_lo = buf[25];

        /* T-44-02-01: validate type==0 (ICMP_ECHOREPLY) and id matches 0x1234 */
        if (icmp_type != 0 || icmp_id_hi != 0x12 || icmp_id_lo != 0x34) {
            rprint("ping: unexpected reply for icmp_seq=");
            rprint_int(seq);
            rprint("\n");
            if (seq < PING_COUNT) sleep(1);
            continue;
        }

        /* Compute RTT in ms */
        long rtt_ms = (t_end.tv_sec  - t_start.tv_sec)  * 1000L
                    + (t_end.tv_nsec - t_start.tv_nsec) / 1000000L;

        /* D-11: print per-packet result */
        rprint("64 bytes from ");
        rprint(argv[1]);
        rprint(": icmp_seq=");
        rprint_int(seq);
        rprint(" time=");
        rprint_int(rtt_ms);
        rprint(" ms\n");

        received_count++;

        /* D-12: 1-second interval between sends */
        if (seq < PING_COUNT) sleep(1);
    }

    close(fd);

    /* D-11: summary line */
    int loss_pct = (sent_count > 0)
                 ? ((sent_count - received_count) * 100 / sent_count)
                 : 100;

    rprint_int(sent_count);
    rprint(" packets transmitted, ");
    rprint_int(received_count);
    rprint(" received, ");
    rprint_int(loss_pct);
    rprint("% packet loss\n");

    return 0;
}
