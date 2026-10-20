/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "font.h"
#include "kstring.h"
#include "gfx.h"
#include "fb.h"
#include "font8x8.h"
#include "utf8.h"

static int g_ok = 0;

void gfx_init(void) {
    g_ok = fb_available();
}

int gfx_available(void) {
    return g_ok;
}

void gfx_clear(uint32_t rgb) {
    if (!g_ok) return;
    const fb_info_t* fi = fb_info();
    fb_fill_rect(0, 0, (int)fi->width, (int)fi->height, rgb);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!g_ok) return;
    fb_fill_rect(x, y, w, h, rgb);
}

/* the 8x8 bitmap of a character: ASCII and Latin-1 have one (NULL: none) */
static const uint8_t* bitmap_of(uint32_t cp) {
    if (cp < 0x80) return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF) return font8x8_latin1[cp - 0xA0];
    return NULL;
}

/* a wide character (an emoji): as tall as its cells - an emoji's ink is
 * about one em, square, so it fills the two cells' width too - its
 * baseline where that ink sits centred in them */
static int wide_px(int h) { return h < 6 ? 6 : h + h / 8; }
static int wide_base(int y, int h, int px) { return y + h / 2 + px * 3 / 8; }

/* a character the 8x8 font has not, drawn with DejaVu Sans Mono into the
 * w x h cell at (x, y) (its background already filled; for a wide
 * character - an emoji, CJK - w spans both its cells): 0 if there is no
 * such font, or it has nothing to draw */
static int ttf_cell(int x, int y, int w, int h, uint32_t cp, uint32_t fg) {
    if (!font_available(FONT_MONO) || cp < 0x20 || cp == 0xFFFF) return 0;
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return 0;
    char u[4];
    uint32_t n = (uint32_t)u8_encode(cp, u);
    int px = h * 13 / 8, a, d, wide = gfx_char_cols(cp) == 2;
    if (wide) px = wide_px(h);
    int adv = font_text_width(FONT_MONO, px, u, n);
    if (adv > w + 1) {                            /* still wider than its cells: smaller, to fit */
        px = px * w / adv;
        if (px < 6) px = 6;
        adv = font_text_width(FONT_MONO, px, u, n);
    }
    font_metrics(FONT_MONO, px, &a, &d, NULL);
    int base = wide ? wide_base(y, h, px) : y + (h - (a + d)) / 2 + a;
    font_draw(t, stride, x, y, x + w, y + h, FONT_MONO, px, x + (w - adv) / 2, base, u, n, fg, 0);
    return 1;
}

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) { gfx_draw_char_cp(x, y, (uint8_t)c, fg, bg); }

void gfx_draw_char_cp(int x, int y, uint32_t cp, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return;
    if (cp >= 0x80 && cp < 0xA0) cp = '?';

    /* Blank cells (most of any terminal) are a plain fill. */
    if (cp == ' ' || cp == 0 || cp == 0xA0) { fb_fill_rect(x, y, 8, 8, bg); return; }
    const uint8_t* glyph = bitmap_of(cp);
    if (!glyph) {                               /* (beyond Latin-1: the TrueType font, else '?') */
        int cw = 8 * gfx_char_cols(cp);         /* (a wide character: two cells, full size) */
        fb_fill_rect(x, y, cw, 8, bg);
        if (ttf_cell(x, y, cw, 8, cp, fg)) return;
        glyph = font8x8_basic['?'];
    }

    /* Clip once per glyph, then write rows straight into the target
     * instead of 64 bounds-checked fb_putpixel() calls. */
    int gx0 = x < 0 ? -x : 0, gx1 = (x + 8 > tw) ? tw - x : 8;
    int gy0 = y < 0 ? -y : 0, gy1 = (y + 8 > th) ? th - y : 8;
    if (gx0 >= gx1 || gy0 >= gy1) return;
    for (int gy = gy0; gy < gy1; gy++) {
        uint8_t row = glyph[gy];
        uint32_t* p = t + (uint32_t)(y + gy) * (uint32_t)stride + (uint32_t)x;
        for (int gx = gx0; gx < gx1; gx++) p[gx] = (row & (1u << gx)) ? fg : bg;
    }
}

