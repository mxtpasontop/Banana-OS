/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "devmgr.h"
#include "devices.h"
#include "i18n.h"
#include "theme.h"
#include "gfx.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "ctxmenu.h"
#include "kbnav.h"
#include "utf8.h"
#include "gui.h"
#include "audio.h"
#include "blockdev.h"
#include "settings.h"
#include "../usb/usbcore.h"

/*
 * Device Manager, like Windows': every device of the computer in a tree -
 * by type (Disk drives, Display adapters, Keyboards...) or by connection
 * (each device under the bridge, controller or hub it hangs off) - with
 * the chosen one's properties at the right: its maker, where it is, the
 * driver that runs it (or a yellow mark: none), its hardware ID and its
 * resources. Plugged or unplugged USB devices show up by themselves;
 * "Scan for hardware changes" looks again. The data: kernel/devices.c.
 */

#define TITLE_H  20
#define TOOL_H   30
#define ROW_H    18
#define STATUS_H 20
#define MAX_ROWS (DEV_MAX + DC_COUNT + 1)

#define C_FRAME  TC(0x1D232C)
#define C_CHROME TCL(0xF0F0F0)
#define C_WHITE  TCL(0xFFFFFF)
#define C_LINE   TCL(0xD9D9D9)
#define C_TEXT   TCL(0x1E1E1E)
#define C_DIM    TCL(0x6D6D6D)
#define C_SEL    TCL(0xCCE8FF)
#define C_SEL_B  TCL(0x99D1FF)
#define C_HEAD   TCL(0x1E395B)
#define C_ACCENT TCL(0x0067C0)
#define C_WARN   0x00F2C230u

static win_geom_t g_win = { .x = 70, .y = 40, .w = 720, .h = 500, .min_w = 520, .min_h = 340 };
static int        g_open;
static uint32_t   g_gen = 1;
static device_t*  g_dev;
static int        g_n;
static int        g_by_conn;                  /* the view: by type / by connection */
static uint8_t    g_cat_open[DC_COUNT];
static uint8_t    g_dev_closed[DEV_MAX];       /* (by connection: all open at first) */
static int        g_sel_kind, g_sel_idx;       /* the row chosen: R_* and its category / device */
static int        g_scroll;
static char       g_status[96];
static uint32_t   g_check_ms, g_hw_sig;
static kbnav_t    g_nav;

enum { R_ROOT = 0, R_CAT, R_DEV };
typedef struct { uint8_t kind, depth; int16_t idx; } row_t;
static row_t g_rows[MAX_ROWS];
static int   g_nrows;

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }

static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t line) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_fill_rect(x, y, w, 1, line);
    gfx_fill_rect(x, y + h - 1, w, 1, line);
    gfx_fill_rect(x, y, 1, h, line);
    gfx_fill_rect(x + w - 1, y, 1, h, line);
}

/* at most cols columns of s, ".." when cut */
static void text_cut(int x, int y, const char* s, int cols, uint32_t fg, uint32_t bg) {
    if (cols <= 0) return;
    int len = (int)strlen(s);
    if (u8_width(s, len) <= cols) { gfx_draw_text(x, y, s, fg, bg); return; }
    char buf[160];
    int b = u8_byte_at_col(s, len, cols > 2 ? cols - 2 : cols);
    if (b > (int)sizeof(buf) - 3) b = (int)sizeof(buf) - 3;
    memcpy(buf, s, (size_t)b);
    buf[b] = '.'; buf[b + 1] = '.'; buf[b + 2] = 0;
    gfx_draw_text(x, y, buf, fg, bg);
}

static void button(int x, int y, int w, const char* label, int enabled) {
    kbnav_add(&g_nav, x, y, w, 22);
    uint32_t bg = enabled ? TCL(0xE1E1E1) : TCL(0xEFEFEF);
    frame(x, y, w, 22, bg, enabled ? TCL(0xADADAD) : TCL(0xD5D5D5));
    int tw = u8_width(label, (int)strlen(label)) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 7, label, enabled ? C_TEXT : TCL(0xA0A0A0), bg);
}
static int text_w(const char* s) { return u8_width(s, (int)strlen(s)) * 8; }

/* ── the devices ── */

/* the USB devices and the drives there are: a new scan when it changes */
static uint32_t hw_signature(void) {
    uint32_t s = (uint32_t)usb_device_count() * 31u;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (bd && bd->present) s = s * 16777619u ^ (uint32_t)(i + 1) ^ (uint32_t)bd->nvol << 8;
    }
    return s;
}

