/* stub_pci.c — minimal PCI config-space stubs for host-native unit tests. */

#include <miniOS/drivers/pci.h>

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    (void)bus; (void)dev; (void)fn; (void)off;
    return 0;
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    (void)bus; (void)dev; (void)fn; (void)off; (void)val;
}
