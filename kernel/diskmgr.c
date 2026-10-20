/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "diskmgr.h"
#include "diskpart.h"
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
#include "blockdev.h"
#include "../usb/usbcore.h"

/*
 * "Disk Management", like Windows': the volumes in a list at the top, and
 * every disk below as a bar of its partitions and unallocated space. A
 * right-click (or the toolbar) on a disk, a volume or free space: New
 * volume (FAT32 or exFAT, a size, a name), Format, Delete, Mount / Unmount,
 * a new partition table (MBR or GPT). The disk Banana OS runs from and CD
 * drives are shown, never changed. The work: kernel/diskpart.c.
 */

#define TITLE_H  20
#define TOOL_H   30
#define LROW_H   18
#define DROW_H   68
#define LABEL_W  118
#define STATUS_H 20

#define C_FRAME  TC(0x1D232C)
#define C_CHROME TCL(0xF0F0F0)
#define C_WHITE  TCL(0xFFFFFF)
#define C_LINE   TCL(0xC8C8C8)
#define C_TEXT   TCL(0x1E1E1E)
#define C_DIM    TCL(0x6D6D6D)
#define C_SEL    TCL(0xCCE8FF)
#define C_SEL_B  TCL(0x0067C0)
#define C_ACCENT TCL(0x0067C0)
#define C_PRIM   0x00203F9Au      /* (Windows' colours: a primary partition's stripe) */
#define C_SYS    0x00406080u
#define C_UNALLOC 0x00101010u

static win_geom_t g_win = { .x = 20, .y = 24, .w = 760, .h = 530, .min_w = 560, .min_h = 380 };
static int        g_open;
static uint32_t   g_gen = 1;
static dp_disk_t* g_disks;
static int        g_n;
static int        g_sd = -1, g_sp = -1, g_sg = -1;   /* chosen: disk, its part (index), or its free space (index) */
static int        g_scroll_list, g_scroll_disks;
static char       g_status[112];
static uint32_t   g_check_ms, g_hw_sig;
static kbnav_t    g_nav;

/* the dialog in the window */
enum { DLG_NONE = 0, DLG_NEW, DLG_FORMAT, DLG_INIT, DLG_DELETE };
static int  g_dlg;
static int  g_dlg_fs = DP_FAT32, g_dlg_scheme = DP_MBR, g_dlg_focus;   /* focus: 0 size, 1 label */
static char g_dlg_size[12], g_dlg_label[12];

static int inside(int mx, int my, int x, int y, int w, int h) { return mx >= x && mx < x + w && my >= y && my < y + h; }
static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t line) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_fill_rect(x, y, w, 1, line);
    gfx_fill_rect(x, y + h - 1, w, 1, line);
    gfx_fill_rect(x, y, 1, h, line);
    gfx_fill_rect(x + w - 1, y, 1, h, line);
}
static int text_w(const char* s) { return u8_width(s, (int)strlen(s)) * 8; }
static void text_cut(int x, int y, const char* s, int cols, uint32_t fg, uint32_t bg) {
    if (cols <= 0) return;
    int len = (int)strlen(s);
    if (u8_width(s, len) <= cols) { gfx_draw_text(x, y, s, fg, bg); return; }
    char buf[128];
    int b = u8_byte_at_col(s, len, cols > 2 ? cols - 2 : cols);
    if (b > (int)sizeof(buf) - 3) b = (int)sizeof(buf) - 3;
    memcpy(buf, s, (size_t)b);
    buf[b] = '.'; buf[b + 1] = '.'; buf[b + 2] = 0;
    gfx_draw_text(x, y, buf, fg, bg);
}
static void button(int x, int y, int w, const char* label, int enabled) {
    if (enabled) kbnav_add(&g_nav, x, y, w, 22);
    uint32_t bg = enabled ? TCL(0xE1E1E1) : TCL(0xEFEFEF);
    frame(x, y, w, 22, bg, enabled ? TCL(0xADADAD) : TCL(0xD5D5D5));
    gfx_draw_text(x + (w - text_w(label)) / 2, y + 7, label, enabled ? C_TEXT : TCL(0xA0A0A0), bg);
}

/* ── the disks ── */

static uint32_t hw_signature(void) {
    uint32_t s = (uint32_t)usb_device_count() * 31u;
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        blockdev_t* bd = blockdev_get(i);
        if (bd && bd->present) s = s * 16777619u ^ (uint32_t)(i + 1) ^ (uint32_t)bd->nvol << 8;
    }
    return s;
}

