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

#include <miniOS/arch/x86_64/port.h>
#include <miniOS/arch/x86_64/spinlock.h>
#include <miniOS/drivers/console.h>
#include <miniOS/drivers/vt.h>
#include <miniOS/mm/vmm.h>

#ifdef CONSOLE_GOP
#include <miniOS/drivers/gop_font.h>

static uint64_t g_fb_virt;
static uint32_t g_fb_pitch;
static uint8_t  g_fb_red_pos;
static uint8_t  g_fb_green_pos;
static uint8_t  g_fb_blue_pos;

static uint32_t gop_palette[16];

/* Backing store: glyph index and attribute for every cell on screen.
 * Sized for the maximum GOP resolution (1280x800 / 8x16 = 160x50). */
#define GOP_CELL_MAX_COLS 160
#define GOP_CELL_MAX_ROWS  50
static uint16_t gop_cell_glyph[GOP_CELL_MAX_ROWS][GOP_CELL_MAX_COLS];
static uint8_t  gop_cell_attr [GOP_CELL_MAX_ROWS][GOP_CELL_MAX_COLS];

/* Cursor blink state */
static uint16_t gop_cursor_row;
static uint16_t gop_cursor_col;
static bool     gop_cursor_drawn;
static uint32_t gop_cursor_tick_ctr;
static bool     gop_cursor_blink_on = true;
#endif

static uint16_t vt_rows = SCREEN_ROWS;
static uint16_t vt_cols = SCREEN_COLS;

/* Protects all framebuffer and backing-store writes against concurrent access
 * from ISR context (vt_cursor_tick on another CPU) and kernel writers. */
static spinlock_t vt_fb_lock = SPINLOCK_INIT;

enum vt_parser_state {
    VT_STATE_NORMAL = 0,
    VT_STATE_ESC,
    VT_STATE_CSI,
};

typedef struct {
    uint8_t fg;
    uint8_t bg;
    bool bold;
    bool underline;
    bool reverse;
} vt_attr_state_t;

typedef struct {
    uint16_t row;
    uint16_t col;
    uint16_t saved_row;
    uint16_t saved_col;
    bool cursor_visible;
    enum vt_parser_state parser_state;
    vt_attr_state_t attr;
    int params[8];
    uint8_t param_count;
    bool param_in_progress;
} vt_state_t;

static vt_state_t vt_state;

static uint8_t vt_palette_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xaa}, {0x00, 0xaa, 0x00}, {0x00, 0xaa, 0xaa},
    {0xaa, 0x00, 0x00}, {0xaa, 0x00, 0xaa}, {0xaa, 0x55, 0x00}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xff}, {0x55, 0xff, 0x55}, {0x55, 0xff, 0xff},
    {0xff, 0x55, 0x55}, {0xff, 0x55, 0xff}, {0xff, 0xff, 0x55}, {0xff, 0xff, 0xff},
};

static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, 0x0C);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
}

static void serial_putchar(char c) {
    while (!(inb(COM1_PORT + COM_LSR_OFFSET) & COM_LSR_THRE));
    outb(COM1_PORT, (uint8_t)c);
}

static uint8_t *vt_vga_cell(uint16_t row, uint16_t col) {
    return (uint8_t*)VGA_BUFFER_VA + 2 * (row * SCREEN_COLS + col);
}

static void vt_reset_parser(void) {
    vt_state.parser_state = VT_STATE_NORMAL;
    vt_state.param_count = 0;
    vt_state.param_in_progress = false;
    for (uint8_t i = 0; i < 8; i++) {
        vt_state.params[i] = 0;
    }
}

static void vt_reset_attr(void) {
    vt_state.attr.fg = TEXT_LIGHT_GRAY;
    vt_state.attr.bg = TEXT_BLACK;
    vt_state.attr.bold = false;
    vt_state.attr.underline = false;
    vt_state.attr.reverse = false;
}

