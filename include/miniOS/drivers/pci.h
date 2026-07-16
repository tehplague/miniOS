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

#ifndef _MINIOS_DRIVERS_PCI_H_
#define _MINIOS_DRIVERS_PCI_H_

/**
 * @file pci.h
 * @defgroup pci PCI
 * @brief PCI bus enumeration and configuration space access.
 *
 * Provides the pci_device_t struct and the global pci_devices[] array
 * populated by pci_init() at boot. Also exports the shared config-space
 * read/write helpers previously duplicated in ata.c.
 */

#include <miniOS/types.h>

/** Maximum number of PCI devices tracked by the kernel. */
#define PCI_MAX_DEVICES 64

/**
 * pci_device_t - Descriptor for a single PCI function discovered during enumeration.
 *
 * Populated by pci_init() for every non-empty bus/device/function tuple on bus 0.
 */
typedef struct {
    uint8_t  bus;             /**< PCI bus number (always 0 for bus-0 scan) */
    uint8_t  dev;             /**< PCI device number (0-31) */
    uint8_t  fn;              /**< PCI function number (0-7) */
    uint16_t vendor_id;       /**< Vendor ID from config-space offset 0x00 low 16 bits */
    uint16_t device_id;       /**< Device ID from config-space offset 0x00 high 16 bits */
    uint8_t  class_code;      /**< Base class code (bits 31-24 of offset 0x08) */
    uint8_t  subclass;        /**< Sub-class code (bits 23-16 of offset 0x08) */
    uint8_t  prog_if;         /**< Programming interface (bits 15-8 of offset 0x08) */
    uint8_t  revision_id;     /**< Revision ID (bits 7-0 of offset 0x08) */
    uint16_t subsys_vendor;   /**< Subsystem vendor ID (offset 0x2C low 16) */
    uint16_t subsys_device;   /**< Subsystem device ID (offset 0x2C high 16) */
    uint8_t  irq_line;        /**< IRQ line (offset 0x3C bits 7-0) */
    uint8_t  irq_pin;         /**< IRQ pin (offset 0x3C bits 15-8) */
    uint32_t bar[6];          /**< Base Address Registers 0-5 (offsets 0x10-0x24) */
    uint8_t  header_type;     /**< Header type byte (offset 0x0C bits 23-16) */
} pci_device_t;

/** Global array of discovered PCI devices; populated by pci_init(). */
extern pci_device_t pci_devices[PCI_MAX_DEVICES];

/** Number of valid entries in pci_devices[]; set by pci_init(). */
extern int pci_device_count;

/**
 * pci_init() - Enumerate PCI bus 0 and populate /sys/bus/pci/devices/.
 *
 * Scans bus 0, slots 0-31, respecting the multi-function bit. Fills
 * pci_devices[] with every non-empty function. Prints the device count
 * via printk(). Then creates /sys/bus/pci/devices/<domain>:<bus>:<dev>.<fn>/
 * directories with vendor, device, class, subsystem_vendor, subsystem_device
 * attribute files.
 *
 * Must be called after sysfs_init() and before ata_init().
 */
void pci_init(void);

/**
 * pci_config_read32() - Read a 32-bit dword from PCI configuration space.
 * @bus: PCI bus number.
 * @dev: PCI device number (0-31).
 * @fn:  PCI function number (0-7).
 * @off: Register offset (must be 4-byte aligned; lower 2 bits ignored).
 *
 * @return: 32-bit value read from the PCI configuration register.
 */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);

/**
 * pci_config_write32() - Write a 32-bit dword to PCI configuration space.
 * @bus: PCI bus number.
 * @dev: PCI device number (0-31).
 * @fn:  PCI function number (0-7).
 * @off: Register offset (must be 4-byte aligned; lower 2 bits ignored).
 * @val: Value to write.
 */
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);

#endif /* _MINIOS_DRIVERS_PCI_H_ */