static void build_rows(void);

static void rescan(void) {
    /* keep the chosen device chosen: by its hardware ID and place */
    char key[112] = "";
    if (g_sel_kind == R_DEV && g_dev && g_sel_idx < g_n) ksnprintf(key, sizeof(key), "%s|%s|%s", g_dev[g_sel_idx].hwid, g_dev[g_sel_idx].loc, g_dev[g_sel_idx].name);
    if (!g_dev) g_dev = (device_t*)kmalloc(sizeof(device_t) * DEV_MAX);
    if (!g_dev) { g_n = 0; return; }
    g_n = devices_scan(g_dev, DEV_MAX);
    g_hw_sig = hw_signature();
    if (key[0]) {
        int found = -1;
        for (int i = 0; i < g_n && found < 0; i++) {
            char k[112];
            ksnprintf(k, sizeof(k), "%s|%s|%s", g_dev[i].hwid, g_dev[i].loc, g_dev[i].name);
            if (!strcmp(k, key)) found = i;
        }
        if (found >= 0) g_sel_idx = found;
        else { g_sel_kind = R_ROOT; g_sel_idx = 0; }
    }
    build_rows();
    g_gen++;
}

static void add_row(int kind, int depth, int idx) {
    if (g_nrows >= MAX_ROWS) return;
    g_rows[g_nrows].kind = (uint8_t)kind;
    g_rows[g_nrows].depth = (uint8_t)depth;
    g_rows[g_nrows].idx = (int16_t)idx;
    g_nrows++;
}

static int has_children(int p) {
    for (int i = 0; i < g_n; i++) if (g_dev[i].parent == p && g_dev[i].cat != DC_COMPUTER) return 1;
    return 0;
}

static void conn_rows(int parent, int depth) {
    for (int i = 0; i < g_n; i++) {
        if (g_dev[i].parent != parent || g_dev[i].cat == DC_COMPUTER) continue;
        add_row(R_DEV, depth, i);
        if (!g_dev_closed[i] && depth < 10) conn_rows(i, depth + 1);
    }
}

static void build_rows(void) {
    g_nrows = 0;
    add_row(R_ROOT, 0, 0);
    if (g_by_conn) conn_rows(-1, 1);
    else
        for (int c = 0; c < DC_COUNT; c++) {
            int any = 0;
            for (int i = 0; i < g_n; i++) if (g_dev[i].cat == c) { any = 1; break; }
            if (!any) continue;
            add_row(R_CAT, 1, c);
            if (g_cat_open[c]) for (int i = 0; i < g_n; i++) if (g_dev[i].cat == c) add_row(R_DEV, 2, i);
        }
}

static int sel_row(void) {
    for (int r = 0; r < g_nrows; r++)
        if (g_rows[r].kind == g_sel_kind && (g_sel_kind == R_ROOT || g_rows[r].idx == g_sel_idx)) return r;
    return -1;
}

static int row_open(int r) {
    if (g_rows[r].kind == R_ROOT) return 1;
    if (g_rows[r].kind == R_CAT) return g_cat_open[g_rows[r].idx];
    return g_by_conn && has_children(g_rows[r].idx) && !g_dev_closed[g_rows[r].idx];
}
static int row_has_kids(int r) {
    if (g_rows[r].kind != R_DEV) return 1;
    return g_by_conn && has_children(g_rows[r].idx);
}
static void row_toggle(int r) {
    if (g_rows[r].kind == R_CAT) g_cat_open[g_rows[r].idx] ^= 1;
    else if (g_rows[r].kind == R_DEV && g_by_conn) g_dev_closed[g_rows[r].idx] ^= 1;
    build_rows();
}

/* ── the window's parts ── */

static int tree_x(void) { return g_win.x + 6; }
static int tree_y(void) { return g_win.y + TITLE_H + TOOL_H + 2; }
static int tree_w(void) { int w = g_win.w * 52 / 100; return w < 260 ? 260 : w; }
static int tree_h(void) { return g_win.h - TITLE_H - TOOL_H - STATUS_H - 6; }
static int rows_max(void) { int n = (tree_h() - 4) / ROW_H; return n < 1 ? 1 : n; }
static int panel_x(void) { return tree_x() + tree_w() + 6; }
static int panel_w(void) { return g_win.x + g_win.w - 6 - panel_x(); }

