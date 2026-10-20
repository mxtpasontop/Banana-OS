/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "crash.h"
#include "terminal.h"
#include "utf8.h"
#include "types.h"
#include "fb.h"
#include "gfx.h"
#include "serial.h"
#include "tty.h"
#include "keyboard.h"
#include "kstring.h"
#include "timer.h"
#include "task.h"
#include "kheap.h"
#include "splash.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((uint16_t*)0xB8000)

/* Framebuffer console max grid (for 800x600 @ 8x8 => 100x75) */
#define FB_MAX_COLS 100
#define FB_MAX_ROWS 75

static size_t   term_row;
static size_t   term_col;
static uint8_t  term_color;
static uint16_t *term_buf;
static size_t   term_reserved_bottom = 0;
static terminal_mode_t term_mode = TERMINAL_MODE_VGA_TEXT;
static int term_fb_scale = 1; /* smaller font by default */
static size_t term_fb_cols = VGA_WIDTH;
static size_t term_fb_rows = VGA_HEIGHT;

/* framebuffer virtual terminals (count shared via terminal.h so other
 * modules can size their own per-window state to match) */
#define VT_MAX TERMINAL_VT_MAX
static uint32_t vt_chars[VT_MAX][FB_MAX_ROWS][FB_MAX_COLS];    /* Unicode characters (code points) */
/* a UTF-8 character still arriving, per vt */
static uint8_t  vt_u8_need[VT_MAX];
static uint32_t vt_u8_cp[VT_MAX];
static uint8_t vt_colors[VT_MAX][FB_MAX_ROWS][FB_MAX_COLS];
static size_t  vt_row[VT_MAX];
static size_t  vt_col[VT_MAX];
static uint8_t vt_color[VT_MAX];
static uint8_t vt_used[VT_MAX] = {1, 0, 0, 0}; /* vt0 reserved */
static int     vt_active = 0;
/* a GUI terminal window's own text grid (0 = the whole screen's grid) */
static size_t  vt_cols[VT_MAX];
static size_t  vt_rows[VT_MAX];

/* Scrollback: the lines that scrolled off the top of each vt, a ring of
 * SB_LINES (made when the first one goes), and how far back its view is
 * (0: the live text; new output brings it back) */
#define SB_LINES 2000
typedef struct { uint32_t* ch; uint8_t* co; uint32_t head, count; } scrollback_t;
static scrollback_t vt_sb[VT_MAX];
static int          vt_back[VT_MAX];
static uint8_t      vt_cshape[VT_MAX];        /* the cursor's shape: TERM_CURSOR_* (ESC [ n SP q) */

static uint32_t vt_pushed[VT_MAX];                  /* lines that left the top, ever: what moved the text up */

static void sb_push(int vt, const uint32_t* ch, const uint8_t* co, size_t w) {
    scrollback_t* s = &vt_sb[vt];
    vt_pushed[vt]++;
    if (!s->ch) {
        s->ch = (uint32_t*)kmalloc(SB_LINES * FB_MAX_COLS * sizeof(uint32_t));
        s->co = (uint8_t*)kmalloc(SB_LINES * FB_MAX_COLS);
        if (!s->ch || !s->co) { kfree(s->ch); kfree(s->co); s->ch = NULL; s->co = NULL; return; }
        s->head = s->count = 0;
    }
    uint32_t at = s->head;
    for (size_t x = 0; x < FB_MAX_COLS; x++) {
        s->ch[at * FB_MAX_COLS + x] = x < w ? ch[x] : ' ';
        s->co[at * FB_MAX_COLS + x] = x < w ? co[x] : co[0];
    }
    s->head = (s->head + 1) % SB_LINES;
    if (s->count < SB_LINES) s->count++;
    if (vt_back[vt]) vt_back[vt]++;                 /* (a view scrolled back stays on the same text) */
}

int terminal_vt_history(int vt) { return vt >= 0 && vt < VT_MAX ? (int)vt_sb[vt].count : 0; }

void terminal_vt_history_line(int vt, int back, const uint32_t** ch, const uint8_t** co) {
    *ch = NULL; *co = NULL;
    if (vt < 0 || vt >= VT_MAX || back < 1 || (uint32_t)back > vt_sb[vt].count) return;
    uint32_t i = (vt_sb[vt].head + SB_LINES - (uint32_t)back) % SB_LINES;
    *ch = vt_sb[vt].ch + i * FB_MAX_COLS;
    *co = vt_sb[vt].co + i * FB_MAX_COLS;
}

uint32_t terminal_vt_lines_scrolled(int vt) { return vt >= 0 && vt < VT_MAX ? vt_pushed[vt] : 0; }

void terminal_vt_clear_history(int vt) {
    if (vt < 0 || vt >= VT_MAX) return;
    vt_sb[vt].head = vt_sb[vt].count = 0;
    vt_back[vt] = 0;
}

/* bumped on every change to any vt (text, colours, cursor): the GUI
 * compares it to decide whether terminal windows need repainting */
static volatile uint32_t g_term_gen = 1;

uint32_t terminal_generation(void) { return g_term_gen; }

static uint32_t vga_color_rgb(uint8_t c) {
    /* simple VGA palette */
    static const uint32_t pal[16] = {
        0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
        0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
        0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
        0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,
    };
    return pal[c & 0x0F];
}

static size_t terminal_fb_cols(void) {
    return term_fb_cols;
}

static size_t terminal_fb_rows(void) {
    return term_fb_rows;
}

static size_t terminal_width(void) {
    if (vt_active != 0 && vt_cols[vt_active] && term_mode != TERMINAL_MODE_VGA_TEXT) return vt_cols[vt_active];
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER || term_mode == TERMINAL_MODE_SUSPENDED) return terminal_fb_cols();
    return VGA_WIDTH;
}

static size_t terminal_height(void) {
    if (vt_active != 0 && vt_rows[vt_active] && term_mode != TERMINAL_MODE_VGA_TEXT) return vt_rows[vt_active];
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER || term_mode == TERMINAL_MODE_SUSPENDED) return terminal_fb_rows();
    return VGA_HEIGHT;
}

static void fb_recompute_grid(void) {
    if (!fb_available()) {
        term_fb_cols = VGA_WIDTH;
        term_fb_rows = VGA_HEIGHT;
        return;
    }

    const fb_info_t* fi = fb_info();
    if (!fi || fi->width == 0 || fi->height == 0) {
        term_fb_cols = VGA_WIDTH;
        term_fb_rows = VGA_HEIGHT;
        return;
    }

    int scale = term_fb_scale;
    if (scale < 1) scale = 1;

    uint32_t cols = fi->width  / (uint32_t)(8u * (uint32_t)scale);
    uint32_t rows = fi->height / (uint32_t)(8u * (uint32_t)scale);
    if (cols < 10) cols = 10;
    if (rows < 5)  rows = 5;
    if (cols > FB_MAX_COLS) cols = FB_MAX_COLS;
    if (rows > FB_MAX_ROWS) rows = FB_MAX_ROWS;

    term_fb_cols = (size_t)cols;
    term_fb_rows = (size_t)rows;

    if (term_col >= term_fb_cols) term_col = term_fb_cols - 1;
    if (term_row >= term_fb_rows) term_row = term_fb_rows - 1;
}

