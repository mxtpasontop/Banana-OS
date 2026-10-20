/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "calpanel.h"
#include "i18n.h"
#include "theme.h"
#include "gfx.h"
#include "kbnav.h"
#include "kstring.h"
#include "rtc.h"

static const char* const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};
static const char* const DAYS[7] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

const char* cal_month_name(int month) { return month >= 1 && month <= 12 ? _(MONTHS[month - 1]) : ""; }
const char* cal_day_name(int wd) { return wd >= 0 && wd < 7 ? _(DAYS[wd]) : ""; }

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int cal_days_in_month(int year, int month) {
    static const uint8_t n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 30;
    return month == 2 && leap(year) ? 29 : n[month - 1];
}

/* Sakamoto's method: 0 Sunday, turned into 0 Monday */
int cal_weekday(int year, int month, int day) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year--;
    int w = (year + year / 4 - year / 100 + year / 400 + t[(month - 1) % 12] + day) % 7;
    return (w + 6) % 7;
}

/* ── the panel ── */
#define CELL_W  36
#define CELL_H  28
#define PW      (7 * CELL_W + 24)
#define TIME_H  64
#define MHEAD_H 30
#define WHEAD_H 22
#define PH      (TIME_H + MHEAD_H + WHEAD_H + 6 * CELL_H + 34)
#define C_BG    TC(0x1B212B)
#define C_LINE  TC(0x505E74)
#define C_TEXT  TC(0xE8EEF6)
#define C_DIM   TC(0x9AA6B6)
#define C_FAINT TC(0x566275)
#define C_TODAY TC(0x3A78D8)
#define C_SEL   TC(0x90B8F0)

static int g_open, g_px, g_py;
static int g_year, g_month;            /* the month shown */
static int g_sy, g_sm, g_sd;           /* the day picked */

static void today(int* y, int* m, int* d) {
    rtc_datetime_t dt;
    if (rtc_read_datetime(&dt) == 0 && dt.month >= 1 && dt.month <= 12) { *y = dt.year; *m = dt.month; *d = dt.day; }
    else { *y = 2026; *m = 1; *d = 1; }
}

int cal_is_open(void) { return g_open; }
void cal_close(void) { g_open = 0; }

static void go_today(void) {
    today(&g_sy, &g_sm, &g_sd);
    g_year = g_sy;
    g_month = g_sm;
}

void cal_toggle(int right_x, int bar_y) {
    if (g_open) { g_open = 0; return; }
    g_open = 1;
    g_px = right_x - PW;
    g_py = bar_y - PH - 6;
    if (g_px < 0) g_px = 0;
    if (g_py < 0) g_py = 0;
    go_today();
}

int cal_contains(int mx, int my) {
    return g_open && mx >= g_px && mx < g_px + PW && my >= g_py && my < g_py + PH;
}

static void month_step(int d) {
    g_month += d;
    while (g_month < 1) { g_month += 12; g_year--; }
    while (g_month > 12) { g_month -= 12; g_year++; }
}

/* the grid's first cell: the Monday on or before the 1st (y, m, d) */
static void first_cell(int* y, int* m, int* d) {
    int wd = cal_weekday(g_year, g_month, 1);
    *y = g_year;
    *m = g_month;
    *d = 1 - wd;
    if (*d < 1) {
        if (--*m < 1) { *m = 12; --*y; }
        *d += cal_days_in_month(*y, *m);
    }
}

static void next_day(int* y, int* m, int* d) {
    if (++*d > cal_days_in_month(*y, *m)) {
        *d = 1;
        if (++*m > 12) { *m = 1; ++*y; }
    }
}

static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t line) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_fill_rect(x, y, w, 1, line);
    gfx_fill_rect(x, y + h - 1, w, 1, line);
    gfx_fill_rect(x, y, 1, h, line);
    gfx_fill_rect(x + w - 1, y, 1, h, line);
}

static int grid_y(void) { return g_py + TIME_H + MHEAD_H + WHEAD_H; }