static void keep_visible(void) {
    int r = sel_row();
    if (r < 0) return;
    if (r < g_scroll) g_scroll = r;
    if (r >= g_scroll + rows_max()) g_scroll = r - rows_max() + 1;
    if (g_scroll > g_nrows - rows_max()) g_scroll = g_nrows - rows_max();
    if (g_scroll < 0) g_scroll = 0;
}

/* small pictures, 16 x 16, one per kind of device */
static void icon(int cat, int x, int y) {
    uint32_t dk = TCL(0x3B4A5E), md = TCL(0x8FA3BC), lt = TCL(0xDCE6F2), ac = TCL(0x2F7FD0);
    switch (cat) {
    case DC_COMPUTER:
        frame(x + 1, y + 1, 14, 10, lt, dk); gfx_fill_rect(x + 3, y + 3, 10, 6, ac);
        gfx_fill_rect(x + 6, y + 11, 4, 2, dk); gfx_fill_rect(x + 3, y + 13, 10, 2, dk); break;
    case DC_MONITOR:
        frame(x + 1, y + 2, 14, 10, lt, dk); gfx_fill_rect(x + 3, y + 4, 10, 6, TCL(0x7FB5E8));
        gfx_fill_rect(x + 7, y + 12, 2, 2, dk); gfx_fill_rect(x + 4, y + 14, 8, 1, dk); break;
    case DC_DISPLAY:
        frame(x, y + 4, 16, 8, TCL(0x3E8E4E), dk); gfx_fill_rect(x + 3, y + 6, 6, 4, dk);
        gfx_fill_rect(x + 11, y + 6, 3, 4, md); gfx_fill_rect(x + 2, y + 12, 10, 2, TCL(0xD4A72C)); break;
    case DC_DISK:
        frame(x + 1, y + 3, 14, 10, md, dk); gfx_fill_rect(x + 3, y + 10, 3, 1, TCL(0x3CC35A));
        gfx_fill_rect(x + 3, y + 5, 10, 3, lt); break;
    case DC_CDROM:
        frame(x + 1, y + 4, 14, 8, md, dk); gfx_fill_rect(x + 3, y + 6, 10, 1, dk); gfx_fill_rect(x + 11, y + 9, 2, 1, TCL(0x3CC35A)); break;
    case DC_KEYBOARD:
        frame(x, y + 4, 16, 9, lt, dk);
        for (int i = 0; i < 6; i++) { gfx_fill_rect(x + 2 + i * 2, y + 6, 1, 1, dk); gfx_fill_rect(x + 2 + i * 2, y + 8, 1, 1, dk); }
        gfx_fill_rect(x + 4, y + 10, 8, 1, dk); break;
    case DC_MICE:
        frame(x + 4, y + 2, 8, 12, lt, dk); gfx_fill_rect(x + 8, y + 2, 1, 5, dk); gfx_fill_rect(x + 4, y + 7, 8, 1, dk); break;
    case DC_NETWORK:
        frame(x + 1, y + 1, 9, 7, TCL(0x7FB5E8), dk); frame(x + 6, y + 7, 9, 7, TCL(0x7FB5E8), dk);
        gfx_fill_rect(x + 4, y + 8, 1, 4, dk); gfx_fill_rect(x + 4, y + 11, 3, 1, dk); break;
    case DC_PROCESSOR:
        frame(x + 3, y + 3, 10, 10, md, dk); gfx_fill_rect(x + 5, y + 5, 6, 6, dk);
        for (int i = 0; i < 4; i++) { gfx_fill_rect(x + 4 + i * 2 + 1, y + 1, 1, 2, dk); gfx_fill_rect(x + 4 + i * 2 + 1, y + 13, 1, 2, dk);
                                      gfx_fill_rect(x + 1, y + 4 + i * 2 + 1, 2, 1, dk); gfx_fill_rect(x + 13, y + 4 + i * 2 + 1, 2, 1, dk); } break;
    case DC_SOUND:
        gfx_fill_rect(x + 2, y + 6, 4, 4, dk); for (int i = 0; i < 5; i++) gfx_fill_rect(x + 6 + i, y + 6 - i, 1, 4 + i * 2, dk);
        gfx_fill_rect(x + 13, y + 4, 1, 8, md); break;
    case DC_USB: case DC_USBDEV:
        gfx_fill_rect(x + 7, y + 1, 2, 13, dk); gfx_fill_rect(x + 3, y + 5, 2, 4, dk); gfx_fill_rect(x + 3, y + 8, 5, 1, dk);
        gfx_fill_rect(x + 11, y + 4, 2, 3, dk); gfx_fill_rect(x + 9, y + 6, 4, 1, dk); frame(x + 6, y + 13, 4, 3, dk, dk); break;
    case DC_IDE: case DC_STORAGE:
        frame(x + 1, y + 2, 14, 12, TCL(0x3E8E4E), dk);
        for (int i = 0; i < 5; i++) gfx_fill_rect(x + 3 + i * 2, y + 12, 1, 2, TCL(0xD4A72C));
        gfx_fill_rect(x + 4, y + 4, 8, 5, dk); break;
    case DC_PORTS:
        frame(x + 1, y + 4, 14, 8, md, dk); for (int i = 0; i < 5; i++) gfx_fill_rect(x + 3 + i * 2, y + 7, 1, 2, dk); break;
    case DC_HID:
        frame(x + 1, y + 4, 14, 8, lt, dk); gfx_fill_rect(x + 3, y + 7, 4, 1, dk); gfx_fill_rect(x + 4, y + 6, 1, 3, dk);
        gfx_fill_rect(x + 10, y + 6, 2, 2, ac); gfx_fill_rect(x + 12, y + 8, 2, 2, TCL(0xD04040)); break;
    case DC_SYSTEM:
        frame(x + 1, y + 1, 14, 14, lt, dk); gfx_fill_rect(x + 4, y + 4, 8, 8, md); gfx_fill_rect(x + 6, y + 6, 4, 4, dk); break;
    default:                                                     /* other devices: a question */
        frame(x + 1, y + 1, 14, 14, lt, dk);
        gfx_draw_char(x + 4, y + 4, '?', dk, lt); break;
    }
}