/* ── framebuffer console: deferred rendering ─────────────────────────
 * The framebuffer console (the shell before `startx`) always shows vt0.
 * Output only updates the text grid and marks rows dirty; the pixels are
 * brought up to date by terminal_flush(), which runs at most every ~25 ms
 * while output streams, whenever the system goes idle (gui_poll() and
 * the blocking waits call it), and immediately for interactive cursor
 * moves. Consecutive scrolls are coalesced into one block move of the
 * pixels. Painting every character (and moving the whole screen for
 * every line) as it was printed made long output crawl - `cat` of an
 * 8000-line file took ~28 s. */

#define CONSOLE_VT 0
#define FLUSH_INTERVAL_MS 25

static uint8_t  g_dirty[FB_MAX_ROWS];
static int      g_any_dirty = 1;
static uint32_t g_pending_scroll = 0;
static uint32_t g_last_flush_ms = 0;

/* where the underline cursor is currently painted */
static int    g_ov_valid = 0;
static size_t g_ov_row, g_ov_col;

static int fb_console_on(void) {
    return term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available();
}

/* is the text being written right now what the console shows? */
static int fb_console_target(void) {
    return fb_console_on() && vt_active == CONSOLE_VT;
}

static void mark_dirty(size_t row) {
    if (row < FB_MAX_ROWS) {
        g_dirty[row] = 1;
        g_any_dirty = 1;
    }
}

static void mark_all_dirty(void) {
    for (size_t r = 0; r < FB_MAX_ROWS; r++) g_dirty[r] = 1;
    g_any_dirty = 1;
}

static void fb_draw_cell(size_t row, size_t col) {
    if (row >= terminal_fb_rows() || col >= terminal_fb_cols()) return;
    uint8_t color = vt_colors[CONSOLE_VT][row][col];
    uint32_t cp = vt_chars[CONSOLE_VT][row][col];
    int v = (int)row - vt_back[CONSOLE_VT];        /* (scrolled back: the lines above come from the scrollback) */
    if (v < 0) {
        const uint32_t* hc;
        const uint8_t* hco;
        terminal_vt_history_line(CONSOLE_VT, -v, &hc, &hco);
        cp = hc ? hc[col] : ' ';
        color = hco ? hco[col] : vt_color[CONSOLE_VT];
    } else if (vt_back[CONSOLE_VT]) {
        cp = vt_chars[CONSOLE_VT][v][col];
        color = vt_colors[CONSOLE_VT][v][col];
    }
    int scale = term_fb_scale < 1 ? 1 : term_fb_scale;
    gfx_draw_char_scaled_cp((int)col * 8 * scale, (int)row * 8 * scale, scale, cp,
                         vga_color_rgb(color & 0x0F), vga_color_rgb((color >> 4) & 0x0F));
}

static void fb_draw_cursor_overlay(size_t row, size_t col) {
    if (row >= terminal_fb_rows() || col >= terminal_fb_cols()) return;
    if (vt_back[CONSOLE_VT] || !terminal_vt_cursor_shown(CONSOLE_VT)) { g_ov_valid = 0; return; }   /* (scrolled back, or a program hid it) */
    g_ov_valid = 1;
    g_ov_row = row;
    g_ov_col = col;

    uint8_t color = vt_colors[CONSOLE_VT][row][col];
    int scale = term_fb_scale < 1 ? 1 : term_fb_scale;
    int x = (int)col * 8 * scale, y = (int)row * 8 * scale, h = (scale > 1) ? 2 : 1;
    uint32_t fg = vga_color_rgb(color & 0x0F);
    switch (vt_cshape[CONSOLE_VT]) {
    case TERM_CURSOR_BLOCK:                             /* the character in reverse */
        gfx_draw_char_scaled_cp(x, y, scale, vt_chars[CONSOLE_VT][row][col], vga_color_rgb((color >> 4) & 0x0F), fg);
        break;
    case TERM_CURSOR_BAR: gfx_fill_rect(x, y, h, 8 * scale, fg); break;
    default: gfx_fill_rect(x, y + (8 * scale - h), 8 * scale, h, fg); break;
    }
}

static void console_cursor(size_t* row, size_t* col) {
    /* the live copy is term_row/col while vt0 is the active vt */
    *row = (vt_active == CONSOLE_VT) ? term_row : vt_row[CONSOLE_VT];
    *col = (vt_active == CONSOLE_VT) ? term_col : vt_col[CONSOLE_VT];
}

void terminal_flush(void) {
    if(crash_preview_active() || crash_in_progress())return;
    if (!fb_console_on()) return;
    if (splash_active()) return;            /* the boot screen is up: the text waits in the grid */
    size_t cr, cc;
    console_cursor(&cr, &cc);
    int cursor_moved = !g_ov_valid || g_ov_row != cr || g_ov_col != cc;
    if (!g_any_dirty && !g_pending_scroll && !cursor_moved) return;

    size_t rows = terminal_fb_rows();

    if (g_pending_scroll) {
        /* All the scrolls since the last flush: every row is painted again
         * from the text grid. (Moving the pixels up instead reads the whole
         * screen back from video memory - cheap in QEMU, but very slow on
         * real graphics cards and VirtualBox, and the more pixels the
         * slower: boot took ages at a resolution above 800x600.) */
        mark_all_dirty();
        g_pending_scroll = 0;
        g_ov_valid = 0;
    }
    if (g_ov_valid && (g_ov_row != cr || g_ov_col != cc)) mark_dirty(g_ov_row);   /* erase old underline */

    size_t cols = terminal_fb_cols();
    for (size_t r = 0; r < rows; r++) {
        if (!g_dirty[r]) continue;
        g_dirty[r] = 0;
        for (size_t c = 0; c < cols; c++) fb_draw_cell(r, c);
    }
    g_any_dirty = 0;
    fb_draw_cursor_overlay(cr, cc);
    fb_present();
    g_last_flush_ms = timer_ms();
}

static void maybe_flush(void) {
    if ((uint32_t)(timer_ms() - g_last_flush_ms) >= FLUSH_INTERVAL_MS) terminal_flush();
}

static void fb_refresh_cursor(size_t old_row, size_t old_col) {
    /* interactive cursor movement: paint right away */
    (void)old_row; (void)old_col;
    if (fb_console_target()) terminal_flush();
}

static void fb_redraw_all(void) {
    if (!fb_console_on()) return;
    g_ov_valid = 0;
    g_pending_scroll = 0;
    mark_all_dirty();
    terminal_flush();
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}

static void terminal_update_cursor(void) {
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void terminal_cursor_enable(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E); /* start scanline */
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F); /* end scanline */
}

static inline uint8_t vga_entry_color(uint8_t fg, uint8_t bg) {
    return fg | bg << 4;
}

