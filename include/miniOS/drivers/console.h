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
 * @file console.h
 * @defgroup console VGA Text Console (low-level)
 * @brief Direct VGA text-mode framebuffer driver used before VT initialises.
 *
 * Provides simple VGA 80x25 text-mode output: clear screen, single-character
 * and string output with automatic scrolling, and hardware cursor control.
 * Also mirrors output to COM1 (0x3F8) for serial logging. This driver is
 * used during early boot; after vt_init() the VT core takes over terminal I/O.
 * @{
 */

#ifndef _MINIOS_DRIVERS_CONSOLE_H_
#define _MINIOS_DRIVERS_CONSOLE_H_

/* Text attributes. */
#define TEXT_STATIC             0
#define TEXT_BLINKING           1

/* Foreground and background colors. */
#define TEXT_BLACK              0 /* 0000 */
#define TEXT_BLUE               1 /* 0001 */
#define TEXT_GREEN              2 /* 0010 */
#define TEXT_CYAN               3 /* 0011 */
#define TEXT_RED                4 /* 0100 */
#define TEXT_MAGENTA            5 /* 0101 */
#define TEXT_BROWN              6 /* 0110 */
#define TEXT_LIGHT_GRAY         7 /* 0111 */

/* Foreground-only colors. */
#define TEXT_DARK_GRAY          8 /* 1000 */
#define TEXT_LIGHT_BLUE         9 /* 1001 */
#define TEXT_LIGHT_GREEN       10 /* 1010 */
#define TEXT_LIGHT_CYAN        11 /* 1011 */
#define TEXT_LIGHT_RED         12 /* 1100 */
#define TEXT_LIGHT_MAGENTA     13 /* 1101 */
#define TEXT_YELLOW            14 /* 1110 */
#define TEXT_WHITE             15 /* 1111 */

// Console text attributes
#define TEXT_ATTR(textcolor, bgcolor, blinking) ((blinking << 7) | (bgcolor << 4) | textcolor)

/* Light gray text on a black background. */
#define DEFAULT_TEXT_ATTR  (TEXT_ATTR(TEXT_LIGHT_GRAY, TEXT_BLACK, TEXT_STATIC))

/* ------------------------------------------------------------------ */
/*  Screen geometry                                                     */
/* ------------------------------------------------------------------ */
#define SCREEN_ROWS     25   /* VGA text mode rows */
#define SCREEN_COLS     80   /* VGA text mode columns */

/* ------------------------------------------------------------------ */
/*  VGA CRT controller I/O ports (for hardware cursor)                 */
/* ------------------------------------------------------------------ */
#define VGA_CURSOR_PORT_IDX   0x3D4  /* CRT index register */
#define VGA_CURSOR_PORT_DATA  0x3D5  /* CRT data register */
#define VGA_CUR_START_REG     0x0A   /* cursor start scanline register */
#define VGA_CUR_HIGH_REG      0x0E   /* cursor high byte register */
#define VGA_CUR_LOW_REG       0x0F   /* cursor low byte register */
#define VGA_CUR_DISABLE       (1 << 5)  /* bit 5 of start register disables cursor */
#define VGA_CUR_END_REG       0x0B      /* cursor end scanline register */
#define VGA_CUR_START_LINE    14        /* underline style: start scanline */
#define VGA_CUR_END_LINE      15        /* underline style: end scanline */

/* ------------------------------------------------------------------ */
/*  COM1 serial port (16550 UART)                                      */
/* ------------------------------------------------------------------ */
#define COM1_PORT         0x3F8  /* COM1 base I/O port */
#define COM_LSR_OFFSET    5      /* Line Status Register offset from COM1_PORT */
#define COM_LSR_THRE      0x20   /* Transmitter Holding Register Empty (TX ready) */

/**
 * console_init() - Initialise the VGA console and COM1 serial port.
 *
 * @brief Sets cursor position to (0,0), initialises the 16550 UART at COM1_PORT
 * (0x3F8) to 9600 baud 8N1, clears the VGA framebuffer using vt_init(), and
 * enables the hardware cursor. Called once at boot before any printk output.
 */
void console_init(void);

/**
 * console_clear() - Clear the VGA screen and reset cursor to row 0, column 0.
 *
 * @brief Fills all 80x25 cells with DEFAULT_TEXT_ATTR space characters and calls
 * vt_clear() to reset the VT parser state and hardware cursor.
 */
void console_clear(void);

/**
 * console_putchar() - Output one character to the VGA console and COM1.
 * @param c Character to write. Routed through vt_write_byte() for ANSI processing
 *          and simultaneously written to COM1 for serial capture.
 *
 * @brief Handles newline, carriage return, backspace, and scrolling via the VT core.
 */
void console_putchar(char c);

/**
 * console_puts() - Output a null-terminated string to the VGA console.
 * @param s Null-terminated string to write. Each byte is passed to console_putchar().
 *
 * @brief Iterates over @s until the null terminator, calling console_putchar() for
 * each character.
 */
void console_puts(char *s);

/**
 * console_show_cursor() - Enable the VGA hardware underline cursor.
 *
 * @brief Writes scanline range VGA_CUR_START_LINE..VGA_CUR_END_LINE to CRT registers
 * 0x0A and 0x0B, making an underline cursor visible at the current position.
 */
void console_show_cursor(void);

/**
 * console_hide_cursor() - Disable the VGA hardware cursor.
 *
 * @brief Sets VGA_CUR_DISABLE (bit 5) in CRT register 0x0A, making the hardware
 * cursor invisible. Used when the terminal is in an intermediate state.
 */
void console_hide_cursor(void);

/** @} */

#endif