/* the yellow triangle of a device without a driver */
static void warn_mark(int x, int y) {
    for (int i = 0; i < 7; i++) gfx_fill_rect(x + 4 - i / 2 - (i & 1), y + i, (i / 2) * 2 + 1 + (i & 1) * 2, 1, C_WARN);
    gfx_fill_rect(x + 4, y + 2, 1, 3, 0x00202020u);
    gfx_fill_rect(x + 4, y + 6, 1, 1, 0x00202020u);
}

static void arrow(int x, int y, int open) {
    uint32_t c = TCL(0x5A5A5A);
    if (open) for (int i = 0; i < 4; i++) gfx_fill_rect(x + i, y + i, 7 - i * 2, 1, c);
    else      for (int i = 0; i < 4; i++) gfx_fill_rect(x + i, y + i - 3 + 3, 1, 7 - i * 2, c);
}

static void draw_tree(void) {
    int x = tree_x(), y = tree_y(), w = tree_w(), h = tree_h();
    frame(x, y, w, h, C_WHITE, C_LINE);
    int max = rows_max();
    for (int k = 0; k < max && g_scroll + k < g_nrows; k++) {
        int r = g_scroll + k;
        const row_t* rw = &g_rows[r];
        int ry = y + 2 + k * ROW_H;
        int ix = x + 6 + rw->depth * 16;
        int sel = rw->kind == g_sel_kind && (g_sel_kind == R_ROOT || rw->idx == g_sel_idx);
        const char* label;
        char root[40];
        int cat;
        if (rw->kind == R_ROOT) { ksnprintf(root, sizeof(root), "%s", "banana-os-0.6"); label = root; cat = DC_COMPUTER; }
        else if (rw->kind == R_CAT) { label = _(device_category_name(rw->idx)); cat = rw->idx; }
        else { label = g_dev[rw->idx].name; cat = g_dev[rw->idx].cat; }
        int tw = text_w(label);
        int lx = ix + 34;
        uint32_t bg = C_WHITE;
        if (sel) {
            int bw = tw + 6;
            if (lx - 3 + bw > x + w - 2) bw = x + w - 2 - (lx - 3);
            frame(lx - 3, ry, bw, ROW_H - 1, C_SEL, C_SEL_B);
            bg = C_SEL;
        }
        if (rw->kind != R_ROOT && row_has_kids(r)) arrow(ix + 2, ry + 5, row_open(r));
        icon(cat, ix + 14, ry);
        if (rw->kind == R_DEV && g_dev[rw->idx].status == DS_NODRIVER) warn_mark(ix + 22, ry + 9);
        text_cut(lx, ry + 5, label, (x + w - 6 - lx) / 8, C_TEXT, bg);
    }
    if (g_nrows > max) {                                         /* the scroll bar */
        int bh = h * max / g_nrows, by = y + (h - bh) * g_scroll / (g_nrows - max);
        gfx_fill_rect(x + w - 6, by, 4, bh < 16 ? 16 : bh, TCL(0xC2C2C2));
    }
}