static void rescan(void) {
    char keep[16] = "";
    if (g_sd >= 0 && g_sd < g_n) kstrlcpy(keep, g_disks[g_sd].name, sizeof(keep));
    if (!g_disks) g_disks = (dp_disk_t*)kmalloc(sizeof(dp_disk_t) * DP_MAX_DISKS);
    if (!g_disks) { g_n = 0; return; }
    g_n = dp_scan(g_disks, DP_MAX_DISKS);
    g_hw_sig = hw_signature();
    int found = -1;
    for (int i = 0; i < g_n; i++) if (keep[0] && !strcmp(g_disks[i].name, keep)) found = i;
    if (found < 0) { g_sd = -1; g_sp = g_sg = -1; }
    else {
        g_sd = found;
        if (g_sp >= g_disks[g_sd].nparts) g_sp = -1;
        if (g_sg >= g_disks[g_sd].ngaps) g_sg = -1;
    }
    g_gen++;
}

static dp_disk_t* sel_disk(void) { return g_sd >= 0 && g_sd < g_n ? &g_disks[g_sd] : NULL; }
static dp_part_t* sel_part(void) { dp_disk_t* d = sel_disk(); return d && g_sp >= 0 && g_sp < d->nparts ? &d->parts[g_sp] : NULL; }

/* what the chosen thing allows */
static int can_new(void)    { dp_disk_t* d = sel_disk(); return d && d->writable && g_sg >= 0; }
static int can_format(void) { dp_disk_t* d = sel_disk(); return d && d->writable && g_sp >= 0; }
static int can_delete(void) { dp_disk_t* d = sel_disk(); dp_part_t* p = sel_part(); return d && d->writable && p && (p->number <= 4 || d->scheme == DP_GPT); }
static int can_mount(void)  { dp_disk_t* d = sel_disk(); dp_part_t* p = sel_part(); return d && d->writable && p && !p->mnt && (!strcmp(p->fs, "FAT32") || !strcmp(p->fs, "exFAT")); }
static int can_unmount(void){ dp_disk_t* d = sel_disk(); dp_part_t* p = sel_part(); return d && !d->system && p && p->mnt; }
static int can_init(void)   { dp_disk_t* d = sel_disk(); return d && d->writable && g_sp < 0 && g_sg < 0; }

static void part_name(const dp_disk_t* d, const dp_part_t* p, char* out, int cap) {
    if (p->number) ksnprintf(out, (size_t)cap, "%sp%d", d->name, p->number);
    else ksnprintf(out, (size_t)cap, "%s", d->name);
}

/* ── layout ── */
static int list_y(void) { return g_win.y + TITLE_H + TOOL_H + 2; }
static int list_h(void) { return (g_win.h - TITLE_H - TOOL_H - STATUS_H) * 36 / 100; }
static int disks_y(void) { return list_y() + list_h() + 6; }
static int disks_h(void) { return g_win.y + g_win.h - STATUS_H - 4 - disks_y(); }
static int list_rows(void) { int n = (list_h() - LROW_H - 4) / LROW_H; return n < 1 ? 1 : n; }

/* the volume list: one row per partition of every disk */
static int list_count(void) { int k = 0; for (int i = 0; i < g_n; i++) k += g_disks[i].cdrom ? 0 : g_disks[i].nparts; return k; }
static int list_at(int row, int* di, int* pi) {
    for (int i = 0; i < g_n; i++) {
        if (g_disks[i].cdrom) continue;
        if (row < g_disks[i].nparts) { *di = i; *pi = row; return 1; }
        row -= g_disks[i].nparts;
    }
    return 0;
}

static const int COLW[] = { 132, 58, 82, 66, 172, 66, 80, 48 };
static const char* const COLN[] = { "Volume", "Layout", "Type", "File System", "Status", "Capacity", "Free Space", "% Free" };

static void draw_list(void) {
    int x = g_win.x + 6, y = list_y(), w = g_win.w - 12, h = list_h();
    frame(x, y, w, h, C_WHITE, C_LINE);
    gfx_fill_rect(x + 1, y + 1, w - 2, LROW_H, C_CHROME);
    int cx = x + 6;
    for (int c = 0; c < 8 && cx < x + w - 20; c++) {
        text_cut(cx, y + 6, _(COLN[c]), (COLW[c] - 6) / 8, C_TEXT, C_CHROME);
        cx += COLW[c];
    }
    int rows = list_rows(), total = list_count();
    if (g_scroll_list > total - rows) g_scroll_list = total - rows;
    if (g_scroll_list < 0) g_scroll_list = 0;
    for (int r = 0; r < rows && g_scroll_list + r < total; r++) {
        int di, pi;
        if (!list_at(g_scroll_list + r, &di, &pi)) break;
        dp_disk_t* d = &g_disks[di];
        dp_part_t* p = &d->parts[pi];
        int ry = y + 2 + LROW_H + r * LROW_H;
        int sel = di == g_sd && pi == g_sp;
        uint32_t bg = sel ? C_SEL : C_WHITE;
        if (sel) gfx_fill_rect(x + 1, ry, w - 2, LROW_H, C_SEL);
        char col[8][48];
        char nm[24];
        part_name(d, p, nm, sizeof(nm));
        if (p->label[0]) ksnprintf(col[0], sizeof(col[0]), "%s (%s)", p->label, nm);
        else ksnprintf(col[0], sizeof(col[0]), "%s", nm);
        ksnprintf(col[1], sizeof(col[1]), "%s", _("Simple"));
        ksnprintf(col[2], sizeof(col[2]), "%s", _(d->removable ? "Removable" : "Basic"));
        ksnprintf(col[3], sizeof(col[3]), "%s", p->fs[0] ? p->fs : "RAW");
        if (p->system) ksnprintf(col[4], sizeof(col[4]), "%s", _("Healthy (System)"));
        else if (p->mnt) ksnprintf(col[4], sizeof(col[4]), "%s %s", _("Healthy, at"), p->where);
        else ksnprintf(col[4], sizeof(col[4]), "%s", _("Healthy (not mounted)"));
        dp_size_text((uint64_t)p->sectors * 512u, col[5], sizeof(col[5]));
        if (p->total) {
            dp_size_text(p->free_bytes, col[6], sizeof(col[6]));
            ksnprintf(col[7], sizeof(col[7]), "%u %%", (uint32_t)(p->free_bytes * 100 / p->total));
        } else { kstrlcpy(col[6], "-", sizeof(col[6])); kstrlcpy(col[7], "-", sizeof(col[7])); }
        cx = x + 6;
        for (int c = 0; c < 8 && cx < x + w - 20; c++) {
            text_cut(cx, ry + 5, col[c], (COLW[c] - 8) / 8, C_TEXT, bg);
            cx += COLW[c];
        }
    }
}

