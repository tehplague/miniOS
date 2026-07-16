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
 * @file pci.c
 * @brief PCI bus enumeration and /sys/bus/pci/devices/ stub sysfs population.
 *
 * Implements pci_init() which scans PCI bus 0, fills the global pci_devices[]
 * array, and creates a static /sys directory tree populated at boot. Also
 * provides the shared pci_config_read32()/pci_config_write32() helpers that
 * replace the duplicates previously in ata.c.
 */

#include <miniOS/drivers/pci.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/drivers/console.h>
#include <miniOS/fs/sysfs.h>
#include <miniOS/io.h>
#include <string.h>

/* ── Global state ─────────────────────────────────────────────────────────── */

pci_device_t pci_devices[PCI_MAX_DEVICES];
int          pci_device_count = 0;

/* ── Config-space access ──────────────────────────────────────────────────── */

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR_PORT, addr);
    return inl(PCI_CONFIG_DATA_PORT);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint32_t val)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (off & 0xFC);
    outl(PCI_CONFIG_ADDR_PORT, addr);
    outl(PCI_CONFIG_DATA_PORT, val);
}

/* ── /sys population helpers ─────────────────────────────────────────────── */

/**
 * hex_nibble() - Convert a 4-bit nibble to its ASCII hex character.
 */
static char hex_nibble(uint8_t n)
{
    n &= 0xF;
    return (n < 10) ? (char)('0' + n) : (char)('a' + n - 10);
}

/**
 * hex8() - Write a 2-character hex representation of val into out[0..1].
 * Does NOT null-terminate.
 */
static void hex8(char *out, uint8_t val)
{
    out[0] = hex_nibble(val >> 4);
    out[1] = hex_nibble(val);
}

/**
 * hex16() - Write a 4-character hex representation of val into out[0..3].
 * Does NOT null-terminate.
 */
static void hex16(char *out, uint16_t val)
{
    hex8(out,     (uint8_t)(val >> 8));
    hex8(out + 2, (uint8_t)(val));
}

/**
 * hex32() - Write an 8-character hex representation of val into out[0..7].
 * Does NOT null-terminate.
 */
static void hex32(char *out, uint32_t val)
{
    hex16(out,     (uint16_t)(val >> 16));
    hex16(out + 4, (uint16_t)(val));
}

/**
 * pci_write_attr16() - Create a sysfs file in dir and write "0xXXXX\n".
 */
static void pci_write_attr16(sysfs_node_t *dir, const char *name, uint16_t val)
{
    char buf[8]; /* "0xXXXX\n" = 7 chars */
    buf[0] = '0'; buf[1] = 'x';
    hex16(buf + 2, val);
    buf[6] = '\n';
    sysfs_create_file(dir, name, buf, 7, NULL);
}

/**
 * pci_write_attr32() - Create a sysfs file in dir and write "0xNNNNNNNN\n".
 */
static void pci_write_attr32(sysfs_node_t *dir, const char *name, uint32_t val)
{
    char buf[12]; /* "0xNNNNNNNN\n" = 11 chars */
    buf[0] = '0'; buf[1] = 'x';
    hex32(buf + 2, val);
    buf[10] = '\n';
    sysfs_create_file(dir, name, buf, 11, NULL);
}

/**
 * pci_write_class() - Create a sysfs file in dir and write "0xCCSS\n".
 */
static void pci_write_class(sysfs_node_t *dir, uint8_t class_code, uint8_t subclass)
{
    char buf[8];
    buf[0] = '0'; buf[1] = 'x';
    hex8(buf + 2, class_code);
    hex8(buf + 4, subclass);
    buf[6] = '\n';
    sysfs_create_file(dir, "class", buf, 7, NULL);
}

/**
 * pci_write_dec() - Create a sysfs file in dir and write a decimal uint8 + newline.
 */
static void pci_write_dec(sysfs_node_t *dir, const char *name, uint8_t val)
{
    char buf[5]; /* "255\n\0" */
    int len = 0;
    if (val >= 100) buf[len++] = (char)('0' + val / 100);
    if (val >= 10)  buf[len++] = (char)('0' + (val / 10) % 10);
    buf[len++] = (char)('0' + val % 10);
    buf[len++] = '\n';
    sysfs_create_file(dir, name, buf, (uint32_t)len, NULL);
}

/**
 * pci_populate_sysfs() - Build /sys/bus/pci/devices/ using sysfs API.
 *
 * Creates the directory tree rooted at g_sysfs_root and populates
 * per-device attribute files. Safe to call before ext2 root is mounted
 * since it only touches the in-kernel sysfs node tree.
 */
