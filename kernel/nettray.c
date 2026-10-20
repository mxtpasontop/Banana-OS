/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nettray.h"
#include "i18n.h"
#include "theme.h"
#include "gfx.h"
#include "kstring.h"
#include "timer.h"
#include "../net/net.h"

/* ── the connections ─────────────────────────────────────────────────
 * One entry per network adapter. A Wi-Fi driver will add its kind and
 * fill `signal`; the icon and the panel already draw both. */

enum { NT_WIRED, NT_USB, NT_WIFI };
enum { ST_OFF, ST_NO_LINK, ST_CONNECTING, ST_CONNECTED };   /* (ST_OFF: present, not in use) */

typedef struct {
    netdev_t* dev;
    int       kind;
    int       active;               /* the one the system uses (one at a time for now) */
    int       state;
    int       signal;               /* Wi-Fi: 0..4 bars */
} nt_conn_t;

#define NT_MAX 8

static int link_of(netdev_t* d) { return d->link_up ? d->link_up(d) != 0 : 1; }

static int collect(nt_conn_t* out, int max) {
    netif_t* nif = net_if();
    int n = 0;
    for (int i = 0; i < net_device_count() && n < max; i++) {
        netdev_t* d = net_device_at(i);
        if (!d || !d->present) continue;
        nt_conn_t* c = &out[n++];
        c->dev = d;
        c->kind = d->is_usb ? NT_USB : NT_WIRED;
        c->active = d == nif->dev;
        c->signal = 0;
        if (!c->active) c->state = ST_OFF;
        else if (!link_of(d)) c->state = ST_NO_LINK;
        else c->state = nif->configured ? ST_CONNECTED : ST_CONNECTING;
    }
    return n;
}

/* the state the icon shows: the active connection's, or none at all */
static int overall(nt_conn_t* c, int n, int* kind) {
    for (int i = 0; i < n; i++) if (c[i].active) { *kind = c[i].kind; return c[i].state; }
    *kind = NT_WIRED;
    return -1;
}

static const char* kind_name(int k) { return k == NT_WIFI ? "Wi-Fi" : k == NT_USB ? "USB Ethernet" : "Ethernet"; }

static const char* state_name(int s) {
    switch (s) {
    case ST_CONNECTED:  return "Connected";
    case ST_CONNECTING: return "Getting an address...";
    case ST_NO_LINK:    return "Cable unplugged";
    default:            return "Not in use";
    }
}

/* ── the icon ──────────────────────────────────────────────────────── */

static void px(int x, int y, uint32_t c) { gfx_fill_rect(x, y, 1, 1, c); }

/* a small red cross: no network */
static void cross(int x, int y, uint32_t c) {
    for (int i = 0; i < 6; i++) { px(x + i, y + i, c); px(x + i + 1, y + i, c); px(x + 5 - i, y + i, c); px(x + 6 - i, y + i, c); }
}

/* kind's glyph in 16x16 at x, y: a screen on its stand (wired), the same
 * with a plug (USB), or bars (Wi-Fi), lit by state */
static void draw_glyph(int x, int y, int kind, int state, int signal, uint32_t bg) {
    uint32_t frame = TC(0xC8D2DE), lit = TC(0x68A8F0), dim = TC(0x3A4556);
    uint32_t screen = state == ST_CONNECTED ? lit : state == ST_CONNECTING ? TC(0xC9A45C) : dim;
    (void)bg;
    if (kind == NT_WIFI) {
        for (int b = 0; b < 4; b++) {
            int h = 4 + b * 3;
            gfx_fill_rect(x + 1 + b * 4, y + 15 - h, 3, h, state == ST_CONNECTED && b < signal ? frame : dim);
        }
    } else {
        gfx_fill_rect(x + 1, y + 1, 14, 10, frame);          /* the screen */
        gfx_fill_rect(x + 2, y + 2, 12, 8, screen);
        gfx_fill_rect(x + 6, y + 11, 4, 2, frame);           /* its stand */
        gfx_fill_rect(x + 4, y + 13, 8, 2, frame);
        if (kind == NT_USB) { gfx_fill_rect(x + 12, y + 12, 4, 3, frame); px(x + 13, y + 15, frame); }
        if (state == ST_CONNECTING && (timer_ms() / 400) % 2) gfx_fill_rect(x + 5, y + 5, 6, 2, TC(0xFFFFFF));
    }
    if (state == ST_NO_LINK || state < 0) cross(x + 9, y + 8, TC(0xE05A4A));
}

