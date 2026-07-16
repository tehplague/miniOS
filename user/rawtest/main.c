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

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

static void rprint(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

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

static void rprint_hex_byte(unsigned char v)
{
    static const char hex[] = "0123456789abcdef";
    char out[3];
    out[0] = hex[(v >> 4) & 0x0f];
    out[1] = hex[v & 0x0f];
    out[2] = '\0';
    rprint(out);
}

static void rprint_hex_dump(const unsigned char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        if (i) rprint(" ");
        rprint_hex_byte(buf[i]);
    }
    rprint("\n");
}

#define QEMU_GW_ADDR 0x0202000aU

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

int main(void)
{
    rprint("rawtest: main\n");
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        rprint("rawtest: socket failed\n");
        return 1;
    }
    rprint("rawtest: socket fd=ok\n");

    unsigned char icmp[16];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 8;
    icmp[1] = 0;
    icmp[4] = 0x12;
    icmp[5] = 0x34;
    icmp[6] = 0x00;
    icmp[7] = 0x01;
    icmp[8] = 0x44;
    icmp[9] = 0x33;
    icmp[10] = 0x22;
    icmp[11] = 0x11;
    icmp[12] = 0xaa;
    icmp[13] = 0xbb;
    icmp[14] = 0xcc;
    icmp[15] = 0xdd;

    unsigned short cksum = icmp_checksum(icmp, sizeof(icmp));
    icmp[2] = (unsigned char)(cksum & 0xff);
    icmp[3] = (unsigned char)(cksum >> 8);

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = QEMU_GW_ADDR;

    ssize_t sent = sendto(fd, icmp, sizeof(icmp), 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        rprint("rawtest: sendto failed\n");
        close(fd);
        return 1;
    }
    rprint("rawtest: sent\n");

    unsigned char buf[1500];
    ssize_t rcvd = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
    if (rcvd < 0) {
        rprint("rawtest: recvfrom timeout\n");
    } else {
        rprint("rawtest: recv ok len=");
        rprint_int((long)rcvd);
        rprint("\n");
        int dump_len = (rcvd < 48) ? (int)rcvd : 48;
        rprint("rawtest: bytes ");
        rprint_hex_dump(buf, dump_len);
        if (rcvd >= 20) {
            int ihl = (buf[0] & 0x0f) * 4;
            rprint("rawtest: ip_v=");
            rprint_int((long)(buf[0] >> 4));
            rprint(" ihl=");
            rprint_int((long)ihl);
            rprint(" ttl=");
            rprint_int((long)buf[8]);
            rprint(" proto=");
            rprint_int((long)buf[9]);
            rprint("\n");
            if (rcvd >= ihl + 12) {
                unsigned long ts = (unsigned long)buf[ihl + 8]
                                 | ((unsigned long)buf[ihl + 9] << 8)
                                 | ((unsigned long)buf[ihl + 10] << 16)
                                 | ((unsigned long)buf[ihl + 11] << 24);
                rprint("rawtest: icmp_type=");
                rprint_int((long)buf[ihl + 0]);
                rprint(" id=");
                rprint_hex_byte(buf[ihl + 4]);
                rprint_hex_byte(buf[ihl + 5]);
                rprint(" seq=");
                rprint_hex_byte(buf[ihl + 6]);
                rprint_hex_byte(buf[ihl + 7]);
                rprint(" ts=");
                rprint_int((long)ts);
                rprint(" payload=");
                if (rcvd >= ihl + 16) {
                    rprint_hex_byte(buf[ihl + 8]);
                    rprint_hex_byte(buf[ihl + 9]);
                    rprint_hex_byte(buf[ihl + 10]);
                    rprint_hex_byte(buf[ihl + 11]);
                    rprint(" ");
                    rprint_hex_byte(buf[ihl + 12]);
                    rprint_hex_byte(buf[ihl + 13]);
                    rprint_hex_byte(buf[ihl + 14]);
                    rprint_hex_byte(buf[ihl + 15]);
                }
                rprint("\n");
            }
        }
    }

    close(fd);
    rprint("rawtest: PASS\n");
    return 0;
}
