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

/* test_console.c — unit tests for VT-backed console behaviour.
 * Directly includes console.c/vt.c with stubs for hardware-dependent headers.
 *
 * Stub strategy:
 *   - port.h   : guarded; outb/inb become no-ops
 *   - vmm.h    : guarded; VGA_BUFFER_VA redirected to fake_vga[]
 *   - console.h: left real (provides COM1_PORT, VGA_CURSOR_PORT_IDX, etc.)
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Stub port.h — guard, then provide no-op outb/inb macros            */
/* ------------------------------------------------------------------ */
#ifndef _MINIOS_ARCH_X86_64_PORT_H_
#define _MINIOS_ARCH_X86_64_PORT_H_
static inline void stub_outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t stub_inb(uint16_t port) { (void)port; return 0xFF; }
#define outb(port, val) stub_outb((uint16_t)(port), (uint8_t)(val))
#define inb(port)       stub_inb((uint16_t)(port))
#endif

/* ------------------------------------------------------------------ */
/* Fake VGA buffer — must be declared before vmm.h guard so the macro */
/* can reference it.                                                   */
/* ------------------------------------------------------------------ */
static uint8_t fake_vga[25 * 80 * 2];  /* SCREEN_ROWS * SCREEN_COLS * 2 */

/* ------------------------------------------------------------------ */
/* Stub vmm.h — guard it, then define VGA_BUFFER_VA -> fake_vga       */
/* ------------------------------------------------------------------ */
#ifndef _MINIOS_MM_VMM_H_
#define _MINIOS_MM_VMM_H_
#define VGA_BUFFER_VA ((uintptr_t)fake_vga)
#endif

/* ------------------------------------------------------------------ */
/* Spinlock stubs — vt.c uses spinlock_irqsave/restore for the fb lock */
/* ------------------------------------------------------------------ */
#ifndef _MINIOS_ARCH_X86_64_SPINLOCK_H_
#define _MINIOS_ARCH_X86_64_SPINLOCK_H_
typedef struct { volatile uint16_t counter; } spinlock_t;
#define SPINLOCK_INIT { .counter = 0 }
static inline void spinlock_lock(spinlock_t *l)                        { (void)l; }
static inline void spinlock_unlock(spinlock_t *l)                      { (void)l; }
static inline void spinlock_irqsave(spinlock_t *l, unsigned long *f)   { (void)l; (void)f; }
static inline void spinlock_irqrestore(spinlock_t *l, unsigned long f) { (void)l; (void)f; }
static inline void spinlock_irq(spinlock_t *l)                         { (void)l; }
static inline void spinlock_irq_unlock(spinlock_t *l)                  { (void)l; }
#endif

/* ------------------------------------------------------------------ */
/* Now include the implementation under test.                          */
/* console.c/vt.c will pull in console.h (real) and the already-      */
/* guarded port.h / vmm.h stubs.                                      */
/* ------------------------------------------------------------------ */
#include "../../src/arch/x86_64/drivers/vt.c"
#include "../../src/arch/x86_64/drivers/console.c"

#include "unity.h"

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                     */
/* ------------------------------------------------------------------ */

void setUp(void)
{
    memset(fake_vga, 0, sizeof(fake_vga));
    console_init();
    vt_set_cursor_visible(false);
}

void tearDown(void) {}

/* Helper: write N identical printable chars via console_putchar */
static void write_chars(char c, int n)
{
    for (int i = 0; i < n; i++)
        console_putchar(c);
}

/*
 * Fill the screen (2000 chars) then write one more to fire the scroll
 * path.  Total = SCREEN_ROWS * SCREEN_COLS + 1.
 */
static void trigger_scroll(void)
{
    write_chars('A', SCREEN_ROWS * SCREEN_COLS + 1);
}

static uint8_t vga_char_at(int row, int col)
{
    return fake_vga[2 * (row * SCREEN_COLS + col)];
}

static uint8_t vga_attr_at(int row, int col)
{
    return fake_vga[2 * (row * SCREEN_COLS + col) + 1];
}

