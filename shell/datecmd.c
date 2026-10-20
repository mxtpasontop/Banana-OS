/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "datecmd.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/rtc.h"
#include "../kernel/tz.h"
#include "../kernel/calpanel.h"

/* `date` and `timezone`: the clock, set by hand, and the time zone */

static void say(const char* s) { terminal_writeln(s); }

static const char* const WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* "2026-10-17 18:30:05 Saturday" */
static void fmt_time(uint32_t t, char* out, int cap) {
    rtc_datetime_t d;
    rtc_split_time(t, &d);
    ksnprintf(out, (size_t)cap, "%04u-%02u-%02u %02u:%02u:%02u %s", d.year, d.month, d.day, d.hour, d.minute, d.second,
              WD[(t / 86400u + 4u) % 7u]);
}

static void zone_line(char* out, int cap) {
    int z = tz_current();
    const tz_zone_t* zz = tz_get(z);
    uint32_t utc = rtc_now_utc();
    char off[16];
    tz_format_offset(tz_offset_at(z, utc), off, sizeof(off));
    ksnprintf(out, (size_t)cap, "%s (%s, %s%s)", zz ? zz->label : "UTC", zz ? zz->id : "UTC", off,
              tz_is_summer(z, utc) ? ", summer time" : "");
}

static void show(int utc_too) {
    uint32_t t = rtc_now();
    if (!t) { say("date: the clock could not be read"); return; }
    char a[48], z[128], line[200];
    fmt_time(t, a, sizeof(a));
    zone_line(z, sizeof(z));
    ksnprintf(line, sizeof(line), "%s  %s", a, z);
    say(line);
    if (utc_too) {
        fmt_time(rtc_now_utc(), a, sizeof(a));
        ksnprintf(line, sizeof(line), "%s  UTC", a);
        say(line);
        ksnprintf(line, sizeof(line), "The computer's clock keeps %s.", rtc_keeps_utc() ? "UTC (as Linux does)" : "the local time (as Windows does)");
        say(line);
    }
}

