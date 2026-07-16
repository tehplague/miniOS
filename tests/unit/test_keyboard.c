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

/* test_keyboard.c — Unit tests for PS/2 keyboard + tty line discipline
 * Directly #includes tty.c and keyboard.c to access static internals.
 *
 * All hardware-dependent functions (inb/outb, irq_set_handler,
 * ioapic_unmask_irq) are stubbed here BEFORE keyboard.c is included.
 * The header guards prevent keyboard.c from re-including the real headers.
 */

#include "unity.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * Port I/O stubs — override port.h before keyboard.c includes it
 * --------------------------------------------------------------------- */
#define _MINIOS_ARCH_X86_64_PORT_H_

static uint8_t fake_port_0x60_value = 0;
static uint8_t fake_port_0x64_value = 0;

static inline uint8_t inb(int port) {
    if (port == 0x60) return fake_port_0x60_value;
    if (port == 0x64) return fake_port_0x64_value;
    return 0;
}

static inline void outb(int port, uint8_t val) { (void)port; (void)val; }

/* -----------------------------------------------------------------------
 * IRQ header stubs — override irq.h before keyboard.c includes it.
 * We must define the typedef that keyboard.c uses AND provide
 * irq_set_handler() with the exact signature from ioapic.c.
 * --------------------------------------------------------------------- */
#define _MINIOS_ARCH_X86_64_IRQ_H_

/* typedef used by irq_set_handler — must match irq.h exactly */
typedef void idt_handler_func(void *interrupt_stack_frame);

#define NR_IRQS 16

static inline void irq_set_handler(uint8_t irq, idt_handler_func *fn) {
    (void)irq; (void)fn;
}

/* -----------------------------------------------------------------------
 * APIC stub — override apic.h before keyboard.c includes it.
 * --------------------------------------------------------------------- */
#define _MINIOS_ARCH_X86_64_APIC_H_

static inline void ioapic_unmask_irq(uint8_t irq) { (void)irq; }

/* -----------------------------------------------------------------------
 * Spinlock stubs — tty.c uses spinlock_irqsave/restore for host tests too.
 * --------------------------------------------------------------------- */
#define _MINIOS_ARCH_X86_64_SPINLOCK_H_

typedef struct {
    volatile uint16_t counter;
} spinlock_t;

#define SPINLOCK_INIT { .counter = 0 }

static inline void spinlock_irqsave(spinlock_t *lock, unsigned long *flags) {
    (void)lock;
    *flags = 0;
}

static inline void spinlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    (void)lock;
    (void)flags;
}

/* -----------------------------------------------------------------------
 * VT and scheduler stubs used by tty.c.
 * --------------------------------------------------------------------- */
static char echoed_bytes[512];
static size_t echoed_len = 0;

void vt_write_byte(char c) {
    if (echoed_len < sizeof(echoed_bytes)) {
        echoed_bytes[echoed_len++] = c;
    }
}

void vt_get_winsize(uint16_t *rows, uint16_t *cols) {
    *rows = 25;
    *cols = 80;
}

#define _MINIOS_SCHED_SCHED_H_

/* Minimal thread stub — only the fields referenced by tty.c are needed. */
typedef enum {
    THREAD_RUNNABLE = 0,
    THREAD_RUNNING  = 1,
    THREAD_DEAD     = 2,
    THREAD_WAITING  = 3,
    THREAD_BLOCKED  = 4,
} thread_state_t;

#define SCHED_MAX_THREADS 8

struct thread {
    uint32_t      pending_signals;
    uint32_t      pgid;
    uint32_t      sid;
    uint32_t      pid;
    thread_state_t state;
};

struct thread thread_pool[SCHED_MAX_THREADS];

static struct thread fake_current_thread;

static inline struct thread *sched_current(void) {
    return &fake_current_thread;
}

static inline void sched_signal_thread(struct thread *t, int sig) {
    if (t && sig > 0 && sig < 32)
        t->pending_signals |= (1u << sig);
}