/* ── the properties at the right ── */

static int g_act_n;
static struct { int x, y, w, id; } g_act[4];
enum { A_EJECT = 1, A_SOUND, A_SETTINGS_SCREEN, A_SETTINGS_KEYBOARD, A_SETTINGS_MOUSE, A_SETTINGS_NETWORK, A_SCAN };

static void act_button(int* bx, int y, const char* label, int id) {
    if (g_act_n >= 4) return;
    int w = text_w(label) + 20;
    if (*bx + w > panel_x() + panel_w() - 4) return;
    button(*bx, y, w, label, 1);
    g_act[g_act_n].x = *bx; g_act[g_act_n].y = y; g_act[g_act_n].w = w; g_act[g_act_n].id = id;
    g_act_n++;
    *bx += w + 6;
}

/* s in cols columns, on two lines when it is longer: the lines used */
static int text_wrap2(int x, int y, const char* s, int cols, uint32_t fg, uint32_t bg) {
    int len = (int)strlen(s);
    if (cols > 8 && u8_width(s, len) > cols) {
        int cut = u8_byte_at_col(s, len, cols);
        for (int i = cut; i > cut / 2; i--) if (s[i] == ' ' || s[i] == ',' || s[i] == '&') { cut = i + 1; break; }
        char first[160];
        if (cut > (int)sizeof(first) - 1) cut = (int)sizeof(first) - 1;
        memcpy(first, s, (size_t)cut);
        first[cut] = 0;
        gfx_draw_text(x, y, first, fg, bg);
        text_cut(x, y + 14, s + cut, cols, fg, bg);
        return 2;
    }
    text_cut(x, y, s, cols, fg, bg);
    return 1;
}

/* a value too long for its line goes on under it (two lines at most) */
static int kv(int x, int y, int kw, int cols, const char* k, const char* v) {
    gfx_draw_text(x, y, k, C_DIM, C_CHROME);
    return y + (text_wrap2(x + kw, y, v, cols - kw / 8, C_TEXT, C_CHROME) == 2 ? 30 : 16);
}

