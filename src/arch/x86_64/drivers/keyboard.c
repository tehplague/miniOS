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

#include <miniOS/drivers/keyboard.h>
#include <miniOS/drivers/tty.h>
#include <miniOS/arch/x86_64/apic.h>
#include <miniOS/arch/x86_64/irq.h>
#include <miniOS/arch/x86_64/port.h>
#include <miniOS/io.h>

#ifdef KEYBOARD_LAYOUT_DE

/* PS/2 set-1 scancode → character table: German QWERTZ layout.
 * Non-ASCII characters use Latin-1 (ISO-8859-1) byte values. */
static const char scancode_map[128] = {
    [0x00] = 0,              /* unused */
    [0x01] = 0,              /* Escape */
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = (char)0xDF,     /* ß */
    [0x0D] = '\'',           /* ´ (acute — treated as plain quote) */
    [0x0E] = '\b',           /* Backspace */
    [0x0F] = '\t',           /* Tab */
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'z',            /* QWERTZ: Z is here, not Y */
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = (char)0xFC,     /* ü */
    [0x1B] = '+',
    [0x1C] = '\n',           /* Enter */
    [0x1D] = 0,              /* Left Ctrl */
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = (char)0xF6,     /* ö */
    [0x28] = (char)0xE4,     /* ä */
    [0x29] = '^',
    [0x2A] = 0,              /* Left Shift */
    [0x2B] = '#',
    [0x2C] = 'y',            /* QWERTZ: Y is here, not Z */
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '-',
    [0x36] = 0,              /* Right Shift */
    [0x37] = 0,              /* Keypad * */
    [0x38] = 0,              /* Left Alt */
    [0x39] = ' ',            /* Space */
    [0x56] = '<',            /* ISO extra key (left of Y on 102-key DE board) */
    /* 0x3A-0x7F (except 0x56): function keys, etc. — all 0 (ignored) */
};

/* Shifted German QWERTZ — uppercase letters and shifted symbols. */
static const char scancode_map_shifted[128] = {
    [0x00] = 0,              /* unused */
    [0x01] = 0,              /* Escape */
    [0x02] = '!',
    [0x03] = '"',            /* Shift+2 */
    [0x04] = (char)0xA7,     /* § (section sign) */
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '&',
    [0x08] = '/',            /* Shift+7 */
    [0x09] = '(',            /* Shift+8 */
    [0x0A] = ')',            /* Shift+9 */
    [0x0B] = '=',            /* Shift+0 */
    [0x0C] = '?',            /* Shift+ß */
    [0x0D] = '`',            /* Shift+´ */
    [0x0E] = '\b',
    [0x0F] = 0,
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Z',            /* QWERTZ */
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = (char)0xDC,     /* Ü */
    [0x1B] = '*',
    [0x1C] = '\n',
    [0x1D] = 0,
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = (char)0xD6,     /* Ö */
    [0x28] = (char)0xC4,     /* Ä */
    [0x29] = (char)0xB0,     /* ° (degree sign) */
    [0x2A] = 0,              /* Left Shift */
    [0x2B] = '\'',           /* Shift+# */
    [0x2C] = 'Y',            /* QWERTZ */
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = ';',            /* Shift+, */
    [0x34] = ':',            /* Shift+. */
    [0x35] = '_',            /* Shift+- */
    [0x36] = 0,              /* Right Shift */
    [0x37] = 0,              /* Keypad * */
    [0x38] = 0,              /* Left Alt */
    [0x39] = ' ',
    [0x56] = '>',            /* Shift+ISO extra key */
    /* 0x3A-0x7F (except 0x56): function keys, etc. — all 0 (ignored) */
};

/* AltGr layer: Right Alt (E0 0x38) held.
 * Only keys that produce a distinct character are listed; all others are 0.
 * € (AltGr+e, scancode 0x12) is not representable in Latin-1 — handled as
 * a special case in keyboard_isr that injects its UTF-8 bytes directly. */