/* the boxes of one disk's bar: parts and gaps in disk order; returns how many */
typedef struct { int kind, idx; uint32_t base, sectors; int x, w; } box_t;   /* kind 0 part, 1 gap */
static int disk_boxes(const dp_disk_t* d, int bx, int bw, box_t* out, int max) {
    int n = 0;
    for (int i = 0; i < d->nparts && n < max; i++) out[n++] = (box_t){ 0, i, d->parts[i].base, d->parts[i].sectors, 0, 0 };
    for (int g = 0; g < d->ngaps && n < max; g++) out[n++] = (box_t){ 1, g, d->gap_base[g], d->gap_sectors[g], 0, 0 };
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j].base < out[j - 1].base; j--) { box_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    if (!n) return 0;
    /* widths by size, each at least 64 px */
    const int MINW = 64;
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += out[i].sectors;
    int spare = bw - MINW * n;
    if (spare < 0) spare = 0;
    int x = bx;
    for (int i = 0; i < n; i++) {
        int w = MINW + (int)(total ? (uint64_t)spare * out[i].sectors / total : 0);
        if (i == n - 1) w = bx + bw - x;
        out[i].x = x;
        out[i].w = w;
        x += w;
    }
    return n;
}

static void draw_disks(void) {
    int x = g_win.x + 6, y = disks_y(), w = g_win.w - 12, h = disks_h();
    frame(x, y, w, h, C_WHITE, C_LINE);
    int fit = (h - 4) / (DROW_H + 6);
    if (g_scroll_disks > g_n - fit) g_scroll_disks = g_n - fit;
    if (g_scroll_disks < 0) g_scroll_disks = 0;
    int ndisk = 0, ncd = 0;
    for (int i = 0; i < g_n; i++) {
        int num = g_disks[i].cdrom ? ncd++ : ndisk++;
        if (i < g_scroll_disks) continue;
        int k = i - g_scroll_disks;
        if (k >= fit) break;
        dp_disk_t* d = &g_disks[i];
        int ry = y + 4 + k * (DROW_H + 6);
        /* the disk's box at the left */
        int dsel = i == g_sd && g_sp < 0 && g_sg < 0;
        frame(x + 4, ry, LABEL_W, DROW_H, dsel ? C_SEL : C_CHROME, dsel ? C_SEL_B : C_LINE);
        uint32_t lb = dsel ? C_SEL : C_CHROME;
        char b[48], sz[16];
        ksnprintf(b, sizeof(b), "%s %d", d->cdrom ? "CD-ROM" : _("Disk"), num);
        gfx_draw_text(x + 10, ry + 6, b, C_TEXT, lb);
        text_cut(x + 10, ry + 21, d->cdrom ? "DVD" : _(d->kind), (LABEL_W - 12) / 8, C_TEXT, lb);
        if (!d->cdrom) {
            dp_size_text((uint64_t)d->sectors * 512u, sz, sizeof(sz));
            gfx_draw_text(x + 10, ry + 36, sz, C_TEXT, lb);
        }
        text_cut(x + 10, ry + 51, d->cdrom ? d->model : d->name, (LABEL_W - 12) / 8, C_DIM, lb);
        /* its bar */
        int bx = x + 8 + LABEL_W, bw = w - LABEL_W - 16;
        if (d->cdrom) {
            frame(bx, ry, bw, DROW_H, C_WHITE, C_LINE);
            text_cut(bx + 8, ry + 26, d->model, bw / 8 - 2, C_DIM, C_WHITE);
            continue;
        }
        box_t boxes[DP_MAX_PARTS + DP_MAX_GAPS];
        int nb = disk_boxes(d, bx, bw, boxes, DP_MAX_PARTS + DP_MAX_GAPS);
        if (!nb) {
            frame(bx, ry, bw, DROW_H, C_WHITE, C_LINE);
            gfx_fill_rect(bx + 1, ry + 1, bw - 2, 8, C_UNALLOC);
            text_cut(bx + 8, ry + 20, d->scheme == DP_NONE ? _("Not initialized: right-click for a partition table") : _("No room"), bw / 8 - 2, C_DIM, C_WHITE);
            continue;
        }
        for (int j = 0; j < nb; j++) {
            box_t* bo = &boxes[j];
            int sel = i == g_sd && ((bo->kind == 0 && bo->idx == g_sp) || (bo->kind == 1 && bo->idx == g_sg));
            uint32_t bg = sel ? C_SEL : C_WHITE;
            frame(bo->x, ry, bo->w - 2, DROW_H, bg, sel ? C_SEL_B : C_LINE);
            if (sel) frame(bo->x + 1, ry + 1, bo->w - 4, DROW_H - 2, bg, C_SEL_B);
            int cols = (bo->w - 12) / 8;
            dp_size_text((uint64_t)bo->sectors * 512u, sz, sizeof(sz));
            if (bo->kind == 1) {
                gfx_fill_rect(bo->x + 2, ry + 2, bo->w - 6, 8, C_UNALLOC);
                text_cut(bo->x + 6, ry + 16, sz, cols, C_TEXT, bg);
                text_cut(bo->x + 6, ry + 31, _("Unallocated"), cols, C_TEXT, bg);
                continue;
            }
            dp_part_t* p = &d->parts[bo->idx];
            gfx_fill_rect(bo->x + 2, ry + 2, bo->w - 6, 8, p->system ? C_SYS : C_PRIM);
            char nm[40], l2[48];
            part_name(d, p, nm, sizeof(nm));
            if (p->label[0]) { char t[40]; ksnprintf(t, sizeof(t), "%s (%s)", p->label, nm); kstrlcpy(nm, t, sizeof(nm)); }
            text_cut(bo->x + 6, ry + 15, nm, cols, C_TEXT, bg);
            ksnprintf(l2, sizeof(l2), "%s %s", sz, p->fs[0] ? p->fs : "RAW");
            text_cut(bo->x + 6, ry + 30, l2, cols, C_TEXT, bg);
            if (p->system) kstrlcpy(l2, _("Banana OS"), sizeof(l2));
            else if (p->mnt) kstrlcpy(l2, p->where, sizeof(l2));
            else kstrlcpy(l2, _("Healthy (not mounted)"), sizeof(l2));
            text_cut(bo->x + 6, ry + 45, l2, cols, C_DIM, bg);
        }
    }
}