void gfx_draw_char_scaled(int x, int y, int scale, char c, uint32_t fg, uint32_t bg) {
    gfx_draw_char_scaled_cp(x, y, scale, (uint8_t)c, fg, bg);
}

void gfx_draw_char_scaled_cp(int x, int y, int scale, uint32_t cp, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    if (scale <= 1) { gfx_draw_char_cp(x, y, cp, fg, bg); return; }

    if (cp >= 0x80 && cp < 0xA0) cp = '?';
    const uint8_t* glyph = bitmap_of(cp);
    if (!glyph) {
        int cw = 8 * scale * gfx_char_cols(cp);
        fb_fill_rect(x, y, cw, 8 * scale, bg);
        if (ttf_cell(x, y, cw, 8 * scale, cp, fg)) return;
        glyph = font8x8_basic['?'];
    }

    for (int gy = 0; gy < 8; gy++) {
        uint8_t row = glyph[gy];
        for (int gx = 0; gx < 8; gx++) {
            uint32_t col = (row & (1u << gx)) ? fg : bg;
            int px = x + gx * scale;
            int py = y + gy * scale;
            fb_fill_rect(px, py, scale, scale, col);
        }
    }
}

/* ── smooth UI text: DejaVu Sans Mono, one glyph per 8-pixel cell (so every
 * layout made for the 8x8 font still fits), UTF-8 aware ── */
static int g_smooth = 1;

void gfx_set_smooth_text(int on) { g_smooth = on; }
int  gfx_smooth_text(void) { return g_smooth && font_available(FONT_MONO); }

/* cell: 8 * scale; the glyphs are a little taller than the cell, centered on it */
static void smooth_text(int x, int y, int scale, const char* s, uint32_t fg, uint32_t bg) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return;
    int cell = 8 * scale, px = 13 * scale, base = y + 8 * scale;
    if (font_ui() != FONT_MONO) {               /* another font: its baseline where Mono's would be */
        int a, d;
        font_metrics(FONT_UI, px, &a, &d, NULL);
        base = y + (cell - (a + d)) / 2 + a + scale;
    }
    const char* e = s + strlen(s);
    int cx = x;
    while (s < e) {
        const char* start = s;
        const char* line_end = s;
        while (line_end < e && *line_end != '\n') line_end++;
        int n = 0;
        for (const char* q = start; q < line_end; ) n += gfx_char_cols(utf8_next(&q, line_end));
        fb_fill_rect(cx, y, n * cell, cell, bg);
        while (s < line_end) {
            const char* g = s;
            uint32_t cp = utf8_next(&s, line_end);
            int cells = gfx_char_cols(cp);      /* (an emoji or CJK character: two) */
            if (cp != ' ' && cells) {
                int adv = font_text_width(FONT_UI, px, g, (uint32_t)(s - g));
                font_draw(t, stride, 0, 0, tw, th, FONT_UI, px, cx + (cell * cells - adv) / 2, base, g, (uint32_t)(s - g), fg, 0);
            }
            cx += cell * cells;
        }
        if (s < e && *s == '\n') { s++; cx = x; y += cell; base += cell; }
    }
}

/* ── the text size: Terminal and Notepad cells, desktop icon names ── */
static int g_tsize;
/* per size: the smooth cell (w, h, pixels per em), the classic one (w, h,
 * x and y scale of the 8x8 glyphs), the icon names' size */
static const struct { int sw, sh, px, cw, ch, sx, sy, label; const char* name; } TSIZE[GFX_TEXT_SIZES] = {
    {  8,  8, 13,  8,  8, 1, 1, 12, "Normal" },
    {  9, 16, 14,  8, 16, 1, 2, 13, "Large" },
    { 11, 20, 17, 16, 16, 2, 2, 14, "Larger" },
    { 14, 26, 22, 16, 24, 2, 3, 15, "Largest" },
};