static const char scancode_map_altgr[128] = {
    [0x03] = (char)0xB2,  /* ² (AltGr+2) */
    [0x04] = (char)0xB3,  /* ³ (AltGr+3) */
    [0x08] = '{',         /* AltGr+7 */
    [0x09] = '[',         /* AltGr+8 */
    [0x0A] = ']',         /* AltGr+9 */
    [0x0B] = '}',         /* AltGr+0 */
    [0x0C] = '\\',        /* AltGr+ß → backslash */
    [0x10] = '@',         /* AltGr+q */
    [0x1B] = '~',         /* AltGr++ */
    [0x32] = (char)0xB5,  /* µ (AltGr+m) */
    [0x56] = '|',         /* AltGr+< (ISO 102nd key) */
};

#else  /* KEYBOARD_LAYOUT_DE not set — default US QWERTY */

/* PS/2 set-1 scancode → ASCII table.
 * Index = scancode (0-127), value = ASCII char (0 = ignore). */
static const char scancode_map[128] = {
    [0x00] = 0,    /* unused */
    [0x01] = 0,    /* Escape */
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b', /* Backspace */
    [0x0F] = '\t', /* Tab */
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n', /* Enter */
    [0x1D] = 0,    /* Left Ctrl */
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = 0,    /* backtick */
    [0x2A] = 0,    /* Left Shift */
    [0x2B] = '\\', /* backslash */
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = 0,    /* Right Shift */
    [0x37] = 0,    /* Keypad * */
    [0x38] = 0,    /* Left Alt */
    [0x39] = ' ',  /* Space */
    /* 0x3A-0x7F: function keys, etc. — all 0 (ignored) */
};

/* PS/2 set-1 scancode → ASCII table for shifted (Shift held) state.
 * US keyboard layout — uppercase letters and shifted symbols.
 * Index = scancode (0-127), value = shifted ASCII char (0 = ignore). */
static const char scancode_map_shifted[128] = {
    [0x00] = 0,    /* unused */
    [0x01] = 0,    /* Escape */
    [0x02] = '!',  /* Shift+1 */
    [0x03] = '@',  /* Shift+2 */
    [0x04] = '#',  /* Shift+3 */
    [0x05] = '$',  /* Shift+4 */
    [0x06] = '%',  /* Shift+5 */
    [0x07] = '^',  /* Shift+6 */
    [0x08] = '&',  /* Shift+7 */
    [0x09] = '*',  /* Shift+8 */
    [0x0A] = '(',  /* Shift+9 */
    [0x0B] = ')',  /* Shift+0 */
    [0x0C] = '_',  /* Shift+- */
    [0x0D] = '+',  /* Shift+= */
    [0x0E] = '\b', /* Backspace (same) */
    [0x0F] = 0,    /* Tab (ignore shifted) */
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',  /* Shift+[ */
    [0x1B] = '}',  /* Shift+] */
    [0x1C] = '\n', /* Enter (same) */
    [0x1D] = 0,    /* Left Ctrl */
    [0x1E] = 'A',
    [0x1F] = 'S',
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',  /* Shift+; */
    [0x28] = '"',  /* Shift+' */
    [0x29] = '~',  /* Shift+` */
    [0x2A] = 0,    /* Left Shift */
    [0x2B] = '|',  /* Shift+\ */
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',  /* Shift+, */
    [0x34] = '>',  /* Shift+. */
    [0x35] = '?',  /* Shift+/ */
    [0x36] = 0,    /* Right Shift */
    [0x37] = 0,    /* Keypad * */
    [0x38] = 0,    /* Left Alt */
    [0x39] = ' ',  /* Space (same) */
    /* 0x3A-0x7F: function keys, etc. — all 0 (ignored) */
};

#endif /* KEYBOARD_LAYOUT_DE */

/* Shift state: 1 = Left or Right Shift currently held */
static volatile uint8_t kb_shift = 0;
/* Ctrl state: 1 = Left or Right Ctrl currently held */
static volatile uint8_t kb_ctrl = 0;

/* E0-prefix state: 1 = previous byte was 0xE0 (extended scancode follows) */
static volatile uint8_t kb_e0 = 0;

#ifdef KEYBOARD_LAYOUT_DE
/* AltGr state: 1 = Right Alt (E0 0x38) currently held */
static volatile uint8_t kb_altgr = 0;
#endif