/* ── the toolbar ── */
enum { T_REFRESH, T_NEW, T_FORMAT, T_DELETE, T_MOUNT, T_UNMOUNT, T_INIT, T_COUNT };
static int g_tb_x[T_COUNT], g_tb_w[T_COUNT];
static const char* tool_label(int t) {
    switch (t) {
    case T_REFRESH: return _("Refresh");
    case T_NEW:     return _("New volume...");
    case T_FORMAT:  return _("Format...");
    case T_DELETE:  return _("Delete...");
    case T_MOUNT:   return _("Mount");
    case T_UNMOUNT: return _("Unmount");
    }
    return _("New table...");
}
static int tool_ok(int t) {
    switch (t) {
    case T_REFRESH: return 1;
    case T_NEW:     return can_new();
    case T_FORMAT:  return can_format();
    case T_DELETE:  return can_delete();
    case T_MOUNT:   return can_mount();
    case T_UNMOUNT: return can_unmount();
    }
    return can_init();
}

static void draw_toolbar(void) {
    int x = g_win.x + 6, y = g_win.y + TITLE_H + 4;
    for (int t = 0; t < T_COUNT; t++) {
        const char* l = tool_label(t);
        int w = text_w(l) + 16;
        g_tb_w[t] = 0;
        if (x + w > g_win.x + g_win.w - 6) continue;
        g_tb_x[t] = x;
        g_tb_w[t] = w;
        button(x, y, w, l, tool_ok(t));
        x += w + (t == T_REFRESH ? 12 : 4);
    }
}

/* ── the dialog ── */

static void open_dialog(int which) {
    dp_disk_t* d = sel_disk();
    if (!d) return;
    g_dlg = which;
    g_dlg_focus = 1;                                  /* (the label: the size is filled in) */
    g_dlg_label[0] = 0;
    g_dlg_size[0] = 0;
    if (which == DLG_NEW && g_sg >= 0) {
        uint32_t mb = d->gap_sectors[g_sg] / 2048;
        ksnprintf(g_dlg_size, sizeof(g_dlg_size), "%u", mb);
        g_dlg_fs = mb >= 64 ? DP_FAT32 : DP_EXFAT;
    }
    if (which == DLG_FORMAT) {
        dp_part_t* p = sel_part();
        if (p) { kstrlcpy(g_dlg_label, p->label, sizeof(g_dlg_label)); g_dlg_fs = !strcmp(p->fs, "exFAT") || p->sectors < 33 * 2048 * 2 ? DP_EXFAT : DP_FAT32; }
    }
    if (which == DLG_INIT) g_dlg_scheme = d->sectors > 0xFFFFFFF0u ? DP_GPT : DP_MBR;
    g_gen++;
}

