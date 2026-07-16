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

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>

/* linux_dirent64-compatible struct as filled by SYS_getdents (kernel case 78).
 * d_type values: DT_UNKNOWN=0, DT_DIR=4, DT_REG=8. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];   /* oversized for stack layout; actual name is NUL-terminated */
};

#define DT_DIR 4
#define DT_REG 8

static long getdents(int fd, void *buf, unsigned int count) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "0"(78L), "D"((long)fd), "S"(buf), "d"((long)count)
        : "rcx", "r11", "memory");
    return ret;
}

/* ── Human-readable PCI class name table ─────────────────────────────────── */

static const char *pci_class_name(uint8_t base_class)
{
    switch (base_class) {
        case 0x00: return "Unclassified";
        case 0x01: return "Mass Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Communication";
        case 0x08: return "System Peripheral";
        case 0x09: return "Input Device";
        case 0x0A: return "Docking Station";
        case 0x0B: return "Processor";
        case 0x0C: return "Serial Bus";
        case 0x0D: return "Wireless";
        case 0x0E: return "Intelligent";
        case 0x0F: return "Satellite";
        case 0x10: return "Encryption";
        case 0x11: return "Signal Processing";
        case 0xFF: return "Unassigned";
        default:   return "Unknown";
    }
}

/* ── sysfs attribute reader ──────────────────────────────────────────────── */

/**
 * read_attr() - Read a sysfs attribute file into buf.
 * @dir:    Absolute path to the device directory.
 * @attr:   Attribute file name (e.g. "vendor").
 * @buf:    Destination buffer.
 * @buflen: Size of buf (including space for NUL terminator).
 *
 * Strips a trailing newline if present.
 * Returns 0 on success, -1 on error.
 */
static int read_attr(const char *dir, const char *attr, char *buf, int buflen)
{
    char path[256];
    int dlen = (int)strlen(dir);
    int alen = (int)strlen(attr);
    if (dlen + 1 + alen >= (int)sizeof(path) - 1)
        return -1;
    memcpy(path, dir, (size_t)dlen);
    path[dlen] = '/';
    memcpy(path + dlen + 1, attr, (size_t)alen + 1);

    int fd = open(path, 0);
    if (fd < 0)
        return -1;
    int n = (int)read(fd, buf, (size_t)(buflen - 1));
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    /* Strip trailing newline */
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    return 0;
}

/* ── Hex string parsers ───────────────────────────────────────────────────── */

/**
 * parse_hex16() - Parse a "0xNNNN" string into a uint16_t.
 */
static uint16_t parse_hex16(const char *s)
{
    uint16_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        val = (uint16_t)(val << 4);
        if (*s >= '0' && *s <= '9')      val |= (uint16_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') val |= (uint16_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') val |= (uint16_t)(*s - 'A' + 10);
    }
    return val;
}

/**
 * parse_hex32() - Parse a "0xNNNNNNNN" string into a uint32_t.
 */
static uint32_t parse_hex32(const char *s)
{
    uint32_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        val <<= 4;
        if (*s >= '0' && *s <= '9')      val |= (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') val |= (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') val |= (uint32_t)(*s - 'A' + 10);
    }
    return val;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-v", 2) == 0)
            verbose = 1;
    }

    const char *devdir = "/sys/bus/pci/devices";
    int dfd = open(devdir, 0);
    if (dfd < 0) {
        printf("lspci: cannot open %s (no PCI sysfs?)\n", devdir);
        return 1;
    }

    char buf[4096];
    long n;
    while ((n = getdents(dfd, buf, sizeof(buf))) > 0) {
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64 *ent = (struct linux_dirent64 *)(buf + pos);

            /* Skip . and .. */
            if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' ||
                 (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
                pos += ent->d_reclen;
                continue;
            }

            /* Build full device directory path: devdir + "/" + d_name */
            char devpath[256];
            int plen = (int)strlen(devdir);
            int nlen = (int)strlen(ent->d_name);
            memcpy(devpath, devdir, (size_t)plen);
            devpath[plen] = '/';
            memcpy(devpath + plen + 1, ent->d_name, (size_t)nlen + 1);

            /* Read mandatory attributes */
            char vendor_s[16], device_s[16], class_s[16];
            if (read_attr(devpath, "vendor", vendor_s, sizeof(vendor_s)) < 0) {
                pos += ent->d_reclen;
                continue;
            }
            read_attr(devpath, "device", device_s, sizeof(device_s));
            read_attr(devpath, "class",  class_s,  sizeof(class_s));

            uint16_t vendor    = parse_hex16(vendor_s);
            uint16_t device    = parse_hex16(device_s);
            uint16_t class_val = parse_hex16(class_s);
            uint8_t  base_class = (uint8_t)(class_val >> 8);

            /* Strip "0000:" domain prefix from directory name for display.
             * Directory format: "0000:BB:DD.F" — skip first 5 chars. */
            const char *bdf = ent->d_name;
            if (nlen > 5 && bdf[4] == ':')
                bdf += 5;

            /* Default output: BB:DD.F Class CCCC: VVVV:DDDD [Class string] */
            printf("%s Class %04x: %04x:%04x [%s]\n",
                   bdf, (unsigned)class_val,
                   (unsigned)vendor, (unsigned)device,
                   pci_class_name(base_class));

            /* Verbose output: subsystem, BARs, IRQ */
            if (verbose) {
                char subsys_v[16], subsys_d[16];
                if (read_attr(devpath, "subsystem_vendor", subsys_v, sizeof(subsys_v)) == 0 &&
                    read_attr(devpath, "subsystem_device", subsys_d, sizeof(subsys_d)) == 0) {
                    printf("        Subsystem: %s:%s\n", subsys_v, subsys_d);
                }

                /* BARs: read bar0..bar5 */
                static const char *bar_names[6] = {
                    "bar0", "bar1", "bar2", "bar3", "bar4", "bar5"
                };
                for (int b = 0; b < 6; b++) {
                    char bar_s[16];
                    if (read_attr(devpath, bar_names[b], bar_s, sizeof(bar_s)) == 0) {
                        uint32_t bar_val = parse_hex32(bar_s);
                        if (bar_val == 0)
                            continue;
                        const char *type = (bar_val & 1) ? "I/O" : "Memory";
                        printf("        BAR%d: 0x%08x [%s]\n",
                               b, (unsigned)bar_val, type);
                    }
                }

                /* IRQ line and pin */
                char irq_line_s[16], irq_pin_s[16];
                if (read_attr(devpath, "irq_line", irq_line_s, sizeof(irq_line_s)) == 0 &&
                    read_attr(devpath, "irq_pin",  irq_pin_s,  sizeof(irq_pin_s))  == 0) {
                    printf("        IRQ: line=%s pin=%s\n", irq_line_s, irq_pin_s);
                }
            }

            pos += ent->d_reclen;
        }
    }

    close(dfd);
    return 0;
}
