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
 * @file vt.h
 * @defgroup vt VT/ANSI Terminal Core
 * @brief ANSI/VT100 escape sequence parser and VGA-backed terminal renderer.
 *
 * The VT core parses ANSI escape sequences (CSI cursor movement, SGR color,
 * erase commands) and renders output to the VGA 80x25 text-mode framebuffer.
 * It implements a software cursor, 256-color SGR mapped to the nearest VGA
 * 16-color palette entry, and hardware cursor sync via the VGA CRT registers.
 * @{
 */

#ifndef _MINIOS_DRIVERS_VT_H_
#define _MINIOS_DRIVERS_VT_H_

#include <miniOS/types.h>

/**
 * vt_init() - Initialise the VT terminal state.
 *
 * @brief Clears the VGA framebuffer, resets cursor position to (0,0), sets default
 * attribute (light gray on black), and initialises the ANSI escape parser
 * state machine. Called once during kernel boot before any console output.
 */
void vt_init(void);

/**
 * vt_clear() - Clear the terminal screen and reset cursor to (0,0).
 *
 * @brief Fills all 80x25 VGA cells with space characters using the current default
 * attribute, resets the cursor position to row 0 column 0, and updates the
 * hardware cursor via VGA CRT registers.
 */
void vt_clear(void);

/**
 * vt_write_byte() - Process one output byte through the VT state machine.
 * @param c Byte to process.
 *
 * @brief If inside an ANSI escape sequence, feeds @c to the CSI parser and applies
 * the completed command (cursor movement, erase, SGR color). Otherwise:
 * - ESC (0x1B): begins escape sequence.
 * - '\\n': advances row, scrolls if at bottom.
 * - '\\r': resets column to 0.
 * - '\\b': moves cursor left one cell (backspace).
 * - Other printable bytes: writes to the VGA cell at the current cursor
 *   position with the current attribute, then advances the cursor.
 */
void vt_write_byte(char c);

/**
 * vt_write() - Write a buffer of bytes through the VT state machine.
 * @param buf Pointer to the bytes to write.
 * @param len Number of bytes in @buf.
 *
 * @brief Calls vt_write_byte() for each byte in [buf, buf+len). Efficient for
 * bulk output from tty_write() or printk().
 */
void vt_write(const char *buf, size_t len);

/**
 * vt_set_cursor_visible() - Show or hide the hardware VGA text cursor.
 * @param visible true to enable the underline hardware cursor, false to hide it.
 *
 * @brief Writes to VGA CRT registers VGA_CUR_START_REG (index 0x0A): sets
 * VGA_CUR_DISABLE bit to hide, clears it and sets scanline range to show.
 */
void vt_set_cursor_visible(bool visible);

/**
 * vt_get_winsize() - Query the terminal dimensions.
 * @param rows Output; set to SCREEN_ROWS (25).
 * @param cols Output; set to SCREEN_COLS (80).
 *
 * @brief Used by tty_ioctl TIOCGWINSZ to fill struct minios_winsize.ws_row/ws_col.
 */
void vt_get_winsize(uint16_t *rows, uint16_t *cols);

/**
 * vt_current_attr() - Return the current VGA text attribute byte.
 *
 * @brief The attribute byte encodes foreground color (bits 3:0), background color
 * (bits 6:4), and blink (bit 7) as set by the most recent SGR sequence.
 *
 * @return Current VGA attribute byte (e.g. 0x07 = light gray on black).
 */
uint8_t vt_current_attr(void);

/**
 * vt_init_gop() - Switch the VT backend to a GOP/VBE linear framebuffer.
 * @param fb_virt   Virtual address of the mapped framebuffer.
 * @param width     Framebuffer width in pixels.
 * @param height    Framebuffer height in pixels.
 * @param pitch     Bytes per scanline.
 * @param red_pos   Bit position of the red channel in a 32bpp pixel.
 * @param green_pos Bit position of the green channel.
 * @param blue_pos  Bit position of the blue channel.
 *
 * @brief Precomputes a 16-entry 32bpp colour palette, updates vt_rows/vt_cols
 * to fb_height/GOP_FONT_H and fb_width/GOP_FONT_W, and calls vt_init() to
 * clear the framebuffer.  After this call all vt_write_*() output is rendered
 * as pixel glyphs; the VGA CRT cursor is suppressed.
 * Only available when the kernel is compiled with -DCONSOLE_GOP.
 */
#ifdef CONSOLE_GOP
void vt_init_gop(uint64_t fb_virt, uint32_t width, uint32_t height,
                 uint32_t pitch, uint8_t red_pos, uint8_t green_pos,
                 uint8_t blue_pos);

/**
 * vt_cursor_tick() - Drive GOP cursor blink from the LAPIC timer ISR.
 *
 * @brief Must be called at ~100 Hz on the BSP only. Toggles the cursor
 * underline every 50 ticks (500 ms). No-op when not in GOP mode or
 * when the cursor is hidden. Safe to call from ISR context.
 */
void vt_cursor_tick(void);
#endif

/** @} */

#endif