static int dlg_w(void) { return 470; }
static int dlg_h(void) { return g_dlg == DLG_NEW ? 230 : g_dlg == DLG_FORMAT ? 190 : 150; }
static int dlg_x(void) { return g_win.x + (g_win.w - dlg_w()) / 2; }
static int dlg_y(void) { return g_win.y + (g_win.h - dlg_h()) / 2; }

static void radio(int x, int y, int on, const char* t) {
    kbnav_add(&g_nav, x, y - 2, text_w(t) + 26, 18);
    frame(x, y, 14, 14, C_WHITE, TCL(0x7A7A7A));
    if (on) gfx_fill_rect(x + 4, y + 4, 6, 6, C_ACCENT);
    gfx_draw_text(x + 22, y + 3, t, C_TEXT, C_CHROME);
}
static void field(int x, int y, int w, const char* v, int focus) {
    kbnav_add(&g_nav, x, y, w, 22);
    frame(x, y, w, 22, C_WHITE, focus ? C_ACCENT : TCL(0x7A7A7A));
    gfx_draw_text(x + 6, y + 7, v, C_TEXT, C_WHITE);
    if (focus && (timer_ms() / 500) % 2 == 0) gfx_fill_rect(x + 6 + text_w(v), y + 5, 2, 12, C_TEXT);
}

static void draw_dialog(void) {
    if (!g_dlg) return;
    dp_disk_t* d = sel_disk();
    if (!d) { g_dlg = 0; return; }
    int x = dlg_x(), y = dlg_y(), w = dlg_w(), h = dlg_h();
    winframe_shade(g_win.x + 1, g_win.y + TITLE_H + 2, g_win.w - 2, g_win.h - TITLE_H - 3, 70);
    frame(x, y, w, h, C_CHROME, C_FRAME);
    gfx_fill_rect(x + 1, y + 1, w - 2, 22, C_WHITE);
    const char* title = g_dlg == DLG_NEW ? _("New volume") : g_dlg == DLG_FORMAT ? _("Format") :
                        g_dlg == DLG_INIT ? _("New partition table") : _("Delete volume");
    gfx_draw_text(x + 10, y + 8, title, C_TEXT, C_WHITE);
    int px = x + 16, py = y + 36;
    char b[128], nm[24];
    if (g_dlg == DLG_DELETE || g_dlg == DLG_INIT) {
        dp_part_t* p = sel_part();
        if (g_dlg == DLG_DELETE && p) part_name(d, p, nm, sizeof(nm));
        else kstrlcpy(nm, d->name, sizeof(nm));
        ksnprintf(b, sizeof(b), "%s %s (%s).", _("Everything on it will be erased:"), nm, d->model);
        text_cut(px, py, b, (w - 32) / 8, C_TEXT, C_CHROME);
        py += 24;
        if (g_dlg == DLG_INIT) {
            radio(px, py, g_dlg_scheme == DP_MBR, _("MBR (Master Boot Record)"));
            radio(px, py + 22, g_dlg_scheme == DP_GPT, _("GPT (GUID Partition Table)"));
        } else text_cut(px, py, _("Delete this volume?"), (w - 32) / 8, C_TEXT, C_CHROME);
    } else {
        if (g_dlg == DLG_NEW) {
            uint32_t mb = g_sg >= 0 && g_sg < d->ngaps ? d->gap_sectors[g_sg] / 2048 : 0;
            ksnprintf(b, sizeof(b), "%s (%s %u MB):", _("Size in MB"), _("at most"), mb);
            gfx_draw_text(px, py + 7, b, C_TEXT, C_CHROME);
            field(px + 240, py, 120, g_dlg_size, g_dlg_focus == 0);
            py += 32;
        } else {
            dp_part_t* p = sel_part();
            if (p) { part_name(d, p, nm, sizeof(nm)); ksnprintf(b, sizeof(b), "%s %s", _("Everything on it will be erased:"), nm); text_cut(px, py, b, (w - 32) / 8, C_TEXT, C_CHROME); }
            py += 26;
        }
        gfx_draw_text(px, py + 7, _("Volume label:"), C_TEXT, C_CHROME);
        field(px + 240, py, 120, g_dlg_label, g_dlg_focus == 1);
        py += 34;
        gfx_draw_text(px, py, _("File system:"), C_TEXT, C_CHROME);
        radio(px + 140, py - 3, g_dlg_fs == DP_FAT32, "FAT32");
        radio(px + 240, py - 3, g_dlg_fs == DP_EXFAT, "exFAT");
        py += 22;
        text_cut(px, py, g_dlg_fs == DP_FAT32 ? _("FAT32: every computer reads it; files up to 4 GB.") : _("exFAT: big files; Windows, macOS, Linux read it."),
                 (w - 32) / 8, C_DIM, C_CHROME);
    }
    button(x + w - 200, y + h - 34, 90, _("OK"), 1);
    button(x + w - 104, y + h - 34, 90, _("Cancel"), 1);
}