static void draw_panel(void) {
    int x = panel_x(), y = tree_y(), w = panel_w();
    int cols = (w - 16) / 8, kw = 15 * 8;
    g_act_n = 0;
    if (w < 120) return;
    int px = x + 8, py = y + 6;
    if (g_sel_kind != R_DEV || g_sel_idx >= g_n) {
        int probs = 0, cats = 0;
        for (int i = 0; i < g_n; i++) if (g_dev[i].status == DS_NODRIVER) probs++;
        for (int c = 0; c < DC_COUNT; c++) for (int i = 0; i < g_n; i++) if (g_dev[i].cat == c) { cats++; break; }
        const char* title = g_sel_kind == R_CAT ? _(device_category_name(g_sel_idx)) : "banana-os-0.6";
        icon(g_sel_kind == R_CAT ? g_sel_idx : DC_COMPUTER, px, py);
        text_cut(px + 24, py + 4, title, cols - 3, C_HEAD, C_CHROME);
        py += 30;
        char b[96];
        if (g_sel_kind == R_CAT) {
            int k = 0;
            for (int i = 0; i < g_n; i++) if (g_dev[i].cat == g_sel_idx) k++;
            ksnprintf(b, sizeof(b), "%d %s", k, k == 1 ? _("device") : _("devices"));
            gfx_draw_text(px, py, b, C_TEXT, C_CHROME);
            py += 24;
        } else {
            ksnprintf(b, sizeof(b), "%d %s, %d %s", g_n, _("devices"), cats, _("kinds"));
            gfx_draw_text(px, py, b, C_TEXT, C_CHROME);
            py += 18;
            if (probs) {
                warn_mark(px, py - 1);
                ksnprintf(b, sizeof(b), "%d %s", probs, _("without a driver (Other devices)"));
                text_cut(px + 14, py, b, cols - 2, C_TEXT, C_CHROME);
            } else text_cut(px, py, _("Every device has its driver."), cols, C_TEXT, C_CHROME);
            py += 24;
            text_cut(px, py, _("Choose a device to see its properties."), cols, C_DIM, C_CHROME);
            py += 30;
        }
        int bx = px;
        act_button(&bx, py, _("Scan for hardware changes"), A_SCAN);
        return;
    }
    const device_t* d = &g_dev[g_sel_idx];
    icon(d->cat, px, py);
    if (d->status == DS_NODRIVER) warn_mark(px + 8, py + 9);
    text_cut(px + 24, py + 4, d->name, cols - 3, C_HEAD, C_CHROME);
    py += 28;
    py = kv(px, py, kw, cols, _("Device type:"), _(device_category_name(d->cat)));
    if (d->maker[0]) py = kv(px, py, kw, cols, _("Manufacturer:"), d->maker);
    if (d->loc[0]) py = kv(px, py, kw, cols, _("Location:"), d->loc);
    py += 8;
    gfx_draw_text(px, py, _("Device status"), C_HEAD, C_CHROME);
    py += 14;
    const char* st = _(device_status_text(d));
    int two = u8_width(st, (int)strlen(st)) > cols - 2;
    frame(px, py, w - 16, two ? 40 : 26, C_WHITE, C_LINE);
    text_wrap2(px + 6, py + 9, st, cols - 2, d->status == DS_NODRIVER ? TCL(0xA03000) : C_TEXT, C_WHITE);
    py += two ? 50 : 36;
    gfx_draw_text(px, py, _("Driver"), C_HEAD, C_CHROME);
    py += 16;
    char b[96];
    if (d->driver[0]) ksnprintf(b, sizeof(b), "%s", d->driver);
    else ksnprintf(b, sizeof(b), "%s", d->status == DS_BUILTIN ? _("(none needed)") : _("(none)"));
    py = kv(px, py, kw, cols, _("Driver:"), b);
    if (d->status == DS_NODRIVER) {
        py += text_wrap2(px, py, _("A driver module (.bpk) may add one: see `drivers`."), cols, C_DIM, C_CHROME) * 14 + 2;
    }
    py += 8;
    gfx_draw_text(px, py, _("Details"), C_HEAD, C_CHROME);
    py += 16;
    if (d->hwid[0]) py = kv(px, py, kw, cols, _("Hardware ID:"), d->hwid);
    if (d->key[0]) py = kv(px, py, kw, cols, _("Drive:"), d->key);
    int bottom = g_win.y + g_win.h - STATUS_H - 36;
    for (int r = 0; r < d->nres && py < bottom - 14; r++) {
        text_cut(px, py, d->res[r], cols, C_TEXT, C_CHROME);
        py += 16;
    }
    /* what can be done with it */
    int bx = px, by = bottom + 4;
    if (d->kind == DK_BLOCK) act_button(&bx, by, _("Eject"), A_EJECT);
    if (d->cat == DC_SOUND && d->status == DS_OK) act_button(&bx, by, _("Test sound"), A_SOUND);
    if (d->cat == DC_DISPLAY || d->cat == DC_MONITOR) act_button(&bx, by, _("Screen resolution..."), A_SETTINGS_SCREEN);
    if (d->cat == DC_KEYBOARD) act_button(&bx, by, _("Keyboard layout..."), A_SETTINGS_KEYBOARD);
    if (d->cat == DC_MICE) act_button(&bx, by, _("Mouse settings..."), A_SETTINGS_MOUSE);
    if (d->cat == DC_NETWORK) act_button(&bx, by, _("Network settings..."), A_SETTINGS_NETWORK);
}

static void do_action(int id) {
    g_gen++;
    switch (id) {
    case A_SCAN:
        usb_rescan();
        rescan();
        ksnprintf(g_status, sizeof(g_status), "%s", _("Looked for hardware changes"));
        break;
    case A_EJECT: {
        if (g_sel_kind != R_DEV || g_sel_idx >= g_n) break;
        blockdev_t* bd = blockdev_find(g_dev[g_sel_idx].key);
        if (bd) {
            blockdev_eject_all(bd);
            ksnprintf(g_status, sizeof(g_status), "%s: %s", bd->name, _("safe to remove"));
        }
        rescan();
        break;
    }
    case A_SOUND: audio_beep(660, 250); break;
    case A_SETTINGS_SCREEN:   gui_open_settings_page(SETTINGS_PAGE_SCREEN); break;
    case A_SETTINGS_KEYBOARD: gui_open_settings_page(SETTINGS_PAGE_KEYBOARD); break;
    case A_SETTINGS_MOUSE:    gui_open_settings_page(SETTINGS_PAGE_MOUSE); break;
    case A_SETTINGS_NETWORK:  gui_open_settings_page(SETTINGS_PAGE_NETWORK); break;
    }
}