void nettray_draw_icon(int x, int y, uint32_t bg, int hover) {
    nt_conn_t c[NT_MAX];
    int n = collect(c, NT_MAX), kind, st = overall(c, n, &kind);
    if (hover) gfx_fill_rect(x - 2, y - 3, NETTRAY_ICON_W, 22, TC(0x303A4A));
    int sig = 0;
    for (int i = 0; i < n; i++) if (c[i].active) sig = c[i].signal;
    draw_glyph(x, y, kind, st, sig, hover ? TC(0x303A4A) : bg);
}

void nettray_summary(char* out, int cap) {
    netif_t* nif = net_if();
    nt_conn_t c[NT_MAX];
    int n = collect(c, NT_MAX), kind, st = overall(c, n, &kind);
    if (st < 0) { kstrlcpy(out, n ? "Not connected" : "No network adapter", (size_t)cap); return; }
    if (st == ST_CONNECTED) {
        char ip[16];
        ip4_to_str(nif->ip, ip);
        ksnprintf(out, (size_t)cap, "%s - Connected, %s", nif->dev->ifname, ip);
    } else ksnprintf(out, (size_t)cap, "%s - %s", nif->dev->ifname, state_name(st));
}

/* ── the panel ─────────────────────────────────────────────────────── */

#define PW      320
#define LH      18
#define PAD     12
#define C_BG    TC(0x1D232C)
#define C_EDGE  TC(0x505E74)
#define C_TEXT  TC(0xE8EEF6)
#define C_DIM   TC(0x9AA6B6)
#define C_HEAD  TC(0xFFFFFF)
#define C_LINK  TC(0x68A8F0)
#define C_OK    TC(0x8FE3A1)
#define C_WAIT  TC(0xC9A45C)
#define C_BAD   TC(0xE07A6A)

static int g_open, g_right, g_bar_y, g_x, g_y, g_h;
static int g_hover = -1;                      /* the button under the mouse: an adapter's, or NT_MAX (settings) */

/* where the panel's buttons are this frame */
static struct { int x, y, w, h; netdev_t* dev; } g_btn[NT_MAX + 1];
static int g_nbtn;

static void fmt_bytes(uint32_t b, char* out, int cap) {
    if (b >= (10u << 20)) ksnprintf(out, (size_t)cap, "%u MB", b >> 20);
    else if (b >= (1u << 20)) ksnprintf(out, (size_t)cap, "%u.%u MB", b >> 20, ((b >> 10) & 1023) * 10 / 1024);
    else if (b >= 1024) ksnprintf(out, (size_t)cap, "%u KB", b >> 10);
    else ksnprintf(out, (size_t)cap, "%u bytes", b);
}

/* the lines an adapter takes in the panel */
static int conn_lines(const nt_conn_t* c) {
    if (c->active && c->state == ST_CONNECTED) return 9;
    return c->active ? 4 : 3;
}

static int panel_height(nt_conn_t* c, int n) {
    int h = PAD + LH + 8;                     /* the title, a rule */
    if (!n) h += LH * 2;
    for (int i = 0; i < n; i++) h += conn_lines(&c[i]) * LH + 8;
    return h + LH + PAD + 4;                  /* the settings link */
}