static uint8_t vt_attr_byte(void) {
    uint8_t fg = vt_state.attr.fg & 0x0f;
    uint8_t bg = vt_state.attr.bg & 0x0f;

    if (vt_state.attr.bold && fg < 8) {
        fg |= 0x08;
    }
    if (vt_state.attr.reverse) {
        uint8_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    return (uint8_t)((bg << 4) | fg);
}

static void vt_write_cell(uint16_t row, uint16_t col, uint16_t ch, uint8_t attr) {
#ifdef CONSOLE_GOP
    if (g_fb_virt) {
        if (row < GOP_CELL_MAX_ROWS && col < GOP_CELL_MAX_COLS) {
            gop_cell_glyph[row][col] = ch;
            gop_cell_attr [row][col] = attr;
        }
        uint32_t fg = gop_palette[attr & 0x0F];
        uint32_t bg = gop_palette[(attr >> 4) & 0x0F];
        uint16_t gi = (ch < GOP_N_GLYPHS) ? ch : 0;
        const uint8_t *glyph = vga_font_8x16[gi];
        for (int y = 0; y < GOP_FONT_H; y++) {
            uint32_t *line = (uint32_t *)((uint8_t *)g_fb_virt
                + ((uint32_t)row * GOP_FONT_H + (uint32_t)y) * g_fb_pitch)
                + (uint32_t)col * GOP_FONT_W;
            uint8_t bits = glyph[y];
            for (int x = 0; x < GOP_FONT_W; x++)
                line[x] = (bits & (0x80u >> x)) ? fg : bg;
        }
        return;
    }
#endif
    uint8_t *cell = vt_vga_cell(row, col);
    cell[0] = (uint8_t)ch;
    cell[1] = attr;
}

static void vt_clear_row_range(uint16_t row, uint16_t col_start, uint16_t col_end, uint8_t attr) {
#ifdef CONSOLE_GOP
    if (g_fb_virt) {
        /* Keep backing store in sync. */
        if (row < GOP_CELL_MAX_ROWS) {
            for (uint16_t c = col_start; c <= col_end && c < GOP_CELL_MAX_COLS; c++) {
                gop_cell_glyph[row][c] = ' ';
                gop_cell_attr [row][c] = attr;
            }
        }
        uint32_t bg = gop_palette[(attr >> 4) & 0x0F];
        uint32_t x0 = (uint32_t)col_start * GOP_FONT_W;
        uint32_t x1 = ((uint32_t)col_end + 1) * GOP_FONT_W;
        for (uint32_t y = 0; y < GOP_FONT_H; y++) {
            uint32_t *line = (uint32_t *)((uint8_t *)g_fb_virt
                + ((uint32_t)row * GOP_FONT_H + y) * g_fb_pitch);
            for (uint32_t x = x0; x < x1; x++)
                line[x] = bg;
        }
        return;
    }
#endif
    for (uint16_t col = col_start; col <= col_end; col++) {
        vt_write_cell(row, col, ' ', attr);
    }
}

#ifdef CONSOLE_GOP
/* Erase the cursor overlay by redrawing the cell from the backing store. */
static void gop_erase_cursor(void) {
    if (!gop_cursor_drawn) return;
    uint16_t r = gop_cursor_row, c = gop_cursor_col;
    if (r < GOP_CELL_MAX_ROWS && c < GOP_CELL_MAX_COLS)
        vt_write_cell(r, c, gop_cell_glyph[r][c], gop_cell_attr[r][c]);
    gop_cursor_drawn = false;
}

/* Draw a 2-scanline underline cursor at the current cursor position. */
static void gop_draw_cursor(void) {
    if (!vt_state.cursor_visible) return;
    uint16_t r = vt_state.row, c = vt_state.col;
    if (r >= vt_rows || c >= vt_cols) return;
    uint32_t color = gop_palette[TEXT_LIGHT_GRAY];
    for (int y = GOP_FONT_H - 2; y < GOP_FONT_H; y++) {
        uint32_t *line = (uint32_t *)((uint8_t *)g_fb_virt
            + ((uint32_t)r * GOP_FONT_H + (uint32_t)y) * g_fb_pitch)
            + (uint32_t)c * GOP_FONT_W;
        for (int x = 0; x < GOP_FONT_W; x++)
            line[x] = color;
    }
    gop_cursor_row   = r;
    gop_cursor_col   = c;
    gop_cursor_drawn = true;
}
#endif

static void vt_scroll_if_needed(void) {
    if (vt_state.row < vt_rows) {
        return;
    }

#ifdef CONSOLE_GOP
    if (g_fb_virt) {
        /* Remove cursor from framebuffer before copying pixels so the underline
         * does not appear one row above the new bottom after the scroll. */
        gop_erase_cursor();

        /* Forward 64-bit copy: src > dst, no overlap corruption. */
        uint64_t *dst64 = (uint64_t *)g_fb_virt;
        const uint64_t *src64 = (const uint64_t *)((const uint8_t *)g_fb_virt
            + (uint32_t)GOP_FONT_H * g_fb_pitch);
        uint32_t qwords = (uint32_t)(vt_rows - 1) * (uint32_t)GOP_FONT_H * g_fb_pitch / 8;
        for (uint32_t i = 0; i < qwords; i++)
            dst64[i] = src64[i];

        /* Shift backing store rows in step with the pixel copy. */
        for (uint16_t r = 0; r < (uint16_t)(vt_rows - 1); r++) {
            for (uint16_t c = 0; c < vt_cols && c < GOP_CELL_MAX_COLS; c++) {
                gop_cell_glyph[r][c] = gop_cell_glyph[r + 1][c];
                gop_cell_attr [r][c] = gop_cell_attr [r + 1][c];
            }
        }

        vt_clear_row_range((uint16_t)(vt_rows - 1), 0, (uint16_t)(vt_cols - 1), DEFAULT_TEXT_ATTR);
        vt_state.row = (uint16_t)(vt_rows - 1);
        return;
    }
#endif

    for (uint16_t row = 1; row < vt_rows; row++) {
        for (uint16_t col = 0; col < vt_cols; col++) {
            uint8_t *dst = vt_vga_cell(row - 1, col);
            uint8_t *src = vt_vga_cell(row, col);
            dst[0] = src[0];
            dst[1] = src[1];
        }
    }

    vt_clear_row_range((uint16_t)(vt_rows - 1), 0, (uint16_t)(vt_cols - 1), DEFAULT_TEXT_ATTR);
    vt_state.row = (uint16_t)(vt_rows - 1);
}

static void vt_update_cursor_hw(void) {
#ifdef CONSOLE_GOP
    if (g_fb_virt) {
        gop_erase_cursor();
        if (gop_cursor_blink_on)
            gop_draw_cursor();
        return;
    }
#endif
    uint16_t offset = (uint16_t)(vt_state.row * SCREEN_COLS + vt_state.col);

    if (!vt_state.cursor_visible) {
        outb(VGA_CURSOR_PORT_IDX, VGA_CUR_START_REG);
        outb(VGA_CURSOR_PORT_DATA, VGA_CUR_DISABLE);
        return;
    }

    outb(VGA_CURSOR_PORT_IDX, VGA_CUR_START_REG);
    outb(VGA_CURSOR_PORT_DATA, VGA_CUR_START_LINE);
    outb(VGA_CURSOR_PORT_IDX, VGA_CUR_END_REG);
    outb(VGA_CURSOR_PORT_DATA, VGA_CUR_END_LINE);
    outb(VGA_CURSOR_PORT_IDX, VGA_CUR_HIGH_REG);
    outb(VGA_CURSOR_PORT_DATA, offset >> 8);
    outb(VGA_CURSOR_PORT_IDX, VGA_CUR_LOW_REG);
    outb(VGA_CURSOR_PORT_DATA, offset & 0xff);
}

static void vt_clamp_cursor(void) {
    if (vt_state.row >= vt_rows) {
        vt_state.row = (uint16_t)(vt_rows - 1);
    }
    if (vt_state.col >= vt_cols) {
        vt_state.col = (uint16_t)(vt_cols - 1);
    }
}

static int vt_csi_param(uint8_t index, int default_value) {
    if (index >= vt_state.param_count) {
        return default_value;
    }
    if (!vt_state.param_in_progress && index == vt_state.param_count && vt_state.param_count == 0) {
        return default_value;
    }
    if (vt_state.params[index] == 0) {
        return default_value;
    }
    return vt_state.params[index];
}

static void vt_erase_in_display(int mode) {
    uint8_t attr = vt_attr_byte();

    switch (mode) {
        case 1:
            /* Start of screen through cursor (inclusive). */
            for (uint16_t row = 0; row < vt_state.row; row++) {
                vt_clear_row_range(row, 0, (uint16_t)(vt_cols - 1), attr);
            }
            vt_clear_row_range(vt_state.row, 0, vt_state.col, attr);
            break;
        case 2:
            for (uint16_t row = 0; row < vt_rows; row++) {
                vt_clear_row_range(row, 0, (uint16_t)(vt_cols - 1), DEFAULT_TEXT_ATTR);
            }
            vt_state.row = 0;
            vt_state.col = 0;
            break;
        case 0:
        default:
            /* Cursor through end of screen (inclusive) — the common case:
             * BusyBox's `clear` sends ESC[H ESC[J, i.e. mode 0 from (0,0). */
            vt_clear_row_range(vt_state.row, vt_state.col, (uint16_t)(vt_cols - 1), attr);
            for (uint16_t row = (uint16_t)(vt_state.row + 1); row < vt_rows; row++) {
                vt_clear_row_range(row, 0, (uint16_t)(vt_cols - 1), attr);
            }
            break;
    }
}

static void vt_erase_in_line(int mode) {
    uint8_t attr = vt_attr_byte();

    switch (mode) {
        case 1:
            vt_clear_row_range(vt_state.row, 0, vt_state.col, attr);
            break;
        case 2:
            vt_clear_row_range(vt_state.row, 0, (uint16_t)(vt_cols - 1), attr);
            break;
        case 0:
        default:
            vt_clear_row_range(vt_state.row, vt_state.col, (uint16_t)(vt_cols - 1), attr);
            break;
    }
}

/* VGA exposes only 16 colors. 256-color SGR requests are converted by mapping the
 * xterm palette entry to the nearest VGA RGB triplet using squared distance. */
static uint8_t vt_map_256_to_vga(int color) {
    if (color < 0) {
        return TEXT_LIGHT_GRAY;
    }
    if (color < 16) {
        return (uint8_t)color;
    }

    uint8_t rgb[3];
    if (color >= 16 && color <= 231) {
        static const uint8_t ramp[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
        int idx = color - 16;
        rgb[0] = ramp[idx / 36];
        rgb[1] = ramp[(idx / 6) % 6];
        rgb[2] = ramp[idx % 6];
    } else {
        uint8_t level = (uint8_t)(8 + (color - 232) * 10);
        rgb[0] = level;
        rgb[1] = level;
        rgb[2] = level;
    }

    uint32_t best_distance = 0xffffffffu;
    uint8_t best_index = TEXT_LIGHT_GRAY;
    for (uint8_t i = 0; i < 16; i++) {
        int dr = (int)rgb[0] - (int)vt_palette_rgb[i][0];
        int dg = (int)rgb[1] - (int)vt_palette_rgb[i][1];
        int db = (int)rgb[2] - (int)vt_palette_rgb[i][2];
        uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

static uint8_t vt_sgr_16_color(int code, bool background) {
    static const uint8_t normal[8] = {
        TEXT_BLACK, TEXT_RED, TEXT_GREEN, TEXT_BROWN,
        TEXT_BLUE, TEXT_MAGENTA, TEXT_CYAN, TEXT_LIGHT_GRAY
    };
    static const uint8_t bright[8] = {
        TEXT_DARK_GRAY, TEXT_LIGHT_RED, TEXT_LIGHT_GREEN, TEXT_YELLOW,
        TEXT_LIGHT_BLUE, TEXT_LIGHT_MAGENTA, TEXT_LIGHT_CYAN, TEXT_WHITE
    };

    if (code >= 30 && code <= 37) {
        return normal[code - 30];
    }
    if (code >= 40 && code <= 47) {
        return normal[code - 40];
    }
    if (code >= 90 && code <= 97) {
        return bright[code - 90];
    }
    if (code >= 100 && code <= 107) {
        return bright[code - 100];
    }

    return background ? TEXT_BLACK : TEXT_LIGHT_GRAY;
}

static void vt_apply_sgr(void) {
    uint8_t count = vt_state.param_count;

    if (count == 0 && !vt_state.param_in_progress) {
        vt_reset_attr();
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        int code = vt_state.params[i];

        switch (code) {
            case 0:
                vt_reset_attr();
                break;
            case 1:
                vt_state.attr.bold = true;
                break;
            case 4:
                vt_state.attr.underline = true;
                break;
            case 7:
                vt_state.attr.reverse = true;
                break;
            case 22:
                vt_state.attr.bold = false;
                break;
            case 24:
                vt_state.attr.underline = false;
                break;
            case 27:
                vt_state.attr.reverse = false;
                break;
            case 39:
                vt_state.attr.fg = TEXT_LIGHT_GRAY;
                break;
            case 49:
                vt_state.attr.bg = TEXT_BLACK;
                break;
            default:
                if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
                    vt_state.attr.fg = vt_sgr_16_color(code, false);
                } else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
                    vt_state.attr.bg = vt_sgr_16_color(code, true);
                } else if ((code == 38 || code == 48) && (i + 2) < count && vt_state.params[i + 1] == 5) {
                    uint8_t mapped = vt_map_256_to_vga(vt_state.params[i + 2]);
                    if (code == 38) {
                        vt_state.attr.fg = mapped;
                    } else {
                        vt_state.attr.bg = mapped;
                    }
                    i += 2;
                }
                break;
        }
    }
}

static void vt_put_printable(uint16_t ch) {
    vt_write_cell(vt_state.row, vt_state.col, ch, vt_attr_byte());
    vt_state.col++;
    if (vt_state.col >= vt_cols) {
        vt_state.col = 0;
        vt_state.row++;
        vt_scroll_if_needed();
    }
}

/* Translate Latin-1 (ISO-8859-1) byte to nearest CP437 glyph for VGA output.
 * Covers German umlauts and the other symbols the DE keyboard layout emits.
 * Unknown high bytes pass through unchanged — VGA renders them as CP437. */
static uint8_t latin1_to_cp437(uint8_t c) {
    switch (c) {
    case 0xC4: return 0x8E; /* Ä */
    case 0xD6: return 0x99; /* Ö */
    case 0xDC: return 0x9A; /* Ü */
    case 0xE4: return 0x84; /* ä */
    case 0xF6: return 0x94; /* ö */
    case 0xFC: return 0x81; /* ü */
    case 0xDF: return 0xE1; /* ß */
    case 0xA7: return 0x15; /* § */
    case 0xB0: return 0xF8; /* ° */
    case 0xB2: return 0xFD; /* ² */
    case 0xB5: return 0xE6; /* µ */
    default:   return c;
    }
}

#ifdef CONSOLE_GOP
/* Binary search in the sorted gop_cp_map[].  Returns 0xFFFF when not found. */
static uint16_t gop_cp_lookup(uint32_t cp) {
    if (cp > 0xFFFF) return 0xFFFF;
    uint16_t key = (uint16_t)cp;
    int lo = 0, hi = (int)GOP_CP_MAP_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (gop_cp_map[mid].cp == key) return gop_cp_map[mid].glyph;
        if (gop_cp_map[mid].cp  < key) lo = mid + 1;
        else                            hi = mid - 1;
    }
    return 0xFFFF;
}
#endif

/* Emit one Unicode codepoint to the current cursor position.
 * GOP mode: codepoint -> glyph index via Unicode table.
 * VGA mode: Latin-1 range only, mapped to CP437. */
static void vt_put_codepoint(uint32_t cp) {
#ifdef CONSOLE_GOP
    if (g_fb_virt) {
        uint16_t g = gop_cp_lookup(cp);
        if (g != 0xFFFF) vt_put_printable(g);
        return;
    }
#endif
    if (cp >= 0x20 && cp <= 0x7E)
        vt_put_printable((uint16_t)cp);
    else if (cp >= 0x80 && cp <= 0xFF)
        vt_put_printable(latin1_to_cp437((uint8_t)cp));
}

/* UTF-8 decode state.  When utf8_rem > 0 a multi-byte sequence is in progress.
 * utf8_buf holds the raw bytes received so far (used for Latin-1 fallback on
 * invalid sequences); utf8_cp accumulates the decoded codepoint. */
static int      utf8_rem;
static uint32_t utf8_cp;
static uint8_t  utf8_buf[4];
static int      utf8_buf_len;

/* Emit each buffered raw byte as a codepoint (fallback for invalid UTF-8). */
static void utf8_flush_raw(void) {
    for (int i = 0; i < utf8_buf_len; i++)
        vt_put_codepoint(utf8_buf[i]);
    utf8_rem     = 0;
    utf8_buf_len = 0;
}

/* Render one decoded Unicode codepoint via the active backend. */
static void utf8_emit_codepoint(uint32_t cp) {
    vt_put_codepoint(cp);
}

static void vt_handle_normal_char(char c) {
    uint8_t uc = (uint8_t)c;

    /* ---- UTF-8 continuation byte ---- */
    if (utf8_rem > 0) {
        if ((uc & 0xC0) == 0x80) {
            utf8_buf[utf8_buf_len++] = uc;
            utf8_cp = (utf8_cp << 6) | (uc & 0x3F);
            if (--utf8_rem == 0) {
                utf8_emit_codepoint(utf8_cp);
                utf8_buf_len = 0;
            }
            return;
        }
        /* Invalid continuation: emit buffered bytes as raw Latin-1, then
         * fall through to process the current byte as a fresh sequence. */
        utf8_flush_raw();
    }

    /* ---- Fresh byte (no sequence in progress) ---- */
    if (uc >= 0x20 && uc <= 0x7E) {
        vt_put_printable(uc);
    } else if ((uc & 0xE0) == 0xC0) {   /* 2-byte lead: U+0080..U+07FF */
        utf8_rem     = 1;
        utf8_cp      = uc & 0x1F;
        utf8_buf[0]  = uc;
        utf8_buf_len = 1;
    } else if ((uc & 0xF0) == 0xE0) {   /* 3-byte lead: U+0800..U+FFFF */
        utf8_rem     = 2;
        utf8_cp      = uc & 0x0F;
        utf8_buf[0]  = uc;
        utf8_buf_len = 1;
    } else if ((uc & 0xF8) == 0xF0) {   /* 4-byte lead: U+10000..U+10FFFF */
        utf8_rem     = 3;
        utf8_cp      = uc & 0x07;
        utf8_buf[0]  = uc;
        utf8_buf_len = 1;
    } else if (uc >= 0x80) {             /* bare continuation or 0xFE/0xFF: treat as codepoint */
        vt_put_codepoint(uc);
    } else {
        /* ASCII control characters */
        if (c == '\n') {
            vt_state.row++;
            vt_state.col = 0;
            vt_scroll_if_needed();
        } else if (c == '\r') {
            vt_state.col = 0;
        } else if (c == '\b') {
            if (vt_state.col > 0 || vt_state.row > 0) {
                if (vt_state.col == 0) {
                    vt_state.row--;
                    vt_state.col = (uint16_t)(vt_cols - 1);
                } else {
                    vt_state.col--;
                }
                vt_write_cell(vt_state.row, vt_state.col, ' ', DEFAULT_TEXT_ATTR);
            }
        }
    }
}

void vt_handle_csi(char final) {
    int first = vt_csi_param(0, 0);
    int second = vt_csi_param(1, 0);

    switch (final) {
        case 'A':
            if (first == 0) {
                first = 1;
            }
            vt_state.row = (first > vt_state.row) ? 0 : (uint16_t)(vt_state.row - first);
            break;
        case 'B':
            if (first == 0) {
                first = 1;
            }
            vt_state.row = (uint16_t)(vt_state.row + first);
            vt_clamp_cursor();
            break;
        case 'C':
            if (first == 0) {
                first = 1;
            }
            vt_state.col = (uint16_t)(vt_state.col + first);
            vt_clamp_cursor();
            break;
        case 'D':
            if (first == 0) {
                first = 1;
            }
            vt_state.col = (first > vt_state.col) ? 0 : (uint16_t)(vt_state.col - first);
            break;
        case 'H':
            if (first == 0) {
                first = 1;
            }
            if (second == 0) {
                second = 1;
            }
            vt_state.row = (uint16_t)(first - 1);
            vt_state.col = (uint16_t)(second - 1);
            vt_clamp_cursor();
            break;
        case 'J':
            vt_erase_in_display(first);
            break;
        case 'K':
            vt_erase_in_line(first);
            break;
        case 's':
            vt_state.saved_row = vt_state.row;
            vt_state.saved_col = vt_state.col;
            break;
        case 'u':
            vt_state.row = vt_state.saved_row;
            vt_state.col = vt_state.saved_col;
            vt_clamp_cursor();
            break;
        case 'm':
            vt_apply_sgr();
            break;
        default:
            break;
    }

    vt_reset_parser();
}

void vt_init(void) {
    serial_init();
    vt_reset_attr();
    vt_state.row = 0;
    vt_state.col = 0;
    vt_state.saved_row = 0;
    vt_state.saved_col = 0;
    vt_state.cursor_visible = true;
    vt_reset_parser();
    vt_clear();
    vt_update_cursor_hw();
}

void vt_clear(void) {
    for (uint16_t row = 0; row < vt_rows; row++) {
        vt_clear_row_range(row, 0, (uint16_t)(vt_cols - 1), DEFAULT_TEXT_ATTR);
    }
    vt_state.row = 0;
    vt_state.col = 0;
    vt_update_cursor_hw();
}

void vt_write_byte(char c) {
    serial_putchar(c);
    if (c == '\n') {
        serial_putchar('\r');
    }

    unsigned long flags;
    spinlock_irqsave(&vt_fb_lock, &flags);

    switch (vt_state.parser_state) {
        case VT_STATE_NORMAL:
            if (c == 0x1b) {
                vt_reset_parser();
                vt_state.parser_state = VT_STATE_ESC;
            } else {
                vt_handle_normal_char(c);
            }
            break;
        case VT_STATE_ESC:
            if (c == '[') {
                vt_state.parser_state = VT_STATE_CSI;
            } else {
                vt_reset_parser();
            }
            break;
        case VT_STATE_CSI:
            if (c >= '0' && c <= '9') {
                if (vt_state.param_count >= 8) {
                    vt_reset_parser();
                    break;
                }
                vt_state.param_in_progress = true;
                vt_state.params[vt_state.param_count] =
                    vt_state.params[vt_state.param_count] * 10 + (c - '0');
            } else if (c == ';') {
                if (vt_state.param_count < 7) {
                    vt_state.param_count++;
                } else {
                    vt_reset_parser();
                }
                vt_state.param_in_progress = false;
            } else if (c >= '@' && c <= '~') {
                if (vt_state.param_in_progress || vt_state.param_count > 0) {
                    vt_state.param_count++;
                }
                vt_handle_csi(c);
            } else {
                vt_reset_parser();
            }
            break;
    }

    vt_update_cursor_hw();
    spinlock_irqrestore(&vt_fb_lock, flags);
}

void vt_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        vt_write_byte(buf[i]);
    }
}