static void dialog_ok(void) {
    dp_disk_t* d = sel_disk();
    int which = g_dlg;
    g_dlg = 0;
    if (!d) return;
    char err[96] = "";
    int r = -1;
    dp_part_t* p = sel_part();
    char nm[24];
    if (which == DLG_NEW) {
        uint32_t mb = 0;
        k_parse_u32(g_dlg_size, &mb);
        int num = 0;
        r = dp_create(d, g_sg, mb * 2048u, g_dlg_fs, g_dlg_label, &num, err, sizeof(err));
        if (!r) { ksnprintf(g_status, sizeof(g_status), "%s %sp%d", _("Created"), d->name, num); g_sg = -1; g_sp = -1;
                  for (int i = 0; i < d->nparts; i++) if (d->parts[i].number == num) g_sp = i; }
    } else if (which == DLG_FORMAT && p) {
        part_name(d, p, nm, sizeof(nm));
        r = dp_format(d, p->number, g_dlg_fs, g_dlg_label, err, sizeof(err));
        if (!r) ksnprintf(g_status, sizeof(g_status), "%s %s", _("Formatted"), nm);
    } else if (which == DLG_DELETE && p) {
        part_name(d, p, nm, sizeof(nm));
        r = dp_delete(d, p->number, err, sizeof(err));
        if (!r) { ksnprintf(g_status, sizeof(g_status), "%s %s", _("Deleted"), nm); g_sp = -1; }
    } else if (which == DLG_INIT) {
        r = dp_init(d, g_dlg_scheme, err, sizeof(err));
        if (!r) ksnprintf(g_status, sizeof(g_status), "%s: %s", d->name, g_dlg_scheme == DP_GPT ? "GPT" : "MBR");
    }
    if (r && err[0]) ksnprintf(g_status, sizeof(g_status), "%s", err);
    if (g_sp >= d->nparts) g_sp = -1;
    if (g_sg >= d->ngaps) g_sg = -1;
    g_hw_sig = hw_signature();
    g_gen++;
}

static int dialog_click(int mx, int my) {
    int x = dlg_x(), y = dlg_y(), w = dlg_w(), h = dlg_h();
    if (inside(mx, my, x + w - 200, y + h - 34, 90, 22)) { dialog_ok(); return 1; }
    if (inside(mx, my, x + w - 104, y + h - 34, 90, 22)) { g_dlg = 0; return 1; }
    int px = x + 16, py = y + 36;
    if (g_dlg == DLG_INIT) {
        py += 24;
        if (inside(mx, my, px, py - 2, 300, 18)) g_dlg_scheme = DP_MBR;
        if (inside(mx, my, px, py + 20, 300, 18)) g_dlg_scheme = DP_GPT;
        return 1;
    }
    if (g_dlg == DLG_NEW || g_dlg == DLG_FORMAT) {
        if (g_dlg == DLG_NEW) { if (inside(mx, my, px + 240, py, 120, 22)) g_dlg_focus = 0; py += 32; }
        else py += 26;
        if (inside(mx, my, px + 240, py, 120, 22)) g_dlg_focus = 1;
        py += 34;
        if (inside(mx, my, px + 140, py - 5, 90, 18)) g_dlg_fs = DP_FAT32;
        if (inside(mx, my, px + 240, py - 5, 90, 18)) g_dlg_fs = DP_EXFAT;
    }
    return 1;
}

/* ── actions ── */

static void act(int t) {
    dp_disk_t* d = sel_disk();
    char err[96] = "";
    g_status[0] = 0;
    switch (t) {
    case T_REFRESH: rescan(); ksnprintf(g_status, sizeof(g_status), "%s", _("Refreshed")); break;
    case T_NEW:     if (can_new()) open_dialog(DLG_NEW); break;
    case T_FORMAT:  if (can_format()) open_dialog(DLG_FORMAT); break;
    case T_DELETE:  if (can_delete()) open_dialog(DLG_DELETE); break;
    case T_INIT:    if (can_init()) open_dialog(DLG_INIT); break;
    case T_MOUNT:
        if (can_mount()) {
            if (dp_mount(d, sel_part()->number, err, sizeof(err))) ksnprintf(g_status, sizeof(g_status), "%s", err);
            else ksnprintf(g_status, sizeof(g_status), "%s %s", _("Mounted at"), sel_part()->where);
        }
        break;
    case T_UNMOUNT:
        if (can_unmount()) {
            dp_unmount(d, sel_part()->number, err, sizeof(err));
            ksnprintf(g_status, sizeof(g_status), "%s", _("Unmounted - safe to remove"));
        }
        break;
    }
    g_hw_sig = hw_signature();
    g_gen++;
}