static void row(int x, int y, const char* k, const char* v) {
    gfx_draw_text(x, y, k, C_DIM, C_BG);
    gfx_draw_text(x + 15 * 8, y, v, C_TEXT, C_BG);
}

static void button(int i, int x, int y, const char* label, netdev_t* dev) {
    int w = (int)strlen(label) * 8 + 16;
    uint32_t base = g_hover == i ? TC(0x405478) : TC(0x303A4A);
    gfx_fill_rect(x, y - 3, w, LH + 2, base);
    gfx_fill_rect(x, y - 3, w, 1, TC(0x6B7892));
    gfx_draw_text(x + 8, y, label, C_TEXT, base);
    if (g_nbtn < NT_MAX + 1) {
        g_btn[g_nbtn].x = x; g_btn[g_nbtn].y = y - 3; g_btn[g_nbtn].w = w; g_btn[g_nbtn].h = LH + 2;
        g_btn[g_nbtn].dev = dev;
        g_nbtn++;
    }
}

void nettray_draw(void) {
    if (!g_open) return;
    nt_conn_t c[NT_MAX];
    int n = collect(c, NT_MAX);
    netif_t* nif = net_if();
    g_h = panel_height(c, n);
    g_x = g_right - PW;
    if (g_x < 4) g_x = 4;
    g_y = g_bar_y - 4 - g_h;
    if (g_y < 4) g_y = 4;
    int x = g_x + PAD, y = g_y + PAD;
    gfx_fill_rect(g_x + 4, g_y + 4, PW, g_h, TC(0x080A0E));    /* a shadow */
    gfx_fill_rect(g_x, g_y, PW, g_h, C_BG);
    gfx_fill_rect(g_x, g_y, PW, 1, C_EDGE); gfx_fill_rect(g_x, g_y + g_h - 1, PW, 1, C_EDGE);
    gfx_fill_rect(g_x, g_y, 1, g_h, C_EDGE); gfx_fill_rect(g_x + PW - 1, g_y, 1, g_h, C_EDGE);
    gfx_draw_text(x, y, _("Network"), C_HEAD, C_BG);
    y += LH;
    gfx_fill_rect(x, y + 2, PW - 2 * PAD, 1, TC(0x343E4E));
    y += 8;
    g_nbtn = 0;
    if (!n) {
        gfx_draw_text(x, y, _("No network adapter found."), C_TEXT, C_BG);
        gfx_draw_text(x, y + LH, _("Plug one in, or see the README."), C_DIM, C_BG);
        y += LH * 2;
    }
    char t[64], v[48];
    for (int i = 0; i < n; i++) {
        nt_conn_t* k = &c[i];
        netdev_t* d = k->dev;
        draw_glyph(x, y, k->kind, k->active ? k->state : ST_OFF, k->signal, C_BG);
        ksnprintf(t, sizeof(t), "%s  %s", kind_name(k->kind), d->ifname);
        gfx_draw_text(x + 24, y, t, C_HEAD, C_BG);
        uint32_t sc = k->state == ST_CONNECTED ? C_OK : k->state == ST_CONNECTING ? C_WAIT
                    : k->state == ST_NO_LINK ? C_BAD : C_DIM;
        const char* st = state_name(k->state);
        gfx_draw_text(g_x + PW - PAD - (int)strlen(st) * 8, y + LH, st, sc, C_BG);
        y += LH;
        kstrlcpy(t, d->model ? d->model : d->name, sizeof(t));
        if ((int)strlen(t) > 22) { t[21] = '.'; t[22] = 0; }
        gfx_draw_text(x + 24, y, t, C_DIM, C_BG);
        y += LH;
        if (k->active && k->state == ST_CONNECTED) {
            char a[16];
            ip4_to_str(nif->ip, a);      row(x + 24, y, "IPv4 address", a); y += LH;
            ip4_to_str(nif->netmask, a); row(x + 24, y, "Subnet mask", a);  y += LH;
            ip4_to_str(nif->gateway, a); row(x + 24, y, "Gateway", nif->gateway ? a : "none"); y += LH;
            ip4_to_str(nif->dns, a);     row(x + 24, y, "DNS server", nif->dns ? a : "none"); y += LH;
            if (nif->dhcp) ksnprintf(v, sizeof(v), "DHCP (%u h lease)", nif->lease_s / 3600);
            else kstrlcpy(v, "set by hand", sizeof(v));
            row(x + 24, y, "Address from", v); y += LH;
        }
        ksnprintf(v, sizeof(v), "%02x:%02x:%02x:%02x:%02x:%02x", d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        if (k->active) {
            row(x + 24, y, "MAC address", v); y += LH;
            char s1[16], s2[16];
            fmt_bytes(d->tx_bytes, s1, sizeof(s1));
            fmt_bytes(d->rx_bytes, s2, sizeof(s2));
            ksnprintf(v, sizeof(v), "%s / %s", s1, s2);
            row(x + 24, y, "Sent / recvd", v); y += LH;
        } else {
            button(i, x + 24, y + 2, _("Use this one"), d);
            y += LH;
        }
        y += 8;
    }
    gfx_fill_rect(x, y - 4, PW - 2 * PAD, 1, TC(0x343E4E));
    y += 4;
    int sx = x, sw = 18 * 8;
    gfx_draw_text(sx, y, _("Network settings..."), g_hover == NT_MAX ? C_HEAD : C_LINK, C_BG);
    gfx_fill_rect(sx, y + 15, sw + 8, 1, g_hover == NT_MAX ? C_HEAD : C_LINK);
    if (g_nbtn < NT_MAX + 1) {
        g_btn[g_nbtn].x = sx; g_btn[g_nbtn].y = y - 2; g_btn[g_nbtn].w = sw + 8; g_btn[g_nbtn].h = LH;
        g_btn[g_nbtn].dev = NULL;
        g_nbtn++;
    }
}

void nettray_toggle(int right_x, int bar_y) {
    g_open = !g_open;
    g_right = right_x;
    g_bar_y = bar_y;
    g_hover = -1;
}

void nettray_close(void) { g_open = 0; g_hover = -1; }
int  nettray_is_open(void) { return g_open; }

int nettray_contains(int mx, int my) {
    return g_open && mx >= g_x && mx < g_x + PW && my >= g_y && my < g_y + g_h;
}

static int button_at(int mx, int my) {
    for (int i = 0; i < g_nbtn; i++)
        if (mx >= g_btn[i].x && mx < g_btn[i].x + g_btn[i].w && my >= g_btn[i].y && my < g_btn[i].y + g_btn[i].h) return i;
    return -1;
}

void nettray_hover(int mx, int my) {
    if (!g_open) return;
    int b = button_at(mx, my);
    g_hover = b < 0 ? -1 : g_btn[b].dev ? b : NT_MAX;
}

int nettray_click(int mx, int my) {
    int b = button_at(mx, my);
    if (b < 0) return 0;
    if (!g_btn[b].dev) { nettray_close(); return NETTRAY_SETTINGS; }
    net_select_device(g_btn[b].dev);                 /* (DHCP on it at once) */
    return 0;
}

uint32_t nettray_sig(void) {
    nt_conn_t c[NT_MAX];
    int n = collect(c, NT_MAX);
    uint32_t s = (uint32_t)n * 131u + (uint32_t)g_open * 7u + (uint32_t)(g_hover + 1) * 17u + net_if()->ip;
    for (int i = 0; i < n; i++) s = s * 31u + (uint32_t)c[i].state * 5u + (uint32_t)c[i].active;
    if (g_open) s += timer_ms() / 1000u;               /* (the traffic counters, once a second) */
    else for (int i = 0; i < n; i++) if (c[i].state == ST_CONNECTING) s += timer_ms() / 400u;   /* (the blinking) */
    return s;
}