/* "2026-10-17" or "17/10/2026": 0, -1 */
static int parse_date(const char* s, int* y, int* m, int* d) {
    uint32_t a = 0, b = 0, c = 0;
    int n = k_parse_u32(s, &a);
    if (!n || (s[n] != '-' && s[n] != '/' && s[n] != '.')) return -1;
    char sep = s[n];
    s += n + 1;
    n = k_parse_u32(s, &b);
    if (!n || s[n] != sep) return -1;
    s += n + 1;
    n = k_parse_u32(s, &c);
    if (!n || s[n]) return -1;
    if (sep == '-') { *y = (int)a; *m = (int)b; *d = (int)c; }      /* year-month-day */
    else { *d = (int)a; *m = (int)b; *y = (int)c; }                 /* day/month/year */
    if (*m < 1 || *m > 12 || *d < 1 || *d > 31 || *y < 2000 || *y > 2099) return -1;
    static const uint8_t D[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (*d > D[*m - 1] || (*m == 2 && *d == 29 && !((*y % 4 == 0 && *y % 100 != 0) || *y % 400 == 0))) return -1;
    return 0;
}

/* "18:30" or "18:30:15": 0, -1 */
static int parse_time(const char* s, int* h, int* mi, int* se) {
    uint32_t a = 0, b = 0, c = 0;
    int n = k_parse_u32(s, &a);
    if (!n || s[n] != ':') return -1;
    s += n + 1;
    n = k_parse_u32(s, &b);
    if (!n) return -1;
    s += n;
    if (*s == ':') {
        s++;
        n = k_parse_u32(s, &c);
        if (!n) return -1;
        s += n;
    }
    if (*s || a > 23 || b > 59 || c > 59) return -1;
    *h = (int)a; *mi = (int)b; *se = (int)c;
    return 0;
}

int date_set_text(const char* date, const char* time, char* err, int cap) {
    rtc_datetime_t now;
    if (rtc_read_datetime(&now) != 0) { now.year = 2026; now.month = 1; now.day = 1; now.hour = now.minute = now.second = 0; }
    int y = now.year, m = now.month, d = now.day, h = now.hour, mi = now.minute, se = now.second;
    if (date && *date && parse_date(date, &y, &m, &d) != 0) {
        ksnprintf(err, (size_t)cap, "not a date: %s (2026-10-17, or 17/10/2026; 2000-2099)", date);
        return -1;
    }
    if (time && *time && parse_time(time, &h, &mi, &se) != 0) {
        ksnprintf(err, (size_t)cap, "not a time: %s (18:30, or 18:30:15)", time);
        return -1;
    }
    if (rtc_set_time(rtc_make_time(y, m, d, h, mi, se)) != 0) {
        ksnprintf(err, (size_t)cap, "the clock could not be set (it holds 2000-2099)");
        return -1;
    }
    return 0;
}

static int cmd_date(int argc, char** argv) {
    if (argc == 1) { show(0); return 1; }
    if (!strcmp(argv[1], "-u") || (!strcmp(argv[1], "utc") && argc == 2)) {
        char a[48], line[64];
        fmt_time(rtc_now_utc(), a, sizeof(a));
        ksnprintf(line, sizeof(line), "%s  UTC", a);
        say(line);
        return 1;
    }
    if (!strcmp(argv[1], "-a") || !strcmp(argv[1], "all")) { show(1); return 1; }
    if (!strcmp(argv[1], "set") && argc >= 3) {
        const char* date = NULL, * time = NULL;
        for (int i = 2; i < argc && i < 4; i++) {
            if (strchr(argv[i], ':')) time = argv[i];
            else date = argv[i];
        }
        char err[96];
        if (date_set_text(date, time, err, sizeof(err)) != 0) {
            terminal_write("date: ");
            say(err);
            return 1;
        }
        show(0);
        return 1;
    }
    if (!strcmp(argv[1], "utc") && argc == 3 && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        rtc_set_keeps_utc(!strcmp(argv[2], "on"));
        say(rtc_keeps_utc() ? "The computer's clock keeps UTC from now on (as Linux does)."
                            : "The computer's clock keeps the local time from now on (as Windows does).");
        show(0);
        return 1;
    }
    say("usage: date [-u | -a] | date set [YYYY-MM-DD] [HH:MM[:SS]] | date utc on|off");
    return 1;
}

/* the zones whose name or places hold every word of q */
static int matches(const tz_zone_t* z, const char* q) {
    char w[48];
    while (*q) {
        while (*q == ' ') q++;
        int n = 0;
        while (q[n] && q[n] != ' ' && n < 47) { w[n] = q[n]; n++; }
        w[n] = 0;
        q += n;
        if (!n) break;
        int hit = 0;
        for (const char* s = z->id; *s && !hit; s++) if (!strncasecmp(s, w, (size_t)n)) hit = 1;
        for (const char* s = z->label; *s && !hit; s++) if (!strncasecmp(s, w, (size_t)n)) hit = 1;
        if (!hit) return 0;
    }
    return 1;
}

static void list(const char* q) {
    char line[128], off[16];
    uint32_t utc = rtc_now_utc();
    for (int i = 0; i < tz_count(); i++) {
        const tz_zone_t* z = tz_get(i);
        if (q && *q && !matches(z, q)) continue;
        tz_format_offset(tz_offset_at(i, utc), off, sizeof(off));
        ksnprintf(line, sizeof(line), "%c %-31s %-10s %s", i == tz_current() ? '*' : ' ', z->id, off, z->label);
        say(line);
    }
}

static int cmd_timezone(int argc, char** argv) {
    char line[160];
    if (argc == 1) {
        zone_line(line, sizeof(line));
        say(line);
        return 1;
    }
    char q[96] = "";
    for (int i = (strcmp(argv[1], "list") ? 1 : 2); i < argc; i++) {
        if (q[0]) kstrlcat(q, " ", sizeof(q));
        kstrlcat(q, argv[i], sizeof(q));
    }
    if (!strcmp(argv[1], "list")) { list(q); return 1; }
    int pick = tz_find(q);
    if (pick < 0) {
        int n = 0;
        for (int i = 0; i < tz_count(); i++) if (matches(tz_get(i), q)) { pick = i; n++; }
        if (n != 1) {
            if (n) ksnprintf(line, sizeof(line), "timezone: %d zones match '%s' - say which:", n, q);
            else ksnprintf(line, sizeof(line), "timezone: no zone matches '%s' (`timezone list` shows them)", q);
            say(line);
            if (n) list(q);
            return 1;
        }
    }
    tz_set(pick);
    zone_line(line, sizeof(line));
    terminal_write("Time zone: ");
    say(line);
    show(0);
    return 1;
}

static int num(const char* t) { uint32_t v = 0; return k_parse_u32(t, &v) && v < 100000 ? (int)v : -1; }

/* cal [month [year]] | cal year: a month as a calendar, weeks from Monday,
 * today in reverse (like Linux's cal -m) */
static int cmd_cal(int argc, char** argv) {
    rtc_datetime_t now;
    int ty = 2026, tm = 1, td = 0;
    if (rtc_read_datetime(&now) == 0) { ty = now.year; tm = now.month; td = now.day; }
    int y = ty, m = tm;
    if (argc == 2) {
        int v = num(argv[1]);
        if (v >= 1 && v <= 12) m = v;
        else if (v > 12) { y = v; m = 0; }
        else { terminal_writeln("usage: cal [month [year]] | cal year"); return 1; }
    } else if (argc >= 3) {
        m = num(argv[1]);
        y = num(argv[2]);
        if (m < 1 || m > 12 || y < 1) { terminal_writeln("usage: cal [month [year]] | cal year"); return 1; }
    }
    for (int mm = m ? m : 1; mm <= (m ? m : 12); mm++) {
        char head[32];
        ksnprintf(head, sizeof(head), "%s %d", cal_month_name(mm), y);
        int pad = (20 - (int)strlen(head)) / 2;
        for (int i = 0; i < pad; i++) terminal_putchar(' ');
        terminal_writeln(head);
        terminal_writeln("Mo Tu We Th Fr Sa Su");
        int wd = cal_weekday(y, mm, 1), n = cal_days_in_month(y, mm);
        for (int i = 0; i < wd; i++) terminal_write("   ");
        for (int d = 1; d <= n; d++) {
            char t[4];
            ksnprintf(t, sizeof(t), "%2d", d);
            if (y == ty && mm == tm && d == td) terminal_write_color(t, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
            else terminal_write(t);
            if ((wd + d) % 7 == 0) terminal_putchar('\n');
            else if (d < n) terminal_putchar(' ');
        }
        if ((wd + n) % 7) terminal_putchar('\n');
        if (!m && mm < 12) terminal_putchar('\n');
    }
    return 1;
}

int datecmd_dispatch(int argc, char** argv) {
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "cal")) return cmd_cal(argc, argv);
    if (!strcmp(argv[0], "date")) return cmd_date(argc, argv);
    if (!strcmp(argv[0], "timezone") || !strcmp(argv[0], "tz")) return cmd_timezone(argc, argv);
    return 0;
}