/* ── public ── */

void diskmgr_open(void) {
    if (!g_open) { g_sd = g_sp = g_sg = -1; g_status[0] = 0; g_dlg = 0; g_scroll_list = g_scroll_disks = 0; }
    g_open = 1;
    rescan();
    win_clamp(&g_win);
    g_gen++;
}

void diskmgr_close(void) {
    g_open = 0;
    g_dlg = 0;
    g_win.dragging = g_win.resizing = 0;
    if (g_disks) { kfree(g_disks); g_disks = NULL; g_n = 0; }
    g_gen++;
}

int diskmgr_is_open(void) { return g_open; }
int diskmgr_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }
win_geom_t* diskmgr_geom(void) { return &g_win; }

uint32_t diskmgr_signature(void) {
    if (!g_open) return 0;
    if (timer_ms() - g_check_ms >= 1000) {
        g_check_ms = timer_ms();
        if (!g_dlg && hw_signature() != g_hw_sig) rescan();
    }
    uint32_t blink = g_dlg ? (timer_ms() / 500) & 1 : 0;
    return (g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^ i18n_generation()) + blink;
}

void diskmgr_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    kbnav_begin(&g_nav);
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    frame(x, y, W, H, C_CHROME, C_FRAME);
    gfx_fill_rect(x + 1, y + 1, W - 2, TITLE_H + 1, C_WHITE);
    /* a disk with a pie of its space */
    frame(x + 7, y + 6, 14, 10, TCL(0xC8D2E0), TCL(0x3B4A5E));
    gfx_fill_rect(x + 9, y + 8, 6, 3, C_PRIM);
    gfx_fill_rect(x + 9, y + 12, 3, 2, 0x003CC35Au);
    gfx_draw_text(x + 28, y + 8, _("Disk Management"), C_TEXT, C_WHITE);
    win_draw_buttons(&g_win, 4, 12);
    gfx_fill_rect(x + 1, y + TITLE_H + 1, W - 2, 1, C_LINE);
    if (!g_disks) { gfx_draw_text(x + 10, y + 60, "out of memory", C_TEXT, C_CHROME); return; }
    draw_toolbar();
    draw_list();
    draw_disks();
    int sy = y + H - STATUS_H;
    gfx_fill_rect(x + 1, sy, W - 2, 1, C_LINE);
    if (g_status[0]) text_cut(x + 8, sy + 6, g_status, (W - 30) / 8, C_TEXT, C_CHROME);
    else {
        gfx_fill_rect(x + 8, sy + 6, 10, 10, C_UNALLOC);
        gfx_draw_text(x + 22, sy + 7, _("Unallocated"), C_DIM, C_CHROME);
        int lx = x + 30 + text_w(_("Unallocated"));
        gfx_fill_rect(lx, sy + 6, 10, 10, C_PRIM);
        gfx_draw_text(lx + 14, sy + 7, _("Primary partition"), C_DIM, C_CHROME);
    }
    draw_dialog();
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, C_ACCENT);
}

/* what is at (mx, my): 1 if something was chosen */
static int pick(int mx, int my) {
    /* the list */
    int lx = g_win.x + 6, ly = list_y() + 2 + LROW_H;
    if (inside(mx, my, lx, ly, g_win.w - 12, list_rows() * LROW_H)) {
        int di, pi;
        if (list_at(g_scroll_list + (my - ly) / LROW_H, &di, &pi)) { g_sd = di; g_sp = pi; g_sg = -1; return 1; }
        return 0;
    }
    /* the disks */
    int x = g_win.x + 6, y = disks_y(), w = g_win.w - 12;
    if (!inside(mx, my, x, y, w, disks_h())) return 0;
    int k = (my - y - 4) / (DROW_H + 6), i = g_scroll_disks + k;
    int ry = y + 4 + k * (DROW_H + 6);
    if (k < 0 || i >= g_n || my >= ry + DROW_H) return 0;
    dp_disk_t* d = &g_disks[i];
    g_sd = i;
    g_sp = g_sg = -1;
    if (mx < x + 8 + LABEL_W || d->cdrom) return 1;
    box_t boxes[DP_MAX_PARTS + DP_MAX_GAPS];
    int nb = disk_boxes(d, x + 8 + LABEL_W, w - LABEL_W - 16, boxes, DP_MAX_PARTS + DP_MAX_GAPS);
    for (int j = 0; j < nb; j++)
        if (mx >= boxes[j].x && mx < boxes[j].x + boxes[j].w) {
            if (boxes[j].kind == 0) g_sp = boxes[j].idx;
            else g_sg = boxes[j].idx;
        }
    return 1;
}