/* -----------------------------------------------------------------------
 * Now include the implementation under test
 * --------------------------------------------------------------------- */
#include "../../src/kernel/drivers/tty.c"
#include "../../src/arch/x86_64/drivers/keyboard.c"

/* -----------------------------------------------------------------------
 * setUp / tearDown
 * --------------------------------------------------------------------- */
void setUp(void) {
    kb_shift = 0;
    fake_port_0x60_value = 0;
    fake_port_0x64_value = 0;
    /* Reset the entire thread pool */
    memset(thread_pool, 0, sizeof(thread_pool));
    /* Thread pool slot 0 = the foreground process (matches fake_current_thread) */
    thread_pool[0].pid   = 1;
    thread_pool[0].pgid  = 1;
    thread_pool[0].sid   = 1;
    thread_pool[0].state = THREAD_RUNNING;
    thread_pool[0].pending_signals = 0;
    /* fake_current_thread defines the tty foreground target */
    fake_current_thread.pending_signals = 0;
    fake_current_thread.pgid = 1;
    fake_current_thread.sid  = 1;
    fake_current_thread.pid  = 1;
    memset(echoed_bytes, 0, sizeof(echoed_bytes));
    echoed_len = 0;
    tty_init();
    /* Use explicit pgid routing so SIGINT goes only to foreground group */
    tty_state.foreground_pgid = 1;
    tty_state.foreground_sid  = 1;
}

void tearDown(void) { /* nothing */ }

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Simulate a PS/2 scancode arriving: set the fake port value and call ISR */
static void simulate_keypress(uint8_t scancode) {
    fake_port_0x60_value = scancode;
    keyboard_isr(NULL);
}

/* Read one char from ring buffer without calling keyboard_read_char()
 * (which uses sti;hlt and would crash on the host).
 * Returns 0 if buffer is empty. */
static int tty_read_one_char(char *out) {
    return tty_read(out, 1, 0);
}

/* -----------------------------------------------------------------------
 * Scancode map tests (unshifted)
 * --------------------------------------------------------------------- */

void test_scancode_a_returns_lowercase_a(void) {
    TEST_ASSERT_EQUAL_CHAR('a', scancode_map[0x1E]);
}

void test_scancode_1_returns_digit(void) {
    TEST_ASSERT_EQUAL_CHAR('1', scancode_map[0x02]);
}

void test_scancode_enter_returns_newline(void) {
    TEST_ASSERT_EQUAL_CHAR('\n', scancode_map[0x1C]);
}

void test_scancode_backspace_returns_backspace(void) {
    TEST_ASSERT_EQUAL_CHAR('\b', scancode_map[0x0E]);
}

void test_scancode_space_returns_space(void) {
    TEST_ASSERT_EQUAL_CHAR(' ', scancode_map[0x39]);
}

void test_scancode_ignored_returns_zero(void) {
    TEST_ASSERT_EQUAL_CHAR(0, scancode_map[0x00]);
}

void test_scancode_shift_returns_zero(void) {
    TEST_ASSERT_EQUAL_CHAR(0, scancode_map[0x2A]);  /* Left Shift */
}

void test_scancode_escape_returns_zero(void) {
    TEST_ASSERT_EQUAL_CHAR(0, scancode_map[0x01]);  /* Escape */
}

/* -----------------------------------------------------------------------
 * Scancode map tests (shifted)
 * --------------------------------------------------------------------- */

void test_shifted_a_returns_uppercase_A(void) {
    TEST_ASSERT_EQUAL_CHAR('A', scancode_map_shifted[0x1E]);
}

void test_shifted_1_returns_bang(void) {
    TEST_ASSERT_EQUAL_CHAR('!', scancode_map_shifted[0x02]);
}

void test_shifted_minus_returns_underscore(void) {
    TEST_ASSERT_EQUAL_CHAR('_', scancode_map_shifted[0x0C]);
}