static void pci_populate_sysfs(void)
{
    sysfs_node_t *bus_node     = sysfs_create_dir(g_sysfs_root, "bus");
    sysfs_node_t *pci_node     = sysfs_create_dir(bus_node,      "pci");
    sysfs_node_t *devices_node = sysfs_create_dir(pci_node,      "devices");

    static const char *bar_names[6] = {
        "bar0", "bar1", "bar2", "bar3", "bar4", "bar5"
    };

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];

        char dirname[16];
        snprintf(dirname, sizeof(dirname), "0000:%02x:%02x.%x",
                 (unsigned)d->bus, (unsigned)d->dev, (unsigned)d->fn);

        /* dirname is on stack — sysfs_node_t stores caller-owned const char *name.
         * Use a static per-device buffer so the name persists beyond this function. */
        static char dir_names[PCI_MAX_DEVICES][16];
        memcpy(dir_names[i], dirname, sizeof(dirname));

        sysfs_node_t *dev_node = sysfs_create_dir(devices_node, dir_names[i]);
        if (!dev_node) continue;

        pci_write_attr16(dev_node, "vendor",           d->vendor_id);
        pci_write_attr16(dev_node, "device",           d->device_id);
        pci_write_class (dev_node, d->class_code,      d->subclass);
        pci_write_attr16(dev_node, "subsystem_vendor", d->subsys_vendor);
        pci_write_attr16(dev_node, "subsystem_device", d->subsys_device);

        for (int b = 0; b < 6; b++)
            pci_write_attr32(dev_node, bar_names[b], d->bar[b]);

        pci_write_dec(dev_node, "irq_line", d->irq_line);
        pci_write_dec(dev_node, "irq_pin",  d->irq_pin);
    }
}

/* ── pci_init ─────────────────────────────────────────────────────────────── */

/**
 * pci_init() - Enumerate PCI bus 0 and populate /sys/bus/pci/devices/.
 *
 * Scans all 32 device slots on bus 0. For multi-function devices (header type
 * bit 7 set), iterates functions 0-7; single-function devices use only fn 0.
 * Each discovered function is stored in pci_devices[pci_device_count++].
 * Capped at PCI_MAX_DEVICES.
 */
void pci_init(void)
{
    pci_device_count = 0;

    for (uint8_t dev = 0; dev < 32; dev++) {
        /* Check slot: read vendor/device at fn=0 */
        uint32_t vendor_fn0 = pci_config_read32(0, dev, 0, 0x00);
        if ((vendor_fn0 & 0xFFFF) == 0xFFFF)
            continue; /* empty slot */

        /* Read header type to determine multi-function */
        uint32_t htype_reg = pci_config_read32(0, dev, 0, 0x0C);
        uint8_t  htype     = (uint8_t)((htype_reg >> 16) & 0xFF);
        uint8_t  max_fn    = (htype & 0x80) ? 7 : 0;

        for (uint8_t fn = 0; fn <= max_fn; fn++) {
            uint32_t vendor_dev = pci_config_read32(0, dev, fn, 0x00);
            uint16_t vendor_id  = (uint16_t)(vendor_dev & 0xFFFF);
            if (vendor_id == 0xFFFF)
                continue; /* function not present */

            if (pci_device_count >= PCI_MAX_DEVICES)
                break;

            pci_device_t *d = &pci_devices[pci_device_count++];
            d->bus       = 0;
            d->dev       = dev;
            d->fn        = fn;
            d->vendor_id = vendor_id;
            d->device_id = (uint16_t)(vendor_dev >> 16);

            /* Class / revision — offset 0x08 */
            uint32_t class_rev  = pci_config_read32(0, dev, fn, 0x08);
            d->class_code  = (uint8_t)((class_rev >> 24) & 0xFF);
            d->subclass    = (uint8_t)((class_rev >> 16) & 0xFF);
            d->prog_if     = (uint8_t)((class_rev >>  8) & 0xFF);
            d->revision_id = (uint8_t)(class_rev & 0xFF);

            /* Subsystem — offset 0x2C */
            uint32_t subsys = pci_config_read32(0, dev, fn, 0x2C);
            d->subsys_vendor = (uint16_t)(subsys & 0xFFFF);
            d->subsys_device = (uint16_t)(subsys >> 16);

            /* IRQ — offset 0x3C */
            uint32_t irq_reg = pci_config_read32(0, dev, fn, 0x3C);
            d->irq_line = (uint8_t)(irq_reg & 0xFF);
            d->irq_pin  = (uint8_t)((irq_reg >> 8) & 0xFF);

            /* BARs 0-5 — offsets 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24 */
            for (int b = 0; b < 6; b++)
                d->bar[b] = pci_config_read32(0, dev, fn, (uint8_t)(0x10 + b * 4));

            /* Header type (re-read for fn > 0) */
            if (fn == 0) {
                d->header_type = htype;
            } else {
                uint32_t ht_reg = pci_config_read32(0, dev, fn, 0x0C);
                d->header_type  = (uint8_t)((ht_reg >> 16) & 0xFF);
            }
        }

        if (pci_device_count >= PCI_MAX_DEVICES)
            break;
    }

    printk("PCI: found %d device(s)\n", pci_device_count);
    pci_populate_sysfs();
}