void diskmgr_click(int mx, int my) {
    if (!diskmgr_contains(mx, my)) return;
    int ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { diskmgr_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    kbnav_mouse(&g_nav);
    if (g_dlg) { dialog_click(mx, my); return; }
    int ty = g_win.y + TITLE_H + 4;
    for (int t = 0; t < T_COUNT; t++)
        if (g_tb_w[t] && inside(mx, my, g_tb_x[t], ty, g_tb_w[t], 22)) { if (tool_ok(t)) act(t); return; }
    g_status[0] = 0;
    pick(mx, my);
}

static void menu_cb(int id, void* arg) { (void)arg; act(id); }

void diskmgr_rclick(int mx, int my) {
    if (!diskmgr_contains(mx, my) || g_dlg) return;
    if (!pick(mx, my)) return;
    g_gen++;
    ctx_item_t items[8];
    int n = 0;
    if (g_sg >= 0) items[n++] = (ctx_item_t){ _("New volume..."), T_NEW, !can_new() };
    else if (g_sp >= 0) {
        items[n++] = (ctx_item_t){ _("Format..."), T_FORMAT, !can_format() };
        items[n++] = (ctx_item_t){ _("Delete..."), T_DELETE, !can_delete() };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ _("Mount"), T_MOUNT, !can_mount() };
        items[n++] = (ctx_item_t){ _("Unmount"), T_UNMOUNT, !can_unmount() };
    } else items[n++] = (ctx_item_t){ _("New table..."), T_INIT, !can_init() };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ _("Refresh"), T_REFRESH, 0 };
    ctxmenu_open(mx, my, items, n, menu_cb, NULL);
}

void diskmgr_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

void diskmgr_wheel(int mx, int my, int dz) {
    (void)mx;
    if (my < disks_y()) g_scroll_list += dz;
    else g_scroll_disks += dz > 0 ? 1 : -1;
    if (g_scroll_list < 0) g_scroll_list = 0;
    if (g_scroll_disks < 0) g_scroll_disks = 0;
    g_gen++;
}

/* typed keys: the dialog's fields */
void diskmgr_key(char c) {
    if (!g_open) return;
    g_gen++;
    if (!g_dlg) { if (c == 27) diskmgr_close(); return; }
    if (c == 27) { g_dlg = 0; return; }
    if (c == '\n' || c == '\r') { dialog_ok(); return; }
    if (c == '\t') { if (g_dlg == DLG_NEW) g_dlg_focus ^= 1; return; }
    char* f = g_dlg_focus == 0 && g_dlg == DLG_NEW ? g_dlg_size : (g_dlg == DLG_NEW || g_dlg == DLG_FORMAT) ? g_dlg_label : NULL;
    if (!f) return;
    int n = (int)strlen(f);
    if (c == 8 || c == 127) { if (n) f[n - 1] = 0; return; }
    if (f == g_dlg_size && (c < '0' || c > '9')) return;
    /* a FAT label: letters, digits, a few marks */
    if (f == g_dlg_label && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')) return;
    if (n < (f == g_dlg_size ? 9 : 11)) { f[n] = c; f[n + 1] = 0; }
}

int diskmgr_navkey(int code) {
    if (!g_open) return 0;
    g_gen++;
    if (g_dlg) {
        if (code == KB_ENTER) { dialog_ok(); return 1; }
        if (code == KB_ESC) { g_dlg = 0; return 1; }
        if (code == KB_TAB || code == KB_BACKTAB) { if (g_dlg == DLG_NEW) g_dlg_focus ^= 1; return 1; }
        if (code == KB_UP || code == KB_DOWN) {
            if (g_dlg == DLG_INIT) g_dlg_scheme = g_dlg_scheme == DP_MBR ? DP_GPT : DP_MBR;
            else g_dlg_fs = g_dlg_fs == DP_FAT32 ? DP_EXFAT : DP_FAT32;
            return 1;
        }
        return 0;
    }
    if (code == KB_UP || code == KB_DOWN) {
        int total = list_count(), cur = -1, k = 0;
        for (int i = 0; i < g_n; i++) {
            if (g_disks[i].cdrom) continue;
            for (int p = 0; p < g_disks[i].nparts; p++, k++) if (i == g_sd && p == g_sp) cur = k;
        }
        int nx = cur < 0 ? 0 : cur + (code == KB_UP ? -1 : 1);
        int di, pi;
        if (nx >= 0 && nx < total && list_at(nx, &di, &pi)) {
            g_sd = di; g_sp = pi; g_sg = -1;
            if (nx < g_scroll_list) g_scroll_list = nx;
            if (nx >= g_scroll_list + list_rows()) g_scroll_list = nx - list_rows() + 1;
        }
        return 1;
    }
    if (code == KB_DEL && can_delete()) { act(T_DELETE); return 1; }
    int cx, cy;
    int r = kbnav_key(&g_nav, code, &cx, &cy);
    if (r == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        diskmgr_click(cx, cy);
        g_nav.shown = shown;
        g_nav.focus = focus;
    }
    return r != 0;
}