/* VGA text mode draws code page 437: the Latin-1 characters it has */
static const unsigned char LATIN1_CP437[96] = {
    0xFF,0xAD,0x9B,0x9C,'?', 0x9D,'|', 0x15,'"', 'c', 0xA6,0xAE,0xAA,'-', 'r', '-',
    0xF8,0xF1,0xFD,'3', '\'','?', 0x14,0xFA,',', '1', 0xA7,0xAF,0xAC,0xAB,'?', 0xA8,
    'A', 'A', 'A', 'A', 0x8E,0x8F,0x92,0x80,'E', 0x90,'E', 'E', 'I', 'I', 'I', 'I',
    'D', 0xA5,'O', 'O', 'O', 'O', 0x99,'x', 'O', 'U', 'U', 'U', 0x9A,'Y', '?', 0xE1,
    0x85,0xA0,0x83,'a', 0x84,0x86,0x91,0x87,0x8A,0x82,0x88,0x89,0x8D,0xA1,0x8C,0x8B,
    '?', 0xA4,0x95,0xA2,0x93,'o', 0x94,0xF6,'o', 0x97,0xA3,0x96,0x81,'y', '?', 0x98,
};
/* ... and beyond Latin-1, what code page 437 has: box drawing, shades,
 * arrows, card suits, Greek and maths */
static const struct { uint16_t cp; uint8_t g; } UNI_CP437[] = {
    { 0x2500, 0xC4 }, { 0x2502, 0xB3 }, { 0x250C, 0xDA }, { 0x2510, 0xBF }, { 0x2514, 0xC0 }, { 0x2518, 0xD9 },
    { 0x251C, 0xC3 }, { 0x2524, 0xB4 }, { 0x252C, 0xC2 }, { 0x2534, 0xC1 }, { 0x253C, 0xC5 },
    { 0x2550, 0xCD }, { 0x2551, 0xBA }, { 0x2554, 0xC9 }, { 0x2557, 0xBB }, { 0x255A, 0xC8 }, { 0x255D, 0xBC },
    { 0x2560, 0xCC }, { 0x2563, 0xB9 }, { 0x2566, 0xCB }, { 0x2569, 0xCA }, { 0x256C, 0xCE },
    { 0x2580, 0xDF }, { 0x2584, 0xDC }, { 0x2588, 0xDB }, { 0x258C, 0xDD }, { 0x2590, 0xDE },
    { 0x2591, 0xB0 }, { 0x2592, 0xB1 }, { 0x2593, 0xB2 }, { 0x25A0, 0xFE },
    { 0x2190, 0x1B }, { 0x2191, 0x18 }, { 0x2192, 0x1A }, { 0x2193, 0x19 }, { 0x2194, 0x1D }, { 0x2195, 0x12 },
    { 0x25B2, 0x1E }, { 0x25BA, 0x10 }, { 0x25BC, 0x1F }, { 0x25C4, 0x11 }, { 0x25CB, 0x09 }, { 0x25D8, 0x08 },
    { 0x263A, 0x01 }, { 0x263B, 0x02 }, { 0x2665, 0x03 }, { 0x2666, 0x04 }, { 0x2663, 0x05 }, { 0x2660, 0x06 },
    { 0x2022, 0x07 }, { 0x266A, 0x0D }, { 0x266B, 0x0E }, { 0x263C, 0x0F }, { 0x2302, 0x7F },
    { 0x03B1, 0xE0 }, { 0x0393, 0xE2 }, { 0x03C0, 0xE3 }, { 0x03A3, 0xE4 }, { 0x03C3, 0xE5 }, { 0x03C4, 0xE7 },
    { 0x03A6, 0xE8 }, { 0x0398, 0xE9 }, { 0x03A9, 0xEA }, { 0x03B4, 0xEB }, { 0x221E, 0xEC }, { 0x03C6, 0xED },
    { 0x03B5, 0xEE }, { 0x2229, 0xEF }, { 0x2261, 0xF0 }, { 0x2265, 0xF2 }, { 0x2264, 0xF3 }, { 0x2248, 0xF7 },
    { 0x2219, 0xF9 }, { 0x221A, 0xFB }, { 0x207F, 0xFC }, { 0x20A7, 0x9E }, { 0x0192, 0x9F },
};

static char vga_glyph(uint32_t cp) {
    if (cp < 0x80) return (char)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (char)LATIN1_CP437[cp - 0xA0];
    for (unsigned i = 0; i < sizeof(UNI_CP437) / sizeof(UNI_CP437[0]); i++)
        if (UNI_CP437[i].cp == cp) return (char)UNI_CP437[i].g;
    return '?';
}

char terminal_cp437(uint32_t cp) { return vga_glyph(cp); }

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

static size_t terminal_usable_height(void) {
    size_t h = terminal_height();
    if (term_reserved_bottom >= h) return 0;
    return h - term_reserved_bottom;
}

void terminal_init(void) {
    term_row   = 0;
    term_col   = 0;
    term_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    term_buf   = VGA_MEMORY;
    term_reserved_bottom = 0;
    terminal_cursor_enable();

    /* If framebuffer exists, default shell output to framebuffer (pre-startx). */
    if (fb_available()) {
        term_mode = TERMINAL_MODE_FRAMEBUFFER;
        term_fb_scale = 1; /* smaller font */
    }
    vt_active = 0;
    vt_row[0] = 0;
    vt_col[0] = 0;
    vt_color[0] = term_color;
    fb_recompute_grid();
    terminal_clear();
}

static int capturing(void);
static void putentry_cp(uint32_t cp, uint8_t fg, uint8_t bg, size_t row, size_t col);

void terminal_clear(void) {
    if (capturing()) return;
    tty_clear_screen((int)vt_active);
    g_term_gen++;
    size_t h = terminal_usable_height();
    size_t w = terminal_width();

    if (term_mode == TERMINAL_MODE_SUSPENDED) {
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                vt_chars[vt_active][y][x] = ' ';
                vt_colors[vt_active][y][x] = vt_color[vt_active];
            }
        }
        vt_row[vt_active] = 0;
        vt_col[vt_active] = 0;
        term_row = 0;
        term_col = 0;
        return;
    }

    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                vt_chars[vt_active][y][x] = ' ';
                vt_colors[vt_active][y][x] = vt_color[vt_active];
            }
        }
        term_row = 0;
        term_col = 0;
        vt_row[vt_active] = 0;
        vt_col[vt_active] = 0;
        fb_redraw_all();
        return;
    }

    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            term_buf[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
    }
    term_row = 0;
    term_col = 0;
    terminal_update_cursor();
}

void terminal_setcolor(uint8_t fg, uint8_t bg) {
    term_color = vga_entry_color(fg, bg);
    vt_color[vt_active] = term_color;
}