void gfx_set_text_size(int size) { g_tsize = size < 0 ? 0 : size >= GFX_TEXT_SIZES ? GFX_TEXT_SIZES - 1 : size; }
int  gfx_text_size(void) { return g_tsize; }
const char* gfx_text_size_name(int size) { return size >= 0 && size < GFX_TEXT_SIZES ? TSIZE[size].name : ""; }
int  gfx_cell_w(void) { return gfx_smooth_text() ? TSIZE[g_tsize].sw : TSIZE[g_tsize].cw; }
int  gfx_cell_h(void) { return gfx_smooth_text() ? TSIZE[g_tsize].sh : TSIZE[g_tsize].ch; }
int  gfx_label_px(void) { return TSIZE[g_tsize].label; }

void gfx_draw_cell_char(int x, int y, char c, uint32_t fg, uint32_t bg) { gfx_draw_cell_cp(x, y, (uint8_t)c, fg, bg); }

int gfx_char_cols(uint32_t cp) { return u8_wcwidth(cp); }

void gfx_draw_cell_cp(int x, int y, uint32_t cp, uint32_t fg, uint32_t bg) {
    if (!g_ok) return;
    if (g_tsize == 0) { gfx_draw_char_cp(x, y, cp, fg, bg); return; }     /* the classic 8x8 cells */
    int cw = gfx_cell_w(), ch = gfx_cell_h();
    if (gfx_char_cols(cp) == 2) cw *= 2;       /* a wide character fills two cells */
    fb_fill_rect(x, y, cw, ch, bg);
    if (cp == ' ' || cp == 0 || cp == 0xA0) return;
    if (cp >= 0x80 && cp < 0xA0) cp = '?';
    if (gfx_smooth_text()) {
        int stride, tw, th;
        uint32_t* t = fb_target(&stride, &tw, &th);
        if (!t) return;
        char u[4];
        uint32_t n = (uint32_t)u8_encode(cp, u);
        int px = TSIZE[g_tsize].px, a, d, wide = gfx_char_cols(cp) == 2;
        if (wide) px = wide_px(ch);
        int adv = font_text_width(FONT_UI, px, u, n);
        if (adv > cw + 2) {                       /* still wider than its cells: smaller, to fit */
            px = px * (cw + 2) / adv;
            if (px < 6) px = 6;
            adv = font_text_width(FONT_UI, px, u, n);
        }
        font_metrics(FONT_UI, px, &a, &d, NULL);
        font_draw(t, stride, x, y, x + cw + 2, y + ch, FONT_UI, px, x + (cw - adv) / 2, wide ? wide_base(y, ch, px) : y + (ch - (a + d)) / 2 + a, u, n, fg, 0);
        return;
    }
    const uint8_t* glyph = bitmap_of(cp);
    if (!glyph) {
        if (ttf_cell(x, y, cw, ch, cp, fg)) return;
        glyph = font8x8_basic['?'];
    }
    int sx = TSIZE[g_tsize].sx, sy = TSIZE[g_tsize].sy;
    int ox = x + (cw - 8 * sx) / 2, oy = y + (ch - 8 * sy) / 2;
    for (int gy = 0; gy < 8; gy++)
        for (int gx = 0; gx < 8; gx++)
            if (glyph[gy] & (1u << gx)) fb_fill_rect(ox + gx * sx, oy + gy * sy, sx, sy, fg);
}

void gfx_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg) {
    if (!g_ok || !s) return;
    if (gfx_smooth_text()) { smooth_text(x, y, 1, s, fg, bg); return; }
    int cx = x, len = (int)strlen(s);
    for (int i = 0; i < len; ) {
        if (s[i] == '\n') { cx = x; y += 8; i++; continue; }
        uint32_t cp = u8_decode(s, len, &i);
        int cells = gfx_char_cols(cp);
        if (!cells) continue;
        gfx_draw_char_cp(cx, y, cp, fg, bg);      /* (a whole UTF-8 character a cell, a wide one two) */
        cx += 8 * cells;
    }
}

void gfx_draw_text_scaled(int x, int y, int scale, const char* s, uint32_t fg, uint32_t bg) {
    if (!g_ok || !s) return;
    if (scale <= 1) { gfx_draw_text(x, y, s, fg, bg); return; }
    if (gfx_smooth_text()) { smooth_text(x, y, scale, s, fg, bg); return; }
    int cx = x, len = (int)strlen(s);
    int step = 8 * scale;
    for (int i = 0; i < len; ) {
        if (s[i] == '\n') { cx = x; y += step; i++; continue; }
        uint32_t cp = u8_decode(s, len, &i);
        int cells = gfx_char_cols(cp);
        if (!cells) continue;
        gfx_draw_char_scaled_cp(cx, y, scale, cp, fg, bg);
        cx += step * cells;
    }
}


