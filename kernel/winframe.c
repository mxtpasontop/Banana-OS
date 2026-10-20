/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "winframe.h"
#include "theme.h"
#include "fb.h"
#include "timer.h"
#include "gfx.h"

void win_clamp(win_geom_t* g) {
    const fb_info_t* fi = fb_info();
    int sw = fi ? (int)fi->width : 800, sh = fi ? (int)fi->height - WIN_TASKBAR : 572;
    if (sw > 2560) sw = 2560;               /* the desktop's backbuffer (kernel/gui.c) */
    if (sh > 1600 - WIN_TASKBAR) sh = 1600 - WIN_TASKBAR;
    if (g->w < g->min_w) g->w = g->min_w;
    if (g->h < g->min_h) g->h = g->min_h;
    if (g->w > sw) g->w = sw;
    if (g->h > sh) g->h = sh;
    if (g->x + g->w > sw) g->x = sw - g->w;
    if (g->y + g->h > sh) g->y = sh - g->h;
    if (g->x < 0) g->x = 0;
    if (g->y < 0) g->y = 0;
}

/* ── snapping ───────────────────────────────────────────────────────── */

static win_rect_t g_preview;
static int        g_have_preview;

void winframe_work_area(win_rect_t* r) {
    const fb_info_t* fi = fb_info();
    int sw = fi ? (int)fi->width : 800, sh = fi ? (int)fi->height - WIN_TASKBAR : 572;
    if (sw > 2560) sw = 2560;
    if (sh > 1600 - WIN_TASKBAR) sh = 1600 - WIN_TASKBAR;
    r->x = 0; r->y = 0; r->w = sw; r->h = sh;
}

void winframe_snap_rect(char where, win_rect_t* r) {
    win_rect_t a;
    winframe_work_area(&a);
    int hw = a.w / 2, hh = a.h / 2;
    *r = a;
    switch (where) {
    case 'l': r->w = hw; break;
    case 'r': r->x = hw; r->w = a.w - hw; break;
    case '1': r->w = hw; r->h = hh; break;
    case '2': r->x = hw; r->w = a.w - hw; r->h = hh; break;
    case '3': r->y = hh; r->w = hw; r->h = a.h - hh; break;
    case '4': r->x = hw; r->y = hh; r->w = a.w - hw; r->h = a.h - hh; break;
    default: break;                          /* 'm': the whole work area */
    }
}

int winframe_snap_zone(int mx, int my, win_rect_t* r) {
    win_rect_t a;
    winframe_work_area(&a);
    const int E = 3;                         /* the edge: the last pixels of the screen */
    int corner = a.h / 5 < 60 ? 60 : a.h / 5;
    char where = 0;
    if (mx <= a.x + E) where = my < a.y + corner ? '1' : my > a.y + a.h - corner ? '3' : 'l';
    else if (mx >= a.x + a.w - 1 - E) where = my < a.y + corner ? '2' : my > a.y + a.h - corner ? '4' : 'r';
    else if (my <= a.y + E) where = 'm';
    if (!where) return SNAP_NONE;
    winframe_snap_rect(where, r);
    return where == 'm' ? SNAP_MAX : SNAP_HALF;
}

void winframe_set_preview(const win_rect_t* r) {
    if (r) { g_preview = *r; g_have_preview = 1; }
    else g_have_preview = 0;
}

int winframe_preview(win_rect_t* r) {
    if (g_have_preview && r) *r = g_preview;
    return g_have_preview;
}

void win_snap(win_geom_t* g, const win_rect_t* r, int where) {
    if (!g->maxed && !g->snapped) { g->sx = g->x; g->sy = g->y; g->sw = g->w; g->sh = g->h; }
    g->x = r->x; g->y = r->y; g->w = r->w; g->h = r->h;
    g->maxed = where == SNAP_MAX;
    g->snapped = where == SNAP_HALF;
    g->dragging = g->resizing = 0;
    win_clamp(g);
}

