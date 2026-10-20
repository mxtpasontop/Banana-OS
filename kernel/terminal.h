/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TERMINAL_H
#define TERMINAL_H

#include "types.h"

/* Virtual terminal count: vt0 is the boot/console shell, plus one per GUI
 * terminal window (TERM_WIN_MAX in kernel/gui.c). Shared with modules
 * (e.g. shell/editor.c) that need one instance of some per-window state
 * per vt, so every window can run independently. */
#define TERMINAL_VT_MAX 16  /* console + the GUI windows and their tabs + 2 SSH sessions */

/* VGA colors */
enum vga_color {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW        = 14,
    VGA_COLOR_WHITE         = 15,
};

void terminal_init(void);
void terminal_clear(void);
void terminal_setcolor(uint8_t fg, uint8_t bg);
void terminal_putchar(char c);
void terminal_write(const char* str);
void terminal_writeln(const char* str);
void terminal_write_color(const char* str, uint8_t fg, uint8_t bg);
void terminal_cursor_left(void);
void terminal_cursor_right(void);
void terminal_set_cursor(size_t row, size_t col);

typedef enum {
    TERMINAL_MODE_VGA_TEXT = 0,
    TERMINAL_MODE_FRAMEBUFFER = 1,
    TERMINAL_MODE_SUSPENDED = 2,
} terminal_mode_t;

void terminal_set_mode(terminal_mode_t mode);
void terminal_screen_changed(void);   /* a new resolution: the grid follows */
terminal_mode_t terminal_get_mode(void);

/* Framebuffer-console scaling (bigger scale = fewer columns/rows on screen) */
void terminal_fb_set_scale(int scale);
int  terminal_fb_get_scale(void);

/* A wide character (an emoji, CJK: u8_wcwidth 2) takes two grid cells:
 * the character, then this in the cell after it (drawn by the first) */
#define TERMINAL_WIDE_CONT 0x110000u

/* Access framebuffer backing store (80x25) for GUI windows */
void terminal_fb_get_buffer(const uint32_t** chars, const uint8_t** colors, int* width, int* height, int* stride);

/* Virtual terminals (for multiple GUI terminal windows) */
int  terminal_vt_alloc(void);          /* returns vt id or -1 */
void terminal_vt_free(int vt);
void terminal_vt_set_active(int vt);   /* selects vt for subsequent output */
int  terminal_vt_get_active(void);
void terminal_vt_get_buffer(int vt, const uint32_t** chars, const uint8_t** colors, int* width, int* height, int* stride);
void terminal_vt_get_cursor(int vt, size_t* row, size_t* col);
/* a GUI window's text grid: output to that vt wraps and scrolls at this size */
void terminal_vt_set_size(int vt, int cols, int rows);

/* Text grid of the active terminal (columns / usable rows). */
size_t terminal_get_width(void);
size_t terminal_get_height(void);

/* vt0 output is mirrored to the serial console; the shell's line editor
 * turns that off while it repaints and sends its own ANSI redraw. */
void terminal_serial_mirror(int on);

/* changes whenever any terminal content or cursor changes */
uint32_t terminal_generation(void);

/* The framebuffer console paints lazily (see terminal.c): this brings
 * the screen up to date now. Cheap when nothing changed; called from
 * idle/wait loops so output never lingers unpainted. */
void terminal_flush(void);

/* Redirection (`cmd > file`): the calling task's output is collected
 * instead of shown. start: 0 if a capture is already running; stop: the
 * heap buffer (caller kfree()s it) and its length. */
int   terminal_capture_start(void);
char* terminal_capture_stop(uint32_t* len);
int   terminal_is_capturing(void);        /* the calling task's output is redirected */

/* GUI helpers */
void terminal_get_cursor(size_t* row, size_t* col);
void terminal_set_reserved_bottom(size_t rows);
void terminal_putentryat(char c, uint8_t fg, uint8_t bg, size_t row, size_t col);
void terminal_write_at(const char* str, uint8_t fg, uint8_t bg, size_t row, size_t col);
/* what VGA text mode shows for a Unicode character (code page 437; '?' if it has none) */
char terminal_cp437(uint32_t cp);

/* Scrollback: the lines that scrolled off a vt's top (2000 kept), and its
 * view - lines back from the live text (new output brings it back) */
int  terminal_vt_history(int vt);                 /* how many lines */
/* how many lines have left the top of vt's screen, ever (text that was at
 * row r is now at r minus the change: what keeps a selection on its text) */
uint32_t terminal_vt_lines_scrolled(int vt);
/* the line `back` lines up (1: the newest); NULLs if there is none */
void terminal_vt_history_line(int vt, int back, const uint32_t** chars, const uint8_t** colors);
void terminal_vt_clear_history(int vt);
int  terminal_vt_view(int vt);
int  terminal_vt_set_view(int vt, int back, int max);   /* 1 if it moved */
int  terminal_vt_scroll_view(int vt, int delta);        /* + is back (up); 1 if it moved */
/* set by programs with escape sequences: the title (ESC ] 0;title BEL, "" if
 * none) and whether the cursor is shown (ESC [ ?25 l / h) */
const char* terminal_vt_title(int vt);
int  terminal_vt_cursor_shown(int vt);
/* ... and its shape (ESC [ n SP q, as xterm: 1-2 a block, 3-4 an underline,
 * 5-6 a bar; 0: the default, an underline) */
enum { TERM_CURSOR_DEFAULT, TERM_CURSOR_BLOCK, TERM_CURSOR_UNDERLINE, TERM_CURSOR_BAR };
int  terminal_vt_cursor_shape(int vt);
/* BEL (\a): how many vt has had, and whether its window is flashing for one (200 ms) */
uint32_t terminal_vt_bells(int vt);
int  terminal_vt_ringing(int vt);

#endif