static void terminal_scroll(void) {
    size_t h = terminal_usable_height();
    if (h == 0) return;
    size_t w = terminal_width();

    if ((term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) || term_mode == TERMINAL_MODE_SUSPENDED) {
        sb_push(vt_active, vt_chars[vt_active][0], vt_colors[vt_active][0], w);     /* (the top line: into the scrollback) */
        memmove(&vt_chars[vt_active][0][0], &vt_chars[vt_active][1][0], (h - 1) * FB_MAX_COLS * sizeof(vt_chars[0][0][0]));
        memmove(&vt_colors[vt_active][0][0], &vt_colors[vt_active][1][0], (h - 1) * FB_MAX_COLS);
        for (size_t x = 0; x < w; x++) {
            vt_chars[vt_active][h - 1][x] = ' ';
            vt_colors[vt_active][h - 1][x] = vt_color[vt_active];
        }
        term_row = h - 1;
        term_col = 0;
        vt_row[vt_active] = term_row;
        vt_col[vt_active] = term_col;
        if (fb_console_target()) {
            /* dirty flags follow their text up; the pixels move at the
             * next flush, together with any further scrolls */
            memmove(g_dirty, g_dirty + 1, h - 1);
            g_dirty[h - 1] = 1;
            g_any_dirty = 1;
            g_pending_scroll++;
        }
        return;
    }

    for (size_t y = 1; y < h; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            term_buf[(y-1) * VGA_WIDTH + x] = term_buf[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        term_buf[(h-1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = h - 1;
    term_col = 0;
}

static int g_serial_mirror = 1;

void terminal_serial_mirror(int on) {
    g_serial_mirror = on;
}

size_t terminal_get_width(void) {
    return terminal_width();
}

size_t terminal_get_height(void) {
    return terminal_usable_height();
}

/* Output capture for shell redirection (`cmd > file`): while a task
 * captures, what it prints goes into a buffer instead of the screen. */
static struct { int active, pid; char* buf; uint32_t len, cap; } g_cap;

int terminal_capture_start(void) {
    if (g_cap.active) return 0;
    g_cap.cap = 4096;
    g_cap.buf = (char*)kmalloc(g_cap.cap);
    if (!g_cap.buf) return 0;
    g_cap.len = 0;
    g_cap.pid = task_current_pid();
    g_cap.active = 1;
    return 1;
}

char* terminal_capture_stop(uint32_t* len) {
    char* b = g_cap.buf;
    *len = g_cap.len;
    g_cap.active = 0;
    g_cap.buf = NULL;
    return b;
}

static int capturing(void) {
    return g_cap.active && g_cap.pid == task_current_pid();
}

static void capture_putc(char c) {
    if (g_cap.len + 1 >= g_cap.cap) {
        if (g_cap.cap >= (32u << 20)) return;     /* the file size limit */
        char* nb = (char*)kmalloc(g_cap.cap * 2);
        if (!nb) return;
        memcpy(nb, g_cap.buf, g_cap.len);
        kfree(g_cap.buf);
        g_cap.buf = nb;
        g_cap.cap *= 2;
    }
    g_cap.buf[g_cap.len++] = c;
}

/* ── ANSI / VT100 escape sequences (the xterm subset programs use) ──────
 * ESC [ ... m colours and attributes (16 colours, 256 and 24-bit ones to
 * the nearest of them; bold as the bright colour, reverse video), cursor
 * moves and positioning, erasing the screen / a line / the scrollback,
 * inserting and deleting lines and characters, scrolling, saving the
 * cursor, hiding it, reporting where it is; ESC ] 0 / 2 ; title BEL sets
 * the window's title. The raw bytes still reach the serial port and SSH
 * clients, which interpret them themselves. */
enum { ES_TEXT, ES_ESC, ES_CSI, ES_OSC, ES_OSC_ESC };
#define ES_PARAMS 16
static uint8_t  es_state[VT_MAX];
static uint8_t  es_priv[VT_MAX];                   /* '?' after ESC [ */
static uint8_t  es_np[VT_MAX];
static uint16_t es_p[VT_MAX][ES_PARAMS];
static uint8_t  es_bold[VT_MAX], es_rev[VT_MAX];
static uint8_t  es_fg[VT_MAX] = {7, 7, 7, 7, 7, 7, 7}, es_bg[VT_MAX];
static size_t   es_save_r[VT_MAX], es_save_c[VT_MAX];
static uint8_t  es_cursor_off[VT_MAX];
static char     es_osc[VT_MAX][80];
static uint8_t  es_osc_n[VT_MAX];
static char     vt_title[VT_MAX][64];
static uint8_t  es_inter[VT_MAX];                  /* an intermediate byte (ESC [ 5 SP q: ' ') */
static uint32_t vt_bell_ms[VT_MAX], vt_bells[VT_MAX];   /* the last BEL's time (0: none), how many */

/* a vt given out again: none of the last owner's codes, colours or title */
static void es_reset(int vt) {
    es_state[vt] = ES_TEXT;
    es_priv[vt] = es_np[vt] = es_bold[vt] = es_rev[vt] = es_cursor_off[vt] = es_osc_n[vt] = 0;
    es_fg[vt] = 7;
    es_bg[vt] = 0;
    vt_title[vt][0] = 0;
    vt_bell_ms[vt] = vt_bells[vt] = 0;
    vt_cshape[vt] = es_inter[vt] = 0;
    vt_u8_need[vt] = 0;
}

/* ANSI's colour order (black red green yellow blue magenta cyan white) in VGA's */
static const uint8_t ANSI_VGA[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint32_t vga_color_rgb(uint8_t c);

/* the 16 colours as xterm defines them, in VGA's order: what 256-colour and
 * 24-bit ones are matched against (pure red is bright red there) */
static const uint32_t XTERM16[16] = {
    0x000000, 0x0000EE, 0x00CD00, 0x00CDCD, 0xCD0000, 0xCD00CD, 0xCDCD00, 0xE5E5E5,
    0x7F7F7F, 0x5C5CFF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF,
};

/* the nearest of the 16 colours to an RGB one */
static uint8_t nearest16(int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        uint32_t p = XTERM16[i];
        int dr = (int)((p >> 16) & 0xFF) - r, dg = (int)((p >> 8) & 0xFF) - g, db = (int)(p & 0xFF) - b;
        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

/* xterm's 256 colours: the 16, a 6x6x6 cube, 24 greys */
static uint8_t color256(int n) {
    if (n < 8) return ANSI_VGA[n];
    if (n < 16) return (uint8_t)(ANSI_VGA[n - 8] | 8);
    if (n < 232) {
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return nearest16(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
    }
    int g = 8 + (n - 232) * 10;
    return nearest16(g, g, g);
}

static void es_apply_color(int v) {
    uint8_t fg = es_fg[v], bg = es_bg[v];
    if (es_bold[v] && fg < 8) fg |= 8;
    if (es_rev[v]) { uint8_t t = fg; fg = bg; bg = t; }
    term_color = (uint8_t)(fg | bg << 4);
    vt_color[v] = term_color;
}

static void es_sgr(int v) {
    /* (the colours as they are now: the shell sets them its own way too) */
    if (!es_rev[v]) { es_fg[v] = vt_color[v] & 0x0F; es_bg[v] = vt_color[v] >> 4; if (es_bold[v]) es_fg[v] &= 7; }
    if (es_np[v] == 0) es_np[v] = 1, es_p[v][0] = 0;
    for (int i = 0; i < es_np[v]; i++) {
        int p = es_p[v][i];
        if (p == 0) { es_bold[v] = es_rev[v] = 0; es_fg[v] = 7; es_bg[v] = 0; }
        else if (p == 1) es_bold[v] = 1;
        else if (p == 22) es_bold[v] = 0;
        else if (p == 7) es_rev[v] = 1;
        else if (p == 27) es_rev[v] = 0;
        else if (p >= 30 && p <= 37) es_fg[v] = ANSI_VGA[p - 30];
        else if (p == 39) es_fg[v] = 7;
        else if (p >= 40 && p <= 47) es_bg[v] = ANSI_VGA[p - 40];
        else if (p == 49) es_bg[v] = 0;
        else if (p >= 90 && p <= 97) es_fg[v] = (uint8_t)(ANSI_VGA[p - 90] | 8);
        else if (p >= 100 && p <= 107) es_bg[v] = (uint8_t)(ANSI_VGA[p - 100] | 8);
        else if ((p == 38 || p == 48) && i + 2 < es_np[v] && es_p[v][i + 1] == 5) {
            uint8_t c = color256(es_p[v][i + 2]);
            if (p == 38) es_fg[v] = c; else es_bg[v] = c;
            i += 2;
        } else if ((p == 38 || p == 48) && i + 4 < es_np[v] && es_p[v][i + 1] == 2) {
            uint8_t c = nearest16(es_p[v][i + 2], es_p[v][i + 3], es_p[v][i + 4]);
            if (p == 38) es_fg[v] = c; else es_bg[v] = c;
            i += 4;
        }
    }
    es_apply_color(v);
}

/* the text grid of the active vt: the framebuffer's (or a window's), or VGA text mode's */
static int es_grid(void) {
    return (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) || term_mode == TERMINAL_MODE_SUSPENDED;
}

static void es_cell_set(size_t r, size_t c, uint32_t cp, uint8_t color) {
    if (es_grid()) {
        if (r >= FB_MAX_ROWS || c >= FB_MAX_COLS) return;
        vt_chars[vt_active][r][c] = cp;
        vt_colors[vt_active][r][c] = color;
        if (vt_active == CONSOLE_VT) mark_dirty(r);
    } else {
        if (r >= VGA_HEIGHT || c >= VGA_WIDTH) return;
        term_buf[r * VGA_WIDTH + c] = vga_entry(vga_glyph(cp), color);
    }
}

/* the cell at (rs, cs) copied to (rd, cd), as it is */
static void es_cell_copy(size_t rd, size_t cd, size_t rs, size_t cs) {
    if (es_grid()) {
        if (rd >= FB_MAX_ROWS || cd >= FB_MAX_COLS || rs >= FB_MAX_ROWS || cs >= FB_MAX_COLS) return;
        vt_chars[vt_active][rd][cd] = vt_chars[vt_active][rs][cs];
        vt_colors[vt_active][rd][cd] = vt_colors[vt_active][rs][cs];
        if (vt_active == CONSOLE_VT) mark_dirty(rd);
    } else {
        if (rd >= VGA_HEIGHT || cd >= VGA_WIDTH || rs >= VGA_HEIGHT || cs >= VGA_WIDTH) return;
        term_buf[rd * VGA_WIDTH + cd] = term_buf[rs * VGA_WIDTH + cs];     /* (VGA: its code page 437 byte as it is) */
    }
}

static void maybe_flush_if_console(void) {
    if (vt_active == CONSOLE_VT && term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) maybe_flush();
}

static void es_cursor(long r, long c) {
    size_t h = terminal_usable_height(), w = terminal_width();
    if (!h || !w) return;
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if ((size_t)r >= h) r = (long)h - 1;
    if ((size_t)c >= w) c = (long)w - 1;
    term_row = (size_t)r;
    term_col = (size_t)c;
    vt_row[vt_active] = term_row;
    vt_col[vt_active] = term_col;
    if (!es_grid()) terminal_update_cursor();
}

static void es_erase(size_t r, size_t c0, size_t c1) {          /* [c0, c1) of row r */
    for (size_t c = c0; c < c1; c++) es_cell_set(r, c, ' ', term_color);
}

/* rows [top, h) moved down n (insert) or up n (delete), blanks coming in */
static void es_shift_rows(size_t top, int n, int down) {
    size_t h = terminal_usable_height(), w = terminal_width();
    if (top >= h || n <= 0) return;
    if ((size_t)n > h - top) n = (int)(h - top);
    if (down) {
        for (size_t r = h - 1; r >= top + (size_t)n; r--)
            for (size_t c = 0; c < w; c++) es_cell_copy(r, c, r - (size_t)n, c);
        for (size_t r = top; r < top + (size_t)n; r++) es_erase(r, 0, w);
    } else {
        for (size_t r = top; r + (size_t)n < h; r++)
            for (size_t c = 0; c < w; c++) es_cell_copy(r, c, r + (size_t)n, c);
        for (size_t r = h - (size_t)n; r < h; r++) es_erase(r, 0, w);
    }
}

/* characters of the cursor's row from col moved right n (insert) or left n (delete) */
static void es_shift_cols(int n, int right) {
    size_t w = terminal_width(), r = term_row, c0 = term_col;
    if (n <= 0 || c0 >= w) return;
    if ((size_t)n > w - c0) n = (int)(w - c0);
    if (right) {
        for (size_t c = w - 1; c >= c0 + (size_t)n; c--) es_cell_copy(r, c, r, c - (size_t)n);
        es_erase(r, c0, c0 + (size_t)n);
    } else {
        for (size_t c = c0; c + (size_t)n < w; c++) es_cell_copy(r, c, r, c + (size_t)n);
        es_erase(r, w - (size_t)n, w);
    }
}

/* an answer to the program (where the cursor is): its input, as typed */
static void es_reply(const char* s) {
    for (int t = 0; t < TTY_MAX; t++)
        if (tty_in_use(t) && tty_vt(t) == (int)vt_active) { tty_input(t, (const uint8_t*)s, (uint32_t)strlen(s)); return; }
    keyboard_inject(s);
}

static void es_csi(int v, char f) {
    int n = es_np[v] ? es_p[v][0] : 0, n1 = n ? n : 1;
    size_t h = terminal_usable_height(), w = terminal_width();
    long r = (long)term_row, c = (long)term_col;
    if (es_inter[v] == ' ' && f == 'q') {               /* ESC [ n SP q: the cursor's shape (xterm's DECSCUSR) */
        uint8_t shape = n == 1 || n == 2 ? TERM_CURSOR_BLOCK : n == 3 || n == 4 ? TERM_CURSOR_UNDERLINE :
                        n == 5 || n == 6 ? TERM_CURSOR_BAR : TERM_CURSOR_DEFAULT;
        if (shape != vt_cshape[v] && v == CONSOLE_VT) { g_ov_valid = 0; mark_dirty(term_row); }   /* (the old one erased) */
        vt_cshape[v] = shape;
        return;
    }
    if (es_inter[v]) return;                             /* (other ones: not understood, ignored) */
    if (es_priv[v]) {                                    /* ESC [ ? ... */
        if ((f == 'h' || f == 'l') && n == 25) es_cursor_off[v] = f == 'l';
        return;
    }
    switch (f) {
    case 'm': es_sgr(v); break;
    case 'A': es_cursor(r - n1, c); break;
    case 'B': es_cursor(r + n1, c); break;
    case 'C': es_cursor(r, c + n1); break;
    case 'D': es_cursor(r, c - n1); break;
    case 'E': es_cursor(r + n1, 0); break;
    case 'F': es_cursor(r - n1, 0); break;
    case 'G': case '`': es_cursor(r, n1 - 1); break;
    case 'd': es_cursor(n1 - 1, c); break;
    case 'H': case 'f': es_cursor((long)n1 - 1, (es_np[v] > 1 && es_p[v][1] ? es_p[v][1] : 1) - 1); break;
    case 'J':
        if (n == 3) { terminal_vt_clear_history(v); break; }
        if (n == 0) { es_erase(term_row, term_col, w); for (size_t y = term_row + 1; y < h; y++) es_erase(y, 0, w); }
        else if (n == 1) { for (size_t y = 0; y < term_row; y++) es_erase(y, 0, w); es_erase(term_row, 0, term_col + 1); }
        else for (size_t y = 0; y < h; y++) es_erase(y, 0, w);
        break;
    case 'K':
        if (n == 0) es_erase(term_row, term_col, w);
        else if (n == 1) es_erase(term_row, 0, term_col + 1);
        else es_erase(term_row, 0, w);
        break;
    case 'X': es_erase(term_row, term_col, term_col + (size_t)n1 < w ? term_col + (size_t)n1 : w); break;
    case 'L': es_shift_rows(term_row, n1, 1); break;
    case 'M': es_shift_rows(term_row, n1, 0); break;
    case '@': es_shift_cols(n1, 1); break;
    case 'P': es_shift_cols(n1, 0); break;
    case 'S': es_shift_rows(0, n1, 0); break;
    case 'T': es_shift_rows(0, n1, 1); break;
    case 's': es_save_r[v] = term_row; es_save_c[v] = term_col; break;
    case 'u': es_cursor((long)es_save_r[v], (long)es_save_c[v]); break;
    case 'n':
        if (n == 6) {
            char b[24];
            ksnprintf(b, sizeof(b), "\x1b[%u;%uR", (unsigned)term_row + 1, (unsigned)term_col + 1);
            es_reply(b);
        }
        break;
    default: break;                                      /* (others: ignored, not printed) */
    }
}

/* c as part of an escape sequence: 1 if it was (nothing more to do with it) */
static int es_feed(unsigned char c) {
    int v = (int)vt_active;
    switch (es_state[v]) {
    case ES_TEXT:
        if (c != 0x1B) return 0;
        es_state[v] = ES_ESC;
        return 1;
    case ES_ESC:
        es_state[v] = ES_TEXT;
        if (c == '[') { es_state[v] = ES_CSI; es_np[v] = 0; es_priv[v] = 0; es_inter[v] = 0; memset(es_p[v], 0, sizeof(es_p[v])); }
        else if (c == ']') { es_state[v] = ES_OSC; es_osc_n[v] = 0; }
        else if (c == '7') { es_save_r[v] = term_row; es_save_c[v] = term_col; }
        else if (c == '8') es_cursor((long)es_save_r[v], (long)es_save_c[v]);
        else if (c == 'c') {                              /* reset */
            es_bold[v] = es_rev[v] = 0; es_fg[v] = 7; es_bg[v] = 0; es_cursor_off[v] = 0;
            es_apply_color(v);
            size_t h = terminal_usable_height(), w = terminal_width();
            for (size_t y = 0; y < h; y++) es_erase(y, 0, w);
            es_cursor(0, 0);
        } else if (c == 'M') {                            /* up a line, scrolling down at the top */
            if (term_row == 0) es_shift_rows(0, 1, 1); else es_cursor((long)term_row - 1, (long)term_col);
        }
        return 1;
    case ES_CSI:
        if (c == '?' && es_np[v] == 0 && !es_p[v][0]) { es_priv[v] = 1; return 1; }
        if (c >= '0' && c <= '9') {
            if (es_np[v] == 0) es_np[v] = 1;
            uint32_t x = es_p[v][es_np[v] - 1] * 10u + (c - '0');
            es_p[v][es_np[v] - 1] = (uint16_t)(x > 9999 ? 9999 : x);
            return 1;
        }
        if (c == ';') {
            if (es_np[v] == 0) es_np[v] = 1;
            if (es_np[v] < ES_PARAMS) es_np[v]++;
            return 1;
        }
        if (c >= 0x20 && c <= 0x2F) { es_inter[v] = (uint8_t)c; return 1; }
        if (c >= 0x40 && c <= 0x7E) { es_state[v] = ES_TEXT; es_csi(v, (char)c); return 1; }
        if (c < 0x20 || c > 0x7E) es_state[v] = ES_TEXT;  /* (broken: dropped) */
        return 1;
    case ES_OSC: case ES_OSC_ESC:
        if (c == 0x07 || (es_state[v] == ES_OSC_ESC && c == '\\')) {
            es_state[v] = ES_TEXT;
            es_osc[v][es_osc_n[v]] = 0;
            if ((es_osc[v][0] == '0' || es_osc[v][0] == '2') && es_osc[v][1] == ';') {
                kstrlcpy(vt_title[v], es_osc[v] + 2, sizeof(vt_title[v]));
                g_term_gen++;
            }
            return 1;
        }
        if (c == 0x1B) { es_state[v] = ES_OSC_ESC; return 1; }
        es_state[v] = ES_OSC;
        if (es_osc_n[v] + 1u < sizeof(es_osc[v])) es_osc[v][es_osc_n[v]++] = (char)c;
        return 1;
    }
    es_state[v] = ES_TEXT;
    return 0;
}

uint32_t terminal_vt_bells(int vt) { return vt >= 0 && vt < VT_MAX ? vt_bells[vt] : 0; }
int terminal_vt_ringing(int vt) {
    return vt >= 0 && vt < VT_MAX && vt_bell_ms[vt] && (int32_t)(timer_ms() - vt_bell_ms[vt]) < 200;
}
const char* terminal_vt_title(int vt) { return vt >= 0 && vt < VT_MAX ? vt_title[vt] : ""; }
int terminal_vt_cursor_shape(int vt) { return vt >= 0 && vt < VT_MAX ? vt_cshape[vt] : 0; }
int terminal_vt_cursor_shown(int vt) { return vt >= 0 && vt < VT_MAX ? !es_cursor_off[vt] : 1; }

void terminal_putchar(char c) {
    if (capturing()) { capture_putc(c); return; }
    g_term_gen++;
    /* the console shell (vt0) is mirrored to the serial port */
    if (vt_active == 0 && g_serial_mirror) serial_putc(c);
    /* ...and an SSH session's vt to its client */
    if (g_serial_mirror && vt_active != 0) tty_mirror((int)vt_active, c);
    if (es_feed((unsigned char)c)) { maybe_flush_if_console(); return; }   /* (ESC [ ...: handled, not shown) */
    if (c == 7) {                                       /* BEL: the window flashes (a visual bell) */
        vt_bell_ms[vt_active] = timer_ms() | 1;
        vt_bells[vt_active]++;
        return;
    }
    if (c == '\t') {                                    /* the cursor to the next tab stop (every 8 columns) */
        es_cursor((long)term_row, (long)((term_col / 8 + 1) * 8));
        maybe_flush_if_console();
        return;
    }

    /* text is UTF-8: é comes as two bytes, € as three, and each fills one cell */
    uint32_t cp = (unsigned char)c;
    {
        unsigned char b = (unsigned char)c;
        int v = (int)vt_active;
        if (b >= 0x80) {
            if ((b & 0xC0) == 0x80) {
                if (!vt_u8_need[v]) {
                    cp = 0xFFFD;
                } else {
                    vt_u8_cp[v] = vt_u8_cp[v] << 6 | (b & 0x3F);
                    if (--vt_u8_need[v]) return;
                    cp = vt_u8_cp[v];
                }
            } else {
                vt_u8_need[v] = (uint8_t)(b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0);
                vt_u8_cp[v] = b & (0x3Fu >> vt_u8_need[v]);
                if (vt_u8_need[v]) return;
                cp = 0xFFFD;
            }
        } else {
            vt_u8_need[v] = 0;
        }
    }

    /* new output: a view scrolled back comes back to the live text */
    if (vt_back[vt_active]) {
        vt_back[vt_active] = 0;
        if (vt_active == CONSOLE_VT && fb_console_on()) { mark_all_dirty(); g_pending_scroll = 0; }
    }

    size_t h = terminal_usable_height();
    if (h == 0) return;
    size_t w = terminal_width();

    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available() && vt_active == CONSOLE_VT) {
        /* text grid only; the pixels follow in terminal_flush() */
        if (c == '\n') {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
        } else if (c == '\r') {
            term_col = 0;
        } else if (c == '\b') {
            if (term_col > 0) {
                term_col--;
                vt_chars[CONSOLE_VT][term_row][term_col] = ' ';
                vt_colors[CONSOLE_VT][term_row][term_col] = vt_color[CONSOLE_VT];
                mark_dirty(term_row);
            }
        } else {
            vt_chars[CONSOLE_VT][term_row][term_col] = cp;
            vt_colors[CONSOLE_VT][term_row][term_col] = vt_color[CONSOLE_VT];
            mark_dirty(term_row);
            if (++term_col == w) {
                term_col = 0;
                if (++term_row == h) terminal_scroll();
            }
        }
        vt_row[CONSOLE_VT] = term_row;
        vt_col[CONSOLE_VT] = term_col;
        maybe_flush();
        return;
    }

    if (term_mode == TERMINAL_MODE_SUSPENDED || (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available())) {
        /* Keep backing store up to date so GUI can render terminal window
         * (also: a hidden window's shell writing while the console is up) */
        if (c == '\n') {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
            vt_row[vt_active] = term_row;
            vt_col[vt_active] = term_col;
            return;
        }
        if (c == '\r') { term_col = 0; return; }
        if (c == '\b') {
            if (term_col > 0) {
                term_col--;
                vt_chars[vt_active][term_row][term_col] = ' ';
                vt_colors[vt_active][term_row][term_col] = vt_color[vt_active];
                vt_row[vt_active] = term_row;
                vt_col[vt_active] = term_col;
            }
            return;
        }
        vt_chars[vt_active][term_row][term_col] = cp;
        vt_colors[vt_active][term_row][term_col] = vt_color[vt_active];
        if (++term_col == w) {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
        }
        vt_row[vt_active] = term_row;
        vt_col[vt_active] = term_col;
        return;
    }

    /* VGA text mode (no framebuffer) */
    if (c == '\n') {
        term_col = 0;
        if (++term_row == h)
            terminal_scroll();
        terminal_update_cursor();
        return;
    }
    if (c == '\r') {
        term_col = 0;
        terminal_update_cursor();
        return;
    }
    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
            term_buf[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
        }
        terminal_update_cursor();
        return;
    }

    term_buf[term_row * VGA_WIDTH + term_col] = vga_entry(vga_glyph(cp), term_color);

    if (++term_col == w) {
        term_col = 0;
        if (++term_row == h)
            terminal_scroll();
    }
    vt_row[vt_active] = term_row;
    vt_col[vt_active] = term_col;
    terminal_update_cursor();
}

void terminal_write(const char* str) {
    for (size_t i = 0; str[i]; i++)
        terminal_putchar(str[i]);
}

void terminal_writeln(const char* str) {
    terminal_write(str);
    terminal_putchar('\n');
}

void terminal_write_color(const char* str, uint8_t fg, uint8_t bg) {
    uint8_t saved = term_color, saved_vt = vt_color[vt_active];
    terminal_setcolor(fg, bg);
    terminal_write(str);
    term_color = saved;
    vt_color[vt_active] = saved_vt;     /* (terminal_clear paints with it) */
}

void terminal_cursor_left(void) {
    g_term_gen++;
    if (term_col > 0) {
        size_t old_row = term_row, old_col = term_col;
        term_col--;
        if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
        else fb_refresh_cursor(old_row, old_col);
    }
}

void terminal_cursor_right(void) {
    g_term_gen++;
    size_t w = terminal_width();
    if (term_col + 1 < w) {
        size_t old_row = term_row, old_col = term_col;
        term_col++;
        if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
        else fb_refresh_cursor(old_row, old_col);
    }
}

void terminal_set_cursor(size_t row, size_t col) {
    g_term_gen++;
    size_t h = terminal_usable_height();
    size_t w = terminal_width();
    size_t old_row = term_row, old_col = term_col;
    if (h == 0) { row = 0; col = 0; }
    else if (row >= h) row = h - 1;
    if (col >= w)  col = w - 1;
    term_row = row;
    term_col = col;
    if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
    else fb_refresh_cursor(old_row, old_col);
}

void terminal_get_cursor(size_t* row, size_t* col) {
    if (row) *row = term_row;
    if (col) *col = term_col;
}

void terminal_set_reserved_bottom(size_t rows) {
    if (rows >= VGA_HEIGHT) rows = VGA_HEIGHT - 1;
    term_reserved_bottom = rows;
    size_t h = terminal_usable_height();
    if (h == 0) {
        term_row = 0;
        term_col = 0;
    } else {
        if (term_row >= h) term_row = h - 1;
        if (term_col >= VGA_WIDTH) term_col = VGA_WIDTH - 1;
    }
    if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
}

void terminal_putentryat(char c, uint8_t fg, uint8_t bg, size_t row, size_t col) {
    putentry_cp((unsigned char)c, fg, bg, row, col);
}

static void putentry_cp(uint32_t cp, uint8_t fg, uint8_t bg, size_t row, size_t col) {
    g_term_gen++;
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        uint8_t color = vga_entry_color(fg, bg);
        vt_chars[vt_active][row][col] = cp;
        vt_colors[vt_active][row][col] = color;
        if (vt_active == CONSOLE_VT) { mark_dirty(row); terminal_flush(); }
        return;
    }

    uint8_t color = vga_entry_color(fg, bg);
    term_buf[row * VGA_WIDTH + col] = vga_entry(vga_glyph(cp), color);
}

void terminal_write_at(const char* str, uint8_t fg, uint8_t bg, size_t row, size_t col) {
    if (!str) return;
    size_t x = col;
    int len = (int)strlen(str);
    for (int i = 0; i < len && x < VGA_WIDTH; x++)
        putentry_cp(u8_decode(str, len, &i), fg, bg, row, x);          /* (a UTF-8 character a cell) */
}

void terminal_screen_changed(void) {
    fb_recompute_grid();
    if (splash_active()) { splash_screen_changed(); return; }   /* (drawn again at the new size) */
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) fb_redraw_all();
}

void terminal_set_mode(terminal_mode_t mode) {
    term_mode = mode;
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        fb_redraw_all();   /* back from the desktop: repaint everything */
    }
}

terminal_mode_t terminal_get_mode(void) {
    return term_mode;
}

void terminal_fb_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    term_fb_scale = scale;
    fb_recompute_grid();
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        fb_redraw_all();
    }
}