/* -----------------------------------------------------------------------
 * IRQ1 handler — called from irq_dispatch after context save
 * NOTE: NO printk here — serial_putchar busy-loops in ISR context and
 *       the console offset global is not reentrant-safe.
 * ----------------------------------------------------------------------- */
static void keyboard_isr(void *frame) {
    (void)frame;
    uint8_t sc = inb(0x60);  /* read scancode from PS/2 data port */

    /* Handle E0-prefix extended scancodes (arrow keys, etc.) */
    if (sc == 0xE0) { kb_e0 = 1; return; }
    if (kb_e0) {
        kb_e0 = 0;
#ifdef KEYBOARD_LAYOUT_DE
        if (sc == 0x38) { kb_altgr = 1; return; }  /* AltGr press  (E0 0x38) */
        if (sc == 0xB8) { kb_altgr = 0; return; }  /* AltGr release (E0 0xB8) */
#endif
        if (sc & 0x80) return;   /* extended key release — ignore */
        const char *seq = NULL;
        switch (sc) {
            case 0x48: seq = "\x1b[A"; break;  /* Up    */
            case 0x50: seq = "\x1b[B"; break;  /* Down  */
            case 0x4D: seq = "\x1b[C"; break;  /* Right */
            case 0x4B: seq = "\x1b[D"; break;  /* Left  */
        }
        if (seq)
            tty_inject_escape(seq, 3);
        return;
    }

    /* Handle modifier key press and release */
    if (sc == 0xAA) { kb_shift = 0; return; }  /* Left Shift release */
    if (sc == 0xB6) { kb_shift = 0; return; }  /* Right Shift release */
    if (sc == 0x9D) { kb_ctrl = 0; return; }   /* Left Ctrl release */
    if (sc & 0x80)  { return; }                /* other key-release — ignore */
    if (sc == 0x2A) { kb_shift = 1; return; }  /* Left Shift press */
    if (sc == 0x36) { kb_shift = 1; return; }  /* Right Shift press */
    if (sc == 0x1D) { kb_ctrl = 1; return; }   /* Left Ctrl press */

    if (sc >= 128) return;
    char c;
#ifdef KEYBOARD_LAYOUT_DE
    if (kb_altgr) {
        if (sc == 0x12) {
            /* € has no glyph in standard CP437 VGA font — suppress output */
            return;
        }
        c = scancode_map_altgr[sc];
    } else
#endif
    {
        c = kb_shift ? scancode_map_shifted[sc] : scancode_map[sc];
    }
    if (c == 0) return;
    if (kb_ctrl && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (kb_ctrl && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);
    if ((uint8_t)c >= 0x80) {
        /* Encode Latin-1 as 2-byte UTF-8 so the VT decoder handles it correctly. */
        tty_keyboard_input((char)(0xC0 | ((uint8_t)c >> 6)));
        tty_keyboard_input((char)(0x80 | ((uint8_t)c & 0x3F)));
    } else {
        tty_keyboard_input(c);
    }
}

/* -----------------------------------------------------------------------
 * keyboard_init — register IRQ1 handler and unmask IRQ1 in IOAPIC
 * ----------------------------------------------------------------------- */
void keyboard_init(void) {
    tty_init();

    /* Flush PS/2 output buffer: stale byte blocks new IRQs */
    while (inb(0x64) & 0x01)
        (void)inb(0x60);

    /* Write 8042 CCB:
     *   bit 0 = 1: keyboard interrupt enable
     *   bit 4 = 0: keyboard clock enabled (not disabled)
     *   bit 6 = 1: translate scancode set 2 -> set 1 (required)
     * CCB = 0x41 */
    while (inb(0x64) & 0x02) {}
    outb(0x64, 0x60);
    while (inb(0x64) & 0x02) {}
    outb(0x60, 0x41);

    irq_set_handler(1, keyboard_isr);
    ioapic_unmask_irq(1);
    printk("Keyboard: PS/2 driver initialised\n");
}

/* -----------------------------------------------------------------------
 * keyboard_read_char — compatibility shim over tty_read().
 * ----------------------------------------------------------------------- */
char keyboard_read_char(void) {
    char c = 0;
    while (tty_read(&c, 1, 0) == 0) {}
    return c;
}