/* ------------------------------------------------------------------ */
/* Test 1: scroll copies column 79 (all 80 columns)                   */
/* ------------------------------------------------------------------ */
/*
 * After scroll, what was in row 1 must appear in row 0 — including
 * column 79.  The buggy loop runs 24*79 iterations (not 24*80), so
 * column 79 of every source row is never copied.
 * Expected: FAIL with buggy console.c.
 */
void test_scroll_copies_all_80_columns(void)
{
    /* Write one full row to row 0 */
    write_chars('A', SCREEN_COLS);   /* offset now == 80 */

    /* Directly stamp row 1 with per-column distinct values.
     * Column k of row 1 gets char byte (k + 1) so column 79 == 80. */
    for (int col = 0; col < SCREEN_COLS; col++) {
        int idx = SCREEN_COLS + col;   /* row 1 base + col */
        fake_vga[2 * idx]     = (uint8_t)(col + 1);
        fake_vga[2 * idx + 1] = DEFAULT_TEXT_ATTR;
    }

    /* Advance the cursor to row 2 so subsequent writes do not overwrite row 1. */
    console_puts("\x1b[3;1H");

    /* Fill rows 2..24 and trigger scroll (+1 char) */
    write_chars('B', (SCREEN_ROWS - 2) * SCREEN_COLS + 1);

    /* After scroll: row 1 is now row 0.  Column 79 char byte == 80. */
    uint8_t actual = fake_vga[2 * 79];   /* row 0, col 79 */
    TEST_ASSERT_EQUAL_UINT8(80, actual);
}

/* ------------------------------------------------------------------ */
/* Test 2: scroll clears all 80 cells of the bottom row               */
/* ------------------------------------------------------------------ */
/*
 * After scroll the last row (row 24, VGA indices 1920..1999) must be
 * entirely zeroed.  The buggy clear loop starts at 1896 (not 1920) and
 * ends at 1975 (not 2000) — leaving cells at columns >=79 non-zero.
 * Expected: FAIL with buggy console.c.
 */
void test_scroll_clears_full_bottom_row(void)
{
    /* Stamp non-zero values across the entire buffer */
    for (int i = 0; i < SCREEN_ROWS * SCREEN_COLS; i++) {
        fake_vga[2 * i]     = 0xFF;
        fake_vga[2 * i + 1] = 0xFF;
    }

    trigger_scroll();
    /* The trigger char lands at last-row col 0; backspace it away so the
     * assertion can check all 80 cells uniformly. */
    console_putchar('\b');

    int last_row = (SCREEN_ROWS - 1) * SCREEN_COLS;
    for (int col = 0; col < SCREEN_COLS; col++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(' ', fake_vga[2 * (last_row + col)],
            "last-row char byte not cleared after scroll");
    }
}

/* ------------------------------------------------------------------ */
/* Test 3: offset after scroll places next char at VGA index 1920     */
/* ------------------------------------------------------------------ */
/*
 * After scroll, offset must equal (SCREEN_ROWS-1)*SCREEN_COLS == 1920.
 * We verify indirectly: trigger_scroll writes the (+1)-th char, which
 * should land at VGA index 1920.
 * The numeric offset calculation (2000-80=1920) is correct in the buggy
 * code, so this test is expected to PASS.
 */
void test_offset_after_scroll(void)
{
    trigger_scroll();   /* last char ('A') lands at offset 1920 */

    uint8_t char_at_1920 = fake_vga[2 * 1920];
    TEST_ASSERT_EQUAL_UINT8_MESSAGE('A', char_at_1920,
        "first char after scroll not at VGA index 1920");
}

/* ------------------------------------------------------------------ */
/* Test 4: newline on last row triggers scroll; next char at col 0    */
/* ------------------------------------------------------------------ */
/*
 * Write 24 newlines (rows 0..23), then one more newline.  At that point
 * offset == SCREEN_COLS*25 == 2000.  Writing a printable char fires the
 * scroll path and the char must appear at VGA index 1920.
 * Expected: PASS on offset; FAIL if scroll corrupts VGA (tests 1&2 catch that).
 */