int terminal_fb_get_scale(void) {
    return term_fb_scale;
}

void terminal_fb_get_buffer(const uint32_t** chars, const uint8_t** colors, int* width, int* height, int* stride) {
    if (chars)  *chars = &vt_chars[vt_active][0][0];
    if (colors) *colors = (const uint8_t*)&vt_colors[vt_active][0][0];
    if (width)  *width = (int)terminal_width();
    if (height) *height = (int)terminal_height();
    if (stride) *stride = FB_MAX_COLS;
}

int terminal_vt_alloc(void) {
    g_term_gen++;
    for (int i = 1; i < VT_MAX; i++) {
        if (!vt_used[i]) {
            vt_used[i] = 1;
            es_reset(i);
            terminal_vt_clear_history(i);
            vt_row[i] = 0;
            vt_col[i] = 0;
            vt_color[i] = vt_color[vt_active];
            for (size_t y = 0; y < FB_MAX_ROWS; y++) {
                for (size_t x = 0; x < FB_MAX_COLS; x++) {
                    vt_chars[i][y][x] = ' ';
                    vt_colors[i][y][x] = vt_color[i];
                }
            }
            return i;
        }
    }
    return -1;
}

void terminal_vt_free(int vt) {
    if (vt <= 0 || vt >= VT_MAX) return;
    vt_used[vt] = 0;
    if (vt_active == vt) terminal_vt_set_active(0);
}