void gfx_draw_grip(int right, int bottom) {
    /* three diagonal rows of dots, like most desktops */
    for (int row = 0; row < 3; row++)
        for (int k = 0; k <= row; k++)
            gfx_fill_rect(right - 5 - (row - k) * 4, bottom - 5 - k * 4, 2, 2, 0x008A96AAu);
}

/* ── desktop icon labels: no background - the text straight on the
 * wallpaper with a dark shadow, so it reads on light and dark pictures
 * alike - centred under the icon, wrapped onto up to two lines ── */

/* one line, centred on cx, drawn transparently (fg) */
static void label_line(int cx, int y, const char* s, uint32_t n, uint32_t fg, int px) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t || !n) return;
    if (font_available(FONT_SANS)) {
        int asc, desc, lh;
        font_metrics(FONT_SANS, px, &asc, &desc, &lh);
        int w = font_text_width(FONT_SANS, px, s, n);
        font_draw(t, stride, 0, 0, tw, th, FONT_SANS, px, cx - w / 2, y + asc, s, n, fg, 0);
        return;
    }
    /* no TrueType font: the 8x8 one, only its set pixels */
    int x = cx - (int)n * 4;
    for (uint32_t i = 0; i < n; i++, x += 8) {
        uint8_t uc = (uint8_t)s[i];
        if (uc >= 128) uc = '?';
        const uint8_t* g = font8x8_basic[uc];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (1u << gx)) fb_fill_rect(x + gx, y + gy, 1, 1, fg);
    }
}

static int label_w(const char* s, uint32_t n, int px) {
    return font_available(FONT_SANS) ? font_text_width(FONT_SANS, px, s, n) : (int)n * 8;
}

int gfx_draw_label(int cx, int y, int max_w, const char* s, uint32_t fg, uint32_t highlight) {
    if (!g_ok || !s) return 0;
    const int px = gfx_label_px();
    int lh = px + 2;
    if (font_available(FONT_SANS)) { int a, d; font_metrics(FONT_SANS, px, &a, &d, &lh); }
    /* the lines: as many words as fit, at most two (the second cut short with "..") */
    uint32_t len = (uint32_t)strlen(s), start[2] = { 0, 0 }, cnt[2] = { 0, 0 };
    int lines = 0;
    uint32_t p = 0;
    while (p < len && lines < 2) {
        while (p < len && s[p] == ' ') p++;
        uint32_t best = 0, q = p;
        while (q <= len) {
            if (q == len || s[q] == ' ') {
                if (label_w(s + p, q - p, px) <= max_w || !best) best = q - p;
                else break;
                if (q == len) break;
            }
            q++;
        }
        if (!best) break;
        start[lines] = p;
        cnt[lines] = best;
        lines++;
        p += best;
    }
    char tail[64];
    const char* l2 = s + start[1];
    if (lines == 2 && start[1] + cnt[1] < len) {          /* more than fits: ".." */
        uint32_t n = cnt[1] < sizeof(tail) - 3 ? cnt[1] : sizeof(tail) - 3;
        while (n && label_w(s + start[1], n, px) + label_w("..", 2, px) > max_w) n--;
        memcpy(tail, s + start[1], n);
        tail[n] = '.'; tail[n + 1] = '.'; tail[n + 2] = 0;
        l2 = tail;
        cnt[1] = n + 2;
    }
    for (int i = 0; i < lines; i++) {
        const char* ls = i ? l2 : s + start[0];
        int w = label_w(ls, cnt[i], px), ly = y + i * lh;
        if (highlight) gfx_fill_rect(cx - w / 2 - 2, ly - 1, w + 4, lh + 1, highlight);
        else label_line(cx + 1, ly + 1, ls, cnt[i], 0x00000000u, px);   /* the shadow */
        label_line(cx, ly, ls, cnt[i], fg, px);
    }
    return lines * lh;
}