/* ── the toolbar ── */
static int g_tb_x[3], g_tb_w[3];

static void draw_toolbar(void) {
    int x = g_win.x + 6, y = g_win.y + TITLE_H + 4;
    const char* L[3] = { _("Scan for hardware changes"), _("Devices by type"), _("Devices by connection") };
    for (int i = 0; i < 3; i++) {
        g_tb_w[i] = text_w(L[i]) + 20;
        g_tb_x[i] = x;
        int on = (i == 1 && !g_by_conn) || (i == 2 && g_by_conn);
        if (x + g_tb_w[i] > g_win.x + g_win.w - 6) { g_tb_w[i] = 0; continue; }
        kbnav_add(&g_nav, x, y, g_tb_w[i], 22);
        uint32_t bg = on ? C_SEL : TCL(0xE1E1E1);
        frame(x, y, g_tb_w[i], 22, bg, on ? C_SEL_B : TCL(0xADADAD));
        gfx_draw_text(x + 10, y + 7, L[i], C_TEXT, bg);
        x += g_tb_w[i] + (i == 0 ? 16 : 4);
    }
}

/* ── public ── */

void devmgr_open(void) {
    if (!g_open) {
        g_sel_kind = R_ROOT;
        g_sel_idx = 0;
        g_scroll = 0;
        g_status[0] = 0;
    }
    g_open = 1;
    rescan();
    /* the devices without a driver: shown at once (as Windows does) */
    for (int i = 0; i < g_n; i++) if (g_dev[i].status == DS_NODRIVER) g_cat_open[DC_OTHER] = 1;
    build_rows();
    win_clamp(&g_win);
    g_gen++;
}

void devmgr_close(void) {
    g_open = 0;
    g_win.dragging = g_win.resizing = 0;
    if (g_dev) { kfree(g_dev); g_dev = NULL; g_n = 0; }
    g_gen++;
}

int devmgr_is_open(void) { return g_open; }
int devmgr_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }
win_geom_t* devmgr_geom(void) { return &g_win; }

uint32_t devmgr_signature(void) {
    if (!g_open) return 0;
    if (timer_ms() - g_check_ms >= 1000) {              /* plugged or unplugged: look again */
        g_check_ms = timer_ms();
        if (hw_signature() != g_hw_sig) rescan();
    }
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^ i18n_generation();
}

void devmgr_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    kbnav_begin(&g_nav);
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    frame(x, y, W, H, C_CHROME, C_FRAME);
    gfx_fill_rect(x + 1, y + 1, W - 2, TITLE_H + 1, C_WHITE);
    icon(DC_SYSTEM, x + 6, y + 3);
    gfx_draw_text(x + 28, y + 8, _("Device Manager"), C_TEXT, C_WHITE);
    win_draw_buttons(&g_win, 4, 12);
    gfx_fill_rect(x + 1, y + TITLE_H + 1, W - 2, 1, C_LINE);
    draw_toolbar();
    if (!g_dev) { gfx_draw_text(x + 10, y + 60, "out of memory", C_TEXT, C_CHROME); return; }
    keep_visible();
    draw_tree();
    draw_panel();
    /* the status bar */
    int sy = y + H - STATUS_H;
    gfx_fill_rect(x + 1, sy, W - 2, 1, C_LINE);
    char b[128];
    if (g_status[0]) ksnprintf(b, sizeof(b), "%s", g_status);
    else {
        int probs = 0;
        for (int i = 0; i < g_n; i++) if (g_dev[i].status == DS_NODRIVER) probs++;
        ksnprintf(b, sizeof(b), "%d %s", g_n, _("devices"));
        if (probs) ksnprintf(b + strlen(b), sizeof(b) - strlen(b), ", %d %s", probs, _("without a driver"));
    }
    text_cut(x + 8, sy + 6, b, (W - 30) / 8, C_DIM, C_CHROME);
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, C_ACCENT);
}