void test_newline_on_last_row_triggers_scroll(void)
{
    /* Move cursor to the last row via 24 newlines */
    for (int row = 0; row < SCREEN_ROWS - 1; row++)
        console_putchar('\n');

    /* One more newline: offset = SCREEN_COLS * 25 = 2000 */
    console_putchar('\n');

    /* Next printable char must scroll then land at index 1920 */
    console_putchar('Z');

    uint8_t char_at_1920 = fake_vga[2 * 1920];
    TEST_ASSERT_EQUAL_UINT8_MESSAGE('Z', char_at_1920,
        "char after newline-triggered scroll not at VGA index 1920");
}

void test_vt_escape_sequences_move_cursor_without_literal_output(void)
{
    char seq[] = "ABC\x1b[2D!";

    console_puts(seq);

    TEST_ASSERT_EQUAL_UINT8('A', vga_char_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8('!', vga_char_at(0, 1));
    TEST_ASSERT_EQUAL_UINT8('C', vga_char_at(0, 2));
    TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, 3));
}

void test_vt_erase_line_variants_clear_expected_cells(void)
{
    char clear_screen[] = "\x1b[2J";
    char erase_tail[] = "ABCDE\x1b[2D\x1b[K";
    char erase_head[] = "ABCDE\x1b[1K";
    char erase_all[] = "ABCDE\x1b[2K";

    console_puts(clear_screen);
    for (int row = 0; row < SCREEN_ROWS; row++) {
        for (int col = 0; col < SCREEN_COLS; col++) {
            TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(row, col));
            TEST_ASSERT_EQUAL_UINT8(DEFAULT_TEXT_ATTR, vga_attr_at(row, col));
        }
    }

    console_puts(erase_tail);
    TEST_ASSERT_EQUAL_UINT8('A', vga_char_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8('B', vga_char_at(0, 1));
    TEST_ASSERT_EQUAL_UINT8('C', vga_char_at(0, 2));
    TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, 3));
    TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, 4));

    console_clear();
    console_puts(erase_head);
    TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, 4));

    console_clear();
    console_puts(erase_all);
    for (int col = 0; col < 5; col++) {
        TEST_ASSERT_EQUAL_UINT8(' ', vga_char_at(0, col));
    }
}

void test_vt_save_and_restore_cursor_round_trips_position(void)
{
    char seq[] = "hello\x1b[s\x1b[10;10HX\x1b[u!";

    console_puts(seq);

    TEST_ASSERT_EQUAL_UINT8('h', vga_char_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8('o', vga_char_at(0, 4));
    TEST_ASSERT_EQUAL_UINT8('!', vga_char_at(0, 5));
    TEST_ASSERT_EQUAL_UINT8('X', vga_char_at(9, 9));
}

void test_vt_sgr_changes_attribute_and_reset_restores_default(void)
{
    char seq[] = "\x1b[31mX\x1b[0mY";

    console_puts(seq);

    TEST_ASSERT_NOT_EQUAL(DEFAULT_TEXT_ATTR, vga_attr_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8(DEFAULT_TEXT_ATTR, vga_attr_at(0, 1));
    TEST_ASSERT_EQUAL_UINT8('X', vga_char_at(0, 0));
    TEST_ASSERT_EQUAL_UINT8('Y', vga_char_at(0, 1));
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    UnityBegin("test_console.c");

    RUN_TEST(test_scroll_copies_all_80_columns);
    RUN_TEST(test_scroll_clears_full_bottom_row);
    RUN_TEST(test_offset_after_scroll);
    RUN_TEST(test_newline_on_last_row_triggers_scroll);
    RUN_TEST(test_vt_escape_sequences_move_cursor_without_literal_output);
    RUN_TEST(test_vt_erase_line_variants_clear_expected_cells);
    RUN_TEST(test_vt_save_and_restore_cursor_round_trips_position);
    RUN_TEST(test_vt_sgr_changes_attribute_and_reset_restores_default);

    return UnityEnd();
}