void terminal_vt_set_active(int vt) {
    if (vt < 0 || vt >= VT_MAX) return;
    if (!vt_used[vt]) return;
    vt_active = vt;
    term_row = vt_row[vt_active];
    term_col = vt_col[vt_active];
    term_color = vt_color[vt_active];
}

int terminal_vt_get_active(void) {
    return vt_active;
}

void terminal_vt_get_buffer(int vt, const uint32_t** chars, const uint8_t** colors, int* width, int* height, int* stride) {
    if (vt < 0 || vt >= VT_MAX || !vt_used[vt]) {
        if (chars) *chars = NULL;
        if (colors) *colors = NULL;
        if (width) *width = 0;
        if (height) *height = 0;
        if (stride) *stride = 0;
        return;
    }
    if (chars)  *chars = &vt_chars[vt][0][0];
    if (colors) *colors = (const uint8_t*)&vt_colors[vt][0][0];
    if (width)  *width = (int)(vt_cols[vt] ? vt_cols[vt] : terminal_fb_cols());
    if (height) *height = (int)(vt_rows[vt] ? vt_rows[vt] : terminal_fb_rows());
    if (stride) *stride = FB_MAX_COLS;
}

void terminal_vt_get_cursor(int vt, size_t* row, size_t* col) {
    if (vt < 0 || vt >= VT_MAX || !vt_used[vt]) {
        if (row) *row = 0;
        if (col) *col = 0;
        return;
    }
    if (row) *row = vt_row[vt];
    if (col) *col = vt_col[vt];
}

