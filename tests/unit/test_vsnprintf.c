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

/* test_vsnprintf.c — unit tests for vsnprintf unsigned/hex formatting.
 * Directly includes vsnprintf.c with stubs for kernel-only headers. */

/* stub_printk.h (force-included via CFLAGS) guards _MINIOS_IO_H_ and maps
 * printk -> printf. Pull in system headers first so our macros below don't
 * interfere with their declarations. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* Stub out <miniOS/drivers/console.h> before vsnprintf.c pulls it in.
 * Define the macro AFTER stdio.h so we don't mangle stdio's own putchar
 * declaration (function-like macros expand on `name(`, which would corrupt
 * the `int putchar(int)` prototype if defined first). */
#ifndef _MINIOS_DRIVERS_CONSOLE_H_
#define _MINIOS_DRIVERS_CONSOLE_H_
static inline void console_putchar(char c) { (void)c; }
#define putchar(c) ((void)(c))
#endif

/* Now pull in the implementation under test. */
#include "../src/kernel/mlibc/string/vsnprintf.c"

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Helper: call our kernel vsnprintf through a real varargs boundary. */
static int fmt(char *buf, size_t sz, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(buf, sz, format, ap);
    va_end(ap);
    return r;
}

/* Test 1: %lx of a high kernel address must not produce a minus sign */
void test_lx_large_kernel_address(void) {
    char buf[64];
    fmt(buf, sizeof(buf), "%lx", (unsigned long)0xFFFF800000100000UL);
    TEST_ASSERT_EQUAL_STRING("ffff800000100000", buf);
}

/* Test 2: %x of 0x80000000 (MSB set in 32-bit) must not produce minus */
void test_x_msb_set_32bit(void) {
    char buf[64];
    fmt(buf, sizeof(buf), "%x", 0x80000000U);
    TEST_ASSERT_EQUAL_STRING("80000000", buf);
}

/* Test 3: %016lx of a large address must be 16 hex chars, no minus */
void test_016lx_large_address(void) {
    char buf[64];
    fmt(buf, sizeof(buf), "%016lx", (unsigned long)0xFFFF800000001000UL);
    TEST_ASSERT_EQUAL_STRING("ffff800000001000", buf);
}

/* Test 4: %d of -42 must still print -42 (signed path unchanged) */
void test_d_negative(void) {
    char buf[64];
    fmt(buf, sizeof(buf), "%d", -42);
    TEST_ASSERT_EQUAL_STRING("-42", buf);
}

/* Test 5: %lu of a large unsigned value must print decimal without sign prefix */
void test_lu_large_unsigned_decimal(void) {
    char buf[64];
    fmt(buf, sizeof(buf), "%lu", (unsigned long)0xFFFF800000100000UL);
    /* Must start with a digit, not '-' */
    TEST_ASSERT_NOT_EQUAL('-', (unsigned char)buf[0]);
    /* Must be non-empty */
    TEST_ASSERT_TRUE(strlen(buf) > 0);
}

int main(void) {
    UnityBegin("test_vsnprintf.c");

    RUN_TEST(test_lx_large_kernel_address);
    RUN_TEST(test_x_msb_set_32bit);
    RUN_TEST(test_016lx_large_address);
    RUN_TEST(test_d_negative);
    RUN_TEST(test_lu_large_unsigned_decimal);

    return UnityEnd();
}