void vt_set_cursor_visible(bool visible) {
    vt_state.cursor_visible = visible;
    vt_update_cursor_hw();
}

void vt_get_winsize(uint16_t *rows, uint16_t *cols) {
    if (rows != NULL) {
        *rows = vt_rows;
    }
    if (cols != NULL) {
        *cols = vt_cols;
    }
}

uint8_t vt_current_attr(void) {
    return vt_attr_byte();
}

#ifdef CONSOLE_GOP
void vt_init_gop(uint64_t fb_virt, uint32_t width, uint32_t height,
                 uint32_t pitch, uint8_t red_pos, uint8_t green_pos,
                 uint8_t blue_pos) {
    g_fb_pitch     = pitch;
    g_fb_red_pos   = red_pos;
    g_fb_green_pos = green_pos;
    g_fb_blue_pos  = blue_pos;

    vt_rows = (uint16_t)(height / GOP_FONT_H);
    vt_cols = (uint16_t)(width  / GOP_FONT_W);

    if (vt_rows > GOP_CELL_MAX_ROWS) vt_rows = GOP_CELL_MAX_ROWS;
    if (vt_cols > GOP_CELL_MAX_COLS) vt_cols = GOP_CELL_MAX_COLS;

    for (int i = 0; i < 16; i++) {
        gop_palette[i] = ((uint32_t)vt_palette_rgb[i][0] << red_pos)
                       | ((uint32_t)vt_palette_rgb[i][1] << green_pos)
                       | ((uint32_t)vt_palette_rgb[i][2] << blue_pos);
    }

    gop_cursor_blink_on  = true;
    gop_cursor_drawn     = false;
    gop_cursor_tick_ctr  = 0;

    g_fb_virt = fb_virt;   /* activate GOP path before vt_init clears the screen */
    vt_init();
}

/* Called from the LAPIC timer ISR (100 Hz) on the BSP only.
 * Toggles cursor blink every 10 ticks (100 ms). */
void vt_cursor_tick(void) {
    if (!g_fb_virt || !vt_state.cursor_visible) return;
    if (++gop_cursor_tick_ctr < 10) return;
    gop_cursor_tick_ctr = 0;
    gop_cursor_blink_on = !gop_cursor_blink_on;
    unsigned long flags;
    spinlock_irqsave(&vt_fb_lock, &flags);
    if (gop_cursor_blink_on)
        gop_draw_cursor();
    else
        gop_erase_cursor();
    spinlock_irqrestore(&vt_fb_lock, flags);
}
#endif