void test_shifted_equals_returns_plus(void) {
    TEST_ASSERT_EQUAL_CHAR('+', scancode_map_shifted[0x0D]);
}

void test_shifted_semicolon_returns_colon(void) {
    TEST_ASSERT_EQUAL_CHAR(':', scancode_map_shifted[0x27]);
}

void test_shifted_quote_returns_dquote(void) {
    TEST_ASSERT_EQUAL_CHAR('"', scancode_map_shifted[0x28]);
}

void test_shifted_backtick_returns_tilde(void) {
    TEST_ASSERT_EQUAL_CHAR('~', scancode_map_shifted[0x29]);
}

void test_shifted_backslash_returns_pipe(void) {
    TEST_ASSERT_EQUAL_CHAR('|', scancode_map_shifted[0x2B]);
}

void test_shifted_comma_returns_lt(void) {
    TEST_ASSERT_EQUAL_CHAR('<', scancode_map_shifted[0x33]);
}

/* -----------------------------------------------------------------------
 * TTY line discipline tests
 * --------------------------------------------------------------------- */

void test_canonical_backspace_shrinks_pending_line(void) {
    char out[4] = {0};

    simulate_keypress(0x1E);  /* a */
    simulate_keypress(0x30);  /* b */
    simulate_keypress(0x0E);  /* backspace */
    simulate_keypress(0x1C);  /* newline */

    TEST_ASSERT_EQUAL_INT(2, tty_read(out, sizeof(out), 1));
    TEST_ASSERT_EQUAL_CHAR('a', out[0]);
    TEST_ASSERT_EQUAL_CHAR('\n', out[1]);
    TEST_ASSERT_EQUAL_MEMORY("ab\b \b\n", echoed_bytes, 6);
}

void test_newline_finalizes_line_for_readers(void) {
    char out[4] = {0};

    simulate_keypress(0x1E);  /* a */
    TEST_ASSERT_EQUAL_INT(0, tty_read(out, sizeof(out), 0));

    simulate_keypress(0x1C);  /* newline */
    TEST_ASSERT_EQUAL_INT(2, tty_read(out, sizeof(out), 1));
    TEST_ASSERT_EQUAL_CHAR('a', out[0]);
    TEST_ASSERT_EQUAL_CHAR('\n', out[1]);
}

void test_raw_mode_delivers_literal_ctrl_c_and_ctrl_d(void) {
    char out = 0;
    tty_state.termios.c_lflag &= ~(TTY_LFLAG_ICANON | TTY_LFLAG_ECHO | TTY_LFLAG_ISIG);

    tty_keyboard_input(0x03);
    TEST_ASSERT_EQUAL_INT(1, tty_read_one_char(&out));
    TEST_ASSERT_EQUAL_HEX8(0x03, (uint8_t)out);

    tty_keyboard_input(0x04);
    TEST_ASSERT_EQUAL_INT(1, tty_read_one_char(&out));
    TEST_ASSERT_EQUAL_HEX8(0x04, (uint8_t)out);
}

void test_canonical_ctrl_c_does_not_enqueue_byte(void) {
    char out = 0;

    tty_keyboard_input(0x03);
    /* Signal must be delivered to the foreground process in thread_pool */
    TEST_ASSERT_EQUAL_UINT32(1u << 2, thread_pool[0].pending_signals);
    TEST_ASSERT_EQUAL_INT(0, tty_read_one_char(&out));
}

/* -----------------------------------------------------------------------
 * Process-group SIGINT routing tests (Phase 46 SIG-03 contract)
 * --------------------------------------------------------------------- */