void win_unsnap(win_geom_t* g) {
    if (!g->maxed && !g->snapped) return;
    g->x = g->sx; g->y = g->sy; g->w = g->sw; g->h = g->sh;
    g->maxed = g->snapped = 0;
    win_clamp(g);
}

/* ── shading (the frame being drawn: the desktop's backbuffer) ──────── */

void winframe_tint(int x, int y, int w, int h, uint32_t rgb, int alpha) {
    int stride = 0, W = 0, H = 0;
    uint32_t* t = fb_target(&stride, &W, &H);
    if (!t || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint32_t tr = (rgb >> 16) & 255u, tg = (rgb >> 8) & 255u, tb = rgb & 255u, ia = (uint32_t)(256 - alpha), a = (uint32_t)alpha;
    for (int row = 0; row < h; row++) {
        uint32_t* p = t + (y + row) * stride + x;
        for (int i = 0; i < w; i++) {
            uint32_t c = p[i];
            uint32_t r = (((c >> 16) & 255u) * ia + tr * a) >> 8;
            uint32_t gg = (((c >> 8) & 255u) * ia + tg * a) >> 8;
            uint32_t b = ((c & 255u) * ia + tb * a) >> 8;
            p[i] = (r << 16) | (gg << 8) | b;
        }
    }
}

void winframe_shade(int x, int y, int w, int h, int alpha) {
    winframe_tint(x, y, w, h, 0, alpha);
}

void winframe_shadow(int x, int y, int w, int h) {
    /* a soft edge to the right and below, fading over 6 pixels */
    for (int d = 1; d <= 6; d++) {
        int a = 70 - d * 11;
        winframe_shade(x + w + d - 1, y + d + 2, 1, h - 2, a);
        winframe_shade(x + d + 2, y + h + d - 1, w - 2 + d - 1, 1, a);
    }
}

int win_title_press(win_geom_t* g, int mx, int my) {
    uint32_t now = timer_ms();
    if (now - g->title_ms < 400) {
        g->title_ms = 0;
        if (!g->maxed) {
            if (!g->snapped) { g->sx = g->x; g->sy = g->y; g->sw = g->w; g->sh = g->h; }
            g->x = 0; g->y = 0; g->w = 10000; g->h = 10000;
            g->maxed = 1;
            g->snapped = 0;
        } else {
            g->x = g->sx; g->y = g->sy; g->w = g->sw; g->h = g->sh;
            g->maxed = 0;
        }
        win_clamp(g);
        return 1;
    }
    g->title_ms = now;
    g->dragging = 1;
    g->dx = mx - g->x;
    g->dy = my - g->y;
    g->px = mx;
    g->py = my;
    return 0;
}

int win_grip_press(win_geom_t* g, int mx, int my) {
    if (mx < g->x + g->w - WIN_GRIP || my < g->y + g->h - WIN_GRIP) return 0;
    g->resizing = 1;
    g->maxed = 0;
    g->dx = g->x + g->w - mx;
    g->dy = g->y + g->h - my;
    return 1;
}

int win_mouse(win_geom_t* g, int mx, int my, int left) {
    if (!left) {
        int changed = 0;
        if (g->dragging) {                    /* let go over a snap zone: it goes there */
            win_rect_t r;
            int z = winframe_snap_zone(mx, my, &r);
            if (z) { win_snap(g, &r, z); changed = 1; }
            winframe_set_preview(NULL);
        }
        g->dragging = 0;
        g->resizing = 0;
        return changed;
    }
    int ox = g->x, oy = g->y, ow = g->w, oh = g->h;
    if (g->dragging) {
        /* a snapped or maximized window, dragged away: back to its own size,
         * the pointer at the same place of its title bar */
        if ((g->maxed || g->snapped) && (mx - g->px > 6 || g->px - mx > 6 || my - g->py > 6 || g->py - my > 6)) {
            int ow0 = g->w > 0 ? g->w : 1;
            g->w = g->sw;
            g->h = g->sh;
            g->dx = g->dx * g->w / ow0;
            g->maxed = g->snapped = 0;
        }
        if (!g->maxed && !g->snapped) {
            g->x = mx - g->dx;
            g->y = my - g->dy;
        }
        win_rect_t r;
        winframe_set_preview(winframe_snap_zone(mx, my, &r) ? &r : NULL);
    } else if (g->resizing) {
        g->snapped = 0;
        g->w = mx + g->dx - g->x;
        g->h = my + g->dy - g->y;
    } else {
        return 0;
    }
    win_clamp(g);
    return g->x != ox || g->y != oy || g->w != ow || g->h != oh;
}

/* ── title-bar buttons ─────────────────────────────────────────────
 * [_] minimize   [#] maximize / restore   [x] close, 20 px wide with 4 px
 * between them, the close button 8 px from the right edge. */

static int g_min_request;

static void btn_box(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void outline(int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(x, y, w, 2, c);              /* a thicker top edge, like a window's title bar */
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void win_draw_button_row(int right, int y, int h, int has_max, int maxed) {
    const uint32_t gb = TC(0x303740), ghi = TC(0x535D6E), glo = TC(0x15191F), fg = TC(0xFFFFFF);
    int cx = right - 28;
    btn_box(cx, y, 20, h, TC(0x6D2F2F), TC(0xA14747), TC(0x301717));
    gfx_draw_text(cx + 6, y + (h - 8) / 2, "x", fg, TC(0x6D2F2F));
    int mx = cx - 24;
    if (has_max) {
        btn_box(mx, y, 20, h, gb, ghi, glo);
        int iy = y + (h - 8) / 2;
        if (maxed) {                            /* restore: two windows */
            outline(mx + 8, iy, 7, 6, fg);
            gfx_fill_rect(mx + 5, iy + 3, 7, 6, gb);
            outline(mx + 5, iy + 3, 7, 6, fg);
        } else {
            outline(mx + 5, iy, 10, 8, fg);
        }
        mx -= 24;
    }
    btn_box(mx, y, 20, h, gb, ghi, glo);
    gfx_fill_rect(mx + 6, y + h - 5, 8, 2, fg);  /* _ */
}

int win_button_hit(int right, int y, int h, int has_max, int mx, int my) {
    if (my < y || my >= y + h) return WIN_BTN_NONE;
    int cx = right - 28;
    if (mx >= cx && mx < cx + 20) return WIN_BTN_CLOSE;
    int bx = cx - 24;
    if (has_max) {
        if (mx >= bx && mx < bx + 20) return WIN_BTN_MAX;
        bx -= 24;
    }
    if (mx >= bx && mx < bx + 20) return WIN_BTN_MIN;
    return WIN_BTN_NONE;
}

void win_toggle_max(win_geom_t* g) {
    if (!g->maxed) {
        if (!g->snapped) { g->sx = g->x; g->sy = g->y; g->sw = g->w; g->sh = g->h; }
        g->snapped = 0;
        g->x = 0; g->y = 0; g->w = 10000; g->h = 10000;
        g->maxed = 1;
    } else {
        g->x = g->sx; g->y = g->sy; g->w = g->sw; g->h = g->sh;
        g->maxed = 0;
    }
    g->dragging = g->resizing = 0;
    win_clamp(g);
}

void win_draw_buttons(const win_geom_t* g, int by, int bh) {
    win_draw_button_row(g->x + g->w, g->y + by, bh, 1, g->maxed);
}

int win_button_press(win_geom_t* g, int by, int bh, int mx, int my) {
    int b = win_button_hit(g->x + g->w, g->y + by, bh, 1, mx, my);
    if (b == WIN_BTN_MAX) win_toggle_max(g);
    else if (b == WIN_BTN_MIN) g_min_request = 1;
    return b;
}

int winframe_take_minimize(void) {
    int r = g_min_request;
    g_min_request = 0;
    return r;
}
