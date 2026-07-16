/* SPDX-License-Identifier: MIT */
/**
 * @file qemu_exit.c
 * @brief QEMU isa-debug-exit device implementation.
 */

#include <miniOS/qemu_exit.h>

/**
 * @brief Write a byte to an I/O port.
 *
 * Bare-metal x86-64 port I/O. Matches the outb pattern used elsewhere
 * in the kernel (e.g. keyboard driver, LAPIC).
 *
 * @param port I/O port address.
 * @param val  Byte to write.
 */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Exit QEMU via the isa-debug-exit device at port 0xf4.
 *
 * QEMU computes exit code as (code << 1) | 1:
 *   qemu_exit(0) -> QEMU exits 1  (functional test PASS)
 *   qemu_exit(1) -> QEMU exits 3  (functional test FAIL / panic)
 *
 * If QEMU is not running with -device isa-debug-exit,iobase=0xf4,iosize=0x04
 * this is a no-op (the port write is ignored).
 */
void qemu_exit(uint8_t code)
{
    outb(0xf4, code);
}