int terminal_is_capturing(void) {
    return capturing();
}

void terminal_vt_set_size(int vt, int cols, int rows) {
    if (vt <= 0 || vt >= VT_MAX || !vt_used[vt]) return;
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols > FB_MAX_COLS) cols = FB_MAX_COLS;
    if (rows > FB_MAX_ROWS) rows = FB_MAX_ROWS;
    size_t old_cols = vt_cols[vt] ? vt_cols[vt] : term_fb_cols;
    size_t old_rows = vt_rows[vt] ? vt_rows[vt] : term_fb_rows;
    if ((size_t)cols == old_cols && (size_t)rows == old_rows && vt_cols[vt]) return;
    /* the cursor's line stays visible: scroll the text up if the window got shorter */
    if (vt_row[vt] >= (size_t)rows) {
        size_t sh = vt_row[vt] - (size_t)rows + 1;
        for (size_t y = 0; y < sh; y++) sb_push(vt, vt_chars[vt][y], vt_colors[vt][y], old_cols);
        memmove(&vt_chars[vt][0][0], &vt_chars[vt][sh][0], (FB_MAX_ROWS - sh) * FB_MAX_COLS * sizeof(vt_chars[0][0][0]));
        memmove(&vt_colors[vt][0][0], &vt_colors[vt][sh][0], (FB_MAX_ROWS - sh) * FB_MAX_COLS);
        for (size_t y = FB_MAX_ROWS - sh; y < FB_MAX_ROWS; y++)
            for (size_t x = 0; x < FB_MAX_COLS; x++) { vt_chars[vt][y][x] = ' '; vt_colors[vt][y][x] = vt_color[vt]; }
        vt_row[vt] -= sh;
    }
    /* cells that come into view are blank */
    for (size_t y = 0; y < FB_MAX_ROWS; y++)
        for (size_t x = 0; x < FB_MAX_COLS; x++)
            if ((y >= old_rows && y < (size_t)rows) || (x >= old_cols && x < (size_t)cols)) {
                vt_chars[vt][y][x] = ' ';
                vt_colors[vt][y][x] = vt_color[vt];
            }
    if (vt_col[vt] >= (size_t)cols) vt_col[vt] = (size_t)cols - 1;
    vt_cols[vt] = (size_t)cols;
    vt_rows[vt] = (size_t)rows;
    if (vt == vt_active) { term_row = vt_row[vt]; term_col = vt_col[vt]; }
    g_term_gen++;
}

/* ── the scrollback's view ── */
int terminal_vt_view(int vt) { return vt >= 0 && vt < VT_MAX ? vt_back[vt] : 0; }

/* the view max lines back at most (the caller's limit: a window also has
 * the vt's rows above what it shows); 1 if it moved */
int terminal_vt_set_view(int vt, int back, int max) {
    if (vt < 0 || vt >= VT_MAX) return 0;
    if (back > max) back = max;
    if (back < 0) back = 0;
    if (back == vt_back[vt]) return 0;
    vt_back[vt] = back;
    g_term_gen++;
    if (vt == CONSOLE_VT && term_mode == TERMINAL_MODE_FRAMEBUFFER) fb_redraw_all();
    return 1;
}

/* the console, a page (or delta lines) back or forward: its limit is its scrollback */
int terminal_vt_scroll_view(int vt, int delta) {
    if (vt < 0 || vt >= VT_MAX) return 0;
    return terminal_vt_set_view(vt, vt_back[vt] + delta, (int)vt_sb[vt].count + (vt == CONSOLE_VT ? 0 : (int)vt_row[vt]));
}