void cal_draw(void) {
    if (!g_open) return;
    int x = g_px, y = g_py;
    frame(x, y, PW, PH, C_BG, C_LINE);

    /* the time, big, and the date in words */
    rtc_datetime_t dt;
    int ty, tm, td;
    today(&ty, &tm, &td);
    char line[64];
    if (rtc_read_datetime(&dt) == 0) ksnprintf(line, sizeof(line), "%02u:%02u:%02u", dt.hour, dt.minute, dt.second);
    else kstrlcpy(line, "--:--:--", sizeof(line));
    gfx_draw_text_scaled(x + 14, y + 12, 3, line, TC(0xFFFFFF), C_BG);
    ksnprintf(line, sizeof(line), "%s %d %s %d", cal_day_name(cal_weekday(ty, tm, td)), td, cal_month_name(tm), ty);
    gfx_draw_text(x + 14, y + 44, line, TC(0x68A8F0), C_BG);
    gfx_fill_rect(x + 8, y + TIME_H - 4, PW - 16, 1, TC(0x384252));

    /* the month, and its arrows */
    int my = y + TIME_H;
    ksnprintf(line, sizeof(line), "%s %d", cal_month_name(g_month), g_year);
    gfx_draw_text(x + 14, my + 9, line, C_TEXT, C_BG);
    frame(x + PW - 66, my + 3, 24, 22, TC(0x262D38), TC(0x384252));
    frame(x + PW - 38, my + 3, 24, 22, TC(0x262D38), TC(0x384252));
    gfx_draw_text(x + PW - 58, my + 10, "^", C_TEXT, TC(0x262D38));
    gfx_draw_text(x + PW - 30, my + 10, "v", C_TEXT, TC(0x262D38));

    /* the days of the week */
    int wy = y + TIME_H + MHEAD_H;
    static const char* const WD[7] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    for (int i = 0; i < 7; i++) gfx_draw_text(x + 12 + i * CELL_W + (CELL_W - 16) / 2, wy + 6, _(WD[i]), C_DIM, C_BG);

    /* six weeks */
    int cy, cm, cd;
    first_cell(&cy, &cm, &cd);
    int gy = grid_y();
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 7; c++) {
            int bx = x + 12 + c * CELL_W, by = gy + r * CELL_H;
            int is_today = cy == ty && cm == tm && cd == td, sel = cy == g_sy && cm == g_sm && cd == g_sd;
            uint32_t bg = C_BG;
            if (is_today) { bg = C_TODAY; gfx_fill_rect(bx + 2, by + 2, CELL_W - 4, CELL_H - 4, bg); }
            if (sel && !is_today) frame(bx + 2, by + 2, CELL_W - 4, CELL_H - 4, C_BG, C_SEL);
            else if (sel) frame(bx + 2, by + 2, CELL_W - 4, CELL_H - 4, bg, TC(0xFFFFFF));
            char n[4];
            ksnprintf(n, sizeof(n), "%d", cd);
            uint32_t fg = is_today ? TC(0xFFFFFF) : cm == g_month ? C_TEXT : C_FAINT;
            gfx_draw_text(bx + (CELL_W - (int)strlen(n) * 8) / 2, by + 10, n, fg, bg);
            next_day(&cy, &cm, &cd);
        }

    /* the footer: back to today */
    int fy = y + PH - 26;
    gfx_fill_rect(x + 8, fy - 4, PW - 16, 1, TC(0x384252));
    gfx_draw_text(x + 14, fy + 6, _("Today"), TC(0x68A8F0), C_BG);
    gfx_draw_text(x + PW - 14 - 18 * 8, fy + 6, _("PgUp/PgDn: months"), C_DIM, C_BG);
}

void cal_click(int mx, int my) {
    if (!cal_contains(mx, my)) { g_open = 0; return; }
    int x = g_px, hy = g_py + TIME_H;
    if (my >= hy + 3 && my < hy + 25) {
        if (mx >= x + PW - 66 && mx < x + PW - 42) month_step(-1);
        else if (mx >= x + PW - 38 && mx < x + PW - 14) month_step(1);
        return;
    }
    int fy = g_py + PH - 26;
    if (my >= fy && mx < x + 14 + 6 * 8) { go_today(); return; }
    int gy = grid_y();
    if (my < gy || my >= gy + 6 * CELL_H || mx < x + 12 || mx >= x + 12 + 7 * CELL_W) return;
    int r = (my - gy) / CELL_H, c = (mx - x - 12) / CELL_W;
    int cy, cm, cd;
    first_cell(&cy, &cm, &cd);
    for (int i = 0; i < r * 7 + c; i++) next_day(&cy, &cm, &cd);
    g_sy = cy; g_sm = cm; g_sd = cd;
    if (cm != g_month) { g_year = cy; g_month = cm; }     /* a day of the month before / after: show it */
}

/* the picked day moved by n days; the month follows it */
static void move_days(int n) {
    while (n > 0) { next_day(&g_sy, &g_sm, &g_sd); n--; }
    while (n < 0) {
        if (--g_sd < 1) {
            if (--g_sm < 1) { g_sm = 12; g_sy--; }
            g_sd = cal_days_in_month(g_sy, g_sm);
        }
        n++;
    }
    g_year = g_sy;
    g_month = g_sm;
}

int cal_key(int code) {
    if (code == KB_ESC) g_open = 0;
    else if (code == KB_LEFT) move_days(-1);
    else if (code == KB_RIGHT) move_days(1);
    else if (code == KB_UP) move_days(-7);
    else if (code == KB_DOWN) move_days(7);
    else if (code == KB_PGUP || code == KB_PGDN) {
        month_step(code == KB_PGUP ? -1 : 1);
        g_sy = g_year; g_sm = g_month;
        if (g_sd > cal_days_in_month(g_sy, g_sm)) g_sd = cal_days_in_month(g_sy, g_sm);
    }
    else if (code == KB_HOME) go_today();
    else if (code == KB_ENTER) g_open = 0;
    return 1;
}