/* VINTR delivers SIGINT only to tasks in the matching pgid */
void test_vintr_signals_foreground_group_only(void) {
    /* Background task in a different pgid */
    thread_pool[1].pid   = 2;
    thread_pool[1].pgid  = 2;   /* different group */
    thread_pool[1].sid   = 1;
    thread_pool[1].state = THREAD_RUNNABLE;
    thread_pool[1].pending_signals = 0;

    tty_keyboard_input(0x03);   /* Ctrl-C */

    /* Foreground group (pgid=1) should receive SIGINT */
    TEST_ASSERT_EQUAL_UINT32(1u << SIGINT, thread_pool[0].pending_signals);
    /* Background group (pgid=2) must NOT receive SIGINT */
    TEST_ASSERT_EQUAL_UINT32(0u, thread_pool[1].pending_signals);

    /* Cleanup */
    thread_pool[1].pid = 0;
    thread_pool[1].state = THREAD_DEAD;
}

/* Multiple foreground processes sharing the same pgid all receive SIGINT */
void test_vintr_signals_all_foreground_members(void) {
    /* Second foreground task in same group */
    thread_pool[1].pid   = 3;
    thread_pool[1].pgid  = 1;   /* same foreground group */
    thread_pool[1].sid   = 1;
    thread_pool[1].state = THREAD_RUNNABLE;
    thread_pool[1].pending_signals = 0;

    tty_keyboard_input(0x03);   /* Ctrl-C */

    TEST_ASSERT_EQUAL_UINT32(1u << SIGINT, thread_pool[0].pending_signals);
    TEST_ASSERT_EQUAL_UINT32(1u << SIGINT, thread_pool[1].pending_signals);

    /* Cleanup */
    thread_pool[1].pid = 0;
    thread_pool[1].state = THREAD_DEAD;
}

/* Canonical input buffer is cleared on Ctrl-C */
void test_vintr_clears_canonical_buffer(void) {
    /* Type some chars then Ctrl-C */
    tty_keyboard_input('a');
    tty_keyboard_input('b');
    /* After Ctrl-C: canonical buffer must be empty (no line finalized) */
    tty_keyboard_input(0x03);
    char out = 0;
    /* No data should be in the read buffer */
    TEST_ASSERT_EQUAL_INT(0, tty_read_one_char(&out));
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void) {
    UNITY_BEGIN();

    /* Scancode map — unshifted */
    RUN_TEST(test_scancode_a_returns_lowercase_a);
    RUN_TEST(test_scancode_1_returns_digit);
    RUN_TEST(test_scancode_enter_returns_newline);
    RUN_TEST(test_scancode_backspace_returns_backspace);
    RUN_TEST(test_scancode_space_returns_space);
    RUN_TEST(test_scancode_ignored_returns_zero);
    RUN_TEST(test_scancode_shift_returns_zero);
    RUN_TEST(test_scancode_escape_returns_zero);

    /* Scancode map — shifted */
    RUN_TEST(test_shifted_a_returns_uppercase_A);
    RUN_TEST(test_shifted_1_returns_bang);
    RUN_TEST(test_shifted_minus_returns_underscore);
    RUN_TEST(test_shifted_equals_returns_plus);
    RUN_TEST(test_shifted_semicolon_returns_colon);
    RUN_TEST(test_shifted_quote_returns_dquote);
    RUN_TEST(test_shifted_backtick_returns_tilde);
    RUN_TEST(test_shifted_backslash_returns_pipe);
    RUN_TEST(test_shifted_comma_returns_lt);

    /* TTY line discipline */
    RUN_TEST(test_canonical_backspace_shrinks_pending_line);
    RUN_TEST(test_newline_finalizes_line_for_readers);
    RUN_TEST(test_raw_mode_delivers_literal_ctrl_c_and_ctrl_d);
    RUN_TEST(test_canonical_ctrl_c_does_not_enqueue_byte);

    /* Process-group SIGINT routing (Phase 46 SIG-03) */
    RUN_TEST(test_vintr_signals_foreground_group_only);
    RUN_TEST(test_vintr_signals_all_foreground_members);
    RUN_TEST(test_vintr_clears_canonical_buffer);

    return UNITY_END();
}
