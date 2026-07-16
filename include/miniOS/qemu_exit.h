/* SPDX-License-Identifier: MIT */
/**
 * @file qemu_exit.h
 * @brief QEMU isa-debug-exit device helper.
 *
 * Provides qemu_exit() which triggers QEMU's isa-debug-exit device to
 * exit the emulator with a deterministic exit code. Used by functional
 * tests to signal pass/fail without relying on serial output polling.
 *
 * isa-debug-exit protocol:
 *   QEMU exit code = (value_written_to_port << 1) | 1
 *   qemu_exit(0) -> QEMU exits with code 1 (test PASS)
 *   qemu_exit(1) -> QEMU exits with code 3 (test FAIL)
 *
 * Device must be added to QEMU invocation:
 *   -device isa-debug-exit,iobase=0xf4,iosize=0x04
 */

#ifndef MINIOS_QEMU_EXIT_H
#define MINIOS_QEMU_EXIT_H

#include <miniOS/types.h>

/**
 * @brief Exit QEMU via the isa-debug-exit device.
 *
 * Writes @p code to port 0xf4. QEMU translates this to exit code
 * @c (code << 1) | 1. Call with 0 for PASS (QEMU exits 1), 1 for
 * FAIL (QEMU exits 3).
 *
 * @param code Exit value to encode. 0 = pass, 1 = fail.
 */
void qemu_exit(uint8_t code);

#endif /* MINIOS_QEMU_EXIT_H */