static int row_at(int mx, int my) {
    int x = tree_x(), y = tree_y();
    if (!inside(mx, my, x, y + 2, tree_w(), rows_max() * ROW_H)) return -1;
    int r = g_scroll + (my - y - 2) / ROW_H;
    return r < g_nrows ? r : -1;
}

static void choose_row(int r) {
    g_sel_kind = g_rows[r].kind;
    g_sel_idx = g_rows[r].idx;
    g_status[0] = 0;
}

void devmgr_click(int mx, int my) {
    if (!devmgr_contains(mx, my)) return;
    int ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { devmgr_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    kbnav_mouse(&g_nav);
    int ty = g_win.y + TITLE_H + 4;
    for (int i = 0; i < 3; i++)
        if (g_tb_w[i] && inside(mx, my, g_tb_x[i], ty, g_tb_w[i], 22)) {
            if (i == 0) do_action(A_SCAN);
            else { g_by_conn = i == 2; g_scroll = 0; build_rows(); }
            return;
        }
    for (int i = 0; i < g_act_n; i++)
        if (inside(mx, my, g_act[i].x, g_act[i].y, g_act[i].w, 22)) { do_action(g_act[i].id); return; }
    int r = row_at(mx, my);
    if (r < 0) return;
    static uint32_t last_ms;
    static int last_r = -1;
    uint32_t now = timer_ms();
    int ix = tree_x() + 6 + g_rows[r].depth * 16;
    int dbl = r == last_r && now - last_ms < 450;
    last_r = r;
    last_ms = now;
    choose_row(r);
    if ((mx < ix + 12 && mx >= ix - 2) || dbl) { if (g_rows[r].kind != R_ROOT && row_has_kids(r)) row_toggle(r); last_r = -1; }
}

static void menu_cb(int id, void* arg) { (void)arg; do_action(id); }

void devmgr_rclick(int mx, int my) {
    if (!devmgr_contains(mx, my)) return;
    int r = row_at(mx, my);
    if (r < 0) return;
    choose_row(r);
    g_gen++;
    ctx_item_t items[4];
    int n = 0;
    if (g_rows[r].kind == R_DEV && g_dev[g_rows[r].idx].kind == DK_BLOCK) items[n++] = (ctx_item_t){ _("Eject"), A_EJECT, 0 };
    items[n++] = (ctx_item_t){ _("Scan for hardware changes"), A_SCAN, 0 };
    ctxmenu_open(mx, my, items, n, menu_cb, NULL);
}

void devmgr_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

void devmgr_wheel(int mx, int my, int dz) {
    (void)mx; (void)my;
    g_scroll += dz * 3;
    if (g_scroll > g_nrows - rows_max()) g_scroll = g_nrows - rows_max();
    if (g_scroll < 0) g_scroll = 0;
    g_gen++;
}

int devmgr_navkey(int code) {
    if (!g_open) return 0;
    g_gen++;
    int r = sel_row();
    if (code == KB_UP || code == KB_DOWN || code == KB_HOME || code == KB_END || code == KB_PGUP || code == KB_PGDN) {
        if (r < 0) r = 0;
        else if (code == KB_UP && r > 0) r--;
        else if (code == KB_DOWN && r < g_nrows - 1) r++;
        else if (code == KB_HOME) r = 0;
        else if (code == KB_END) r = g_nrows - 1;
        else if (code == KB_PGUP) r = r > rows_max() ? r - rows_max() : 0;
        else if (code == KB_PGDN) r = r + rows_max() < g_nrows ? r + rows_max() : g_nrows - 1;
        choose_row(r);
        keep_visible();
        return 1;
    }
    if (r >= 0 && (code == KB_RIGHT || code == KB_LEFT || code == KB_ENTER)) {
        int kids = g_rows[r].kind != R_ROOT && row_has_kids(r);
        if (code == KB_ENTER && kids) row_toggle(r);
        else if (code == KB_RIGHT && kids && !row_open(r)) row_toggle(r);
        else if (code == KB_LEFT && kids && row_open(r)) row_toggle(r);
        else if (code == KB_LEFT) {                              /* to the row above it in the tree */
            for (int p = r - 1; p >= 0; p--) if (g_rows[p].depth < g_rows[r].depth) { choose_row(p); break; }
        }
        keep_visible();
        return 1;
    }
    int cx, cy;
    int k = kbnav_key(&g_nav, code, &cx, &cy);
    if (k == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        devmgr_click(cx, cy);
        g_nav.shown = shown;
        g_nav.focus = focus;
    }
    return k != 0;
}
