/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "theme.h"
#include "kstring.h"
#include "kheap.h"
#include "fs.h"
#include "zip.h"
#include "settings.h"
#include "wallpaper.h"

const char* const TM_STYLE_KEYS[TM_STYLES] = { "banana", "classic", "windows10", "windows11" };
const char* const TM_STYLE_NAMES[TM_STYLES] = { "Banana OS", "Windows classic", "Windows 10", "Windows 11" };

/* ── the built-in themes ── */
static const theme_t BUILTIN[] = {
    /* Banana Dark: the desktop's own palette (TC() changes nothing) */
    { "Banana Dark", "Banana OS", "", { 0x000000, 0x10141C, 0x1B212B, 0x303740, 0x505E74, 0x9AA6B6, 0xE8EEF6, 0xFFFFFF }, 0x3A78D8, 0, TM_STYLE_WIN10, "" },
    { "Light", "Banana OS", "", { 0xA8A8A8, 0xFFFFFF, 0xF3F3F3, 0xE3E3E3, 0xC4C4C4, 0x5C5C5C, 0x1B1B1B, 0x000000 }, 0x005FB8, 1, TM_STYLE_WIN10, "" },
    { "Midnight", "Banana OS", "", { 0x000000, 0x0A0A0A, 0x161616, 0x262626, 0x434343, 0x9C9C9C, 0xEDEDED, 0xFFFFFF }, 0x4CC2FF, 0, TM_STYLE_WIN11, "" },
    { "Ocean", "Banana OS", "Ocean Wave", { 0x000000, 0x0B1A20, 0x10262E, 0x1C3A44, 0x2F5A66, 0x8FB3BD, 0xE3F4F7, 0xFFFFFF }, 0x2EC4B6, 0, TM_STYLE_WIN11, "" },
    { "Forest", "Banana OS", "Forest Night", { 0x000000, 0x0E1610, 0x16221A, 0x24362A, 0x3E5A46, 0x9DB5A3, 0xE6F2E8, 0xFFFFFF }, 0x5CC25C, 0, TM_STYLE_BANANA, "" },
    { "Banana", "Banana OS", "", { 0x000000, 0x17140C, 0x221E12, 0x36301E, 0x5C5232, 0xB8AE8E, 0xF6F0DA, 0xFFFFFF }, 0xE8B820, 0, TM_STYLE_BANANA, "" },
    { "Sakura", "Banana OS", "", { 0xB89AA4, 0xFFFFFF, 0xFDF2F5, 0xF5DCE4, 0xE0B8C6, 0x7A5562, 0x2E1A22, 0x000000 }, 0xD94A7A, 1, TM_STYLE_WIN11, "" },
    { "High Contrast", "Banana OS", "Plain Black", { 0x000000, 0x000000, 0x000000, 0x1A1A1A, 0xFFFFFF, 0xFFFF00, 0xFFFFFF, 0xFFFFFF }, 0x00FFFF, 0, TM_STYLE_CLASSIC, "" },
};
#define NBUILTIN ((int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

static theme_t  g_themes[THEME_MAX];
static int      g_n, g_cur, g_on;
static int      g_tm_override = -1;           /* Task Manager's look chosen by hand, -1 the theme's */
static uint32_t g_gen = 1;

/* the default palette's shades, by their lightness (0..255): the anchors a
 * colour is placed between */
static int g_anchor_l[TH_SHADES];
static int g_acc_l;

static int lum(uint32_t c) {
    return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000);
}

static uint32_t mix(uint32_t a, uint32_t b, int t) {        /* t: 0..256 from a to b */
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(256 - t) + ((b >> 16) & 255) * (uint32_t)t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(256 - t) + ((b >> 8) & 255) * (uint32_t)t) >> 8;
    uint32_t c = ((a & 255) * (uint32_t)(256 - t) + (b & 255) * (uint32_t)t) >> 8;
    return r << 16 | g << 8 | c;
}

/* ── the mapping, cached per colour ── */
#define CACHE 2048
static uint32_t g_ck[CACHE], g_cv[CACHE];
static uint8_t  g_cu[CACHE];

static uint32_t map_color(uint32_t c, int light_design) {
    const theme_t* t = &g_themes[g_cur];
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int chroma = mx - mn, l = lum(c);
    if (chroma >= 70) {
        /* a blue: the accent, as much lighter or darker as it was than the default accent */
        if (b == mx && b - r >= 60) {
            int d = l - g_acc_l;
            return d >= 0 ? mix(t->accent, 0xFFFFFF, d * 256 / (256 - g_acc_l)) : mix(t->accent, 0x000000, -d * 256 / g_acc_l);
        }
        return c;                                           /* red, green, yellow...: what they say */
    }
    /* a grey (or a greyish blue): between the two shades around its lightness */
    if (light_design) {
        /* (a window drawn light - Files, the Start menu: white its base, dark its text) */
        static const int LL[7] = { 255, 248, 229, 211, 109, 30, 0 };
        static const int LS[7] = { TH_BASE, TH_SURFACE, TH_RAISED, TH_LINE, TH_DIM, TH_TEXT, TH_BRIGHT };
        for (int i = 0; i < 6; i++) {
            int a = LL[i], z = LL[i + 1];
            if (l >= z || i == 5) {
                if (l >= a) return t->shade[LS[i]];
                return mix(t->shade[LS[i]], t->shade[LS[i + 1]], (a - l) * 256 / (a - z));
            }
        }
        return t->shade[TH_TEXT];
    }
    for (int i = 0; i < TH_SHADES - 1; i++) {
        int a = g_anchor_l[i], z = g_anchor_l[i + 1];
        if (l <= z || i == TH_SHADES - 2) {
            if (l <= a) return t->shade[i];
            return mix(t->shade[i], t->shade[i + 1], (l - a) * 256 / (z - a ? z - a : 1));
        }
    }
    return t->shade[TH_TEXT];
}

static uint32_t lookup(uint32_t c, int light_design) {
    c &= 0xFFFFFFu;
    if (light_design) c |= 1u << 24;
    uint32_t h = (c * 2654435761u) >> 21 & (CACHE - 1);
    for (int k = 0; k < 16; k++, h = (h + 1) & (CACHE - 1)) {
        if (g_cu[h] && g_ck[h] == c) return g_cv[h];
        if (!g_cu[h]) {
            uint32_t v = map_color(c & 0xFFFFFFu, light_design);
            g_ck[h] = c; g_cv[h] = v; g_cu[h] = 1;
            return v;
        }
    }
    return map_color(c & 0xFFFFFFu, light_design);          /* (the table is crowded: not cached) */
}

uint32_t theme_color(uint32_t c) { return g_on ? lookup(c, 0) : c; }
uint32_t theme_color_light(uint32_t c) { return g_on ? lookup(c, 1) : c; }

/* ── theme files ── */
static int hexval(const char* s, uint32_t* out) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int n = 0;
    for (; n < 6; n++) {
        char ch = s[n];
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uint32_t)d;
    }
    if (n != 6) return -1;
    *out = v;
    return 0;
}

static const char* const SHADE_KEYS[TH_SHADES] = { "shadow", "base", "surface", "raised", "line", "text_dim", "text", "bright" };

int theme_parse(const char* text, uint32_t len, theme_t* t, char* err, int cap) {
    memset(t, 0, sizeof(*t));
    t->taskmgr = TM_STYLE_WIN10;                  /* (the default look) */
    int have[TH_SHADES] = { 0 }, have_acc = 0;
    uint32_t p = 0;
    int lineno = 0;
    while (p < len) {
        uint32_t e = p;
        while (e < len && text[e] != '\n') e++;
        lineno++;
        char line[160];
        uint32_t n = e - p < sizeof(line) - 1 ? e - p : sizeof(line) - 1;
        memcpy(line, text + p, n);
        line[n] = 0;
        p = e + 1;
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        int l = (int)strlen(s);
        while (l > 0 && (s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char* eq = strchr(s, '=');
        if (!eq) { ksnprintf(err, (size_t)cap, "line %d: no '=' (key=value)", lineno); return -1; }
        *eq = 0;
        char* k = s;
        char* v = eq + 1;
        int kl = (int)strlen(k);
        while (kl > 0 && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (!strcasecmp(k, "name")) { kstrlcpy(t->name, v, sizeof(t->name)); continue; }
        if (!strcasecmp(k, "author")) { kstrlcpy(t->author, v, sizeof(t->author)); continue; }
        if (!strcasecmp(k, "wallpaper")) { kstrlcpy(t->wallpaper, v, sizeof(t->wallpaper)); continue; }
        if (!strcasecmp(k, "mode")) { t->light = !strcasecmp(v, "light"); continue; }
        if (!strcasecmp(k, "description")) continue;
        if (!strcasecmp(k, "taskmgr")) {
            int st = theme_taskmgr_style_find(v);
            if (st < 0) { ksnprintf(err, (size_t)cap, "line %d: taskmgr= is banana, classic, windows10 or windows11", lineno); return -1; }
            t->taskmgr = st;
            continue;
        }
        uint32_t col;
        int which = -1;
        for (int i = 0; i < TH_SHADES; i++) if (!strcasecmp(k, SHADE_KEYS[i])) which = i;
        if (which < 0 && strcasecmp(k, "accent")) { ksnprintf(err, (size_t)cap, "line %d: unknown key '%s'", lineno, k); return -1; }
        if (hexval(v, &col) < 0) { ksnprintf(err, (size_t)cap, "line %d: '%s' is not a colour (#RRGGBB)", lineno, v); return -1; }
        if (which < 0) { t->accent = col; have_acc = 1; }
        else { t->shade[which] = col; have[which] = 1; }
    }
    if (!t->name[0]) { ksnprintf(err, (size_t)cap, "no name= line"); return -1; }
    static const char* const NEED[] = { "base", "surface", "raised", "line", "text_dim", "text" };
    for (int i = TH_BASE; i <= TH_TEXT; i++)
        if (!have[i]) { ksnprintf(err, (size_t)cap, "%s= is missing", NEED[i - TH_BASE]); return -1; }
    if (!have_acc) { ksnprintf(err, (size_t)cap, "accent= is missing"); return -1; }
    /* light or dark: by its text against its background */
    int light = lum(t->shade[TH_BASE]) > lum(t->shade[TH_TEXT]);
    if (!have[TH_SHADOW]) t->shade[TH_SHADOW] = light ? mix(t->shade[TH_BASE], 0, 90) : 0x000000;
    if (!have[TH_BRIGHT]) t->shade[TH_BRIGHT] = light ? 0x000000 : 0xFFFFFF;
    t->light = light;
    return 0;
}

static void scan_dir(void) {
    static int idx[64];
    int n = fs_list_files(THEME_DIR, idx, 64);
    for (int i = 0; i < n && g_n < THEME_MAX; i++) {
        fs_file_t* f = fs_get_file(idx[i]);
        if (!f || !f->content) continue;
        int l = (int)strlen(f->name);
        if (l < 7 || strcasecmp(f->name + l - 6, ".theme")) continue;
        theme_t t;
        char err[64];
        if (theme_parse(f->content, f->size, &t, err, sizeof(err)) < 0) continue;
        ksnprintf(t.path, sizeof(t.path), "%s/%s", THEME_DIR, f->name);
        int dup = theme_find(t.name);
        if (dup >= NBUILTIN) g_themes[dup] = t;          /* (installed again: the new one) */
        else if (dup < 0) g_themes[g_n++] = t;
    }
}

void theme_init(void) {
    for (int i = 0; i < TH_SHADES; i++) g_anchor_l[i] = lum(BUILTIN[0].shade[i]);
    g_acc_l = lum(BUILTIN[0].accent);
    g_n = 0;
    for (int i = 0; i < NBUILTIN; i++) g_themes[g_n++] = BUILTIN[i];
    scan_dir();
    char v[40];
    g_tm_override = settings_get("taskmgr_style", v, sizeof(v)) ? theme_taskmgr_style_find(v) : -1;
    int i = settings_get("theme", v, sizeof(v)) ? theme_find(v) : -1;
    g_cur = i >= 0 ? i : 0;
    g_on = g_cur != 0;
    memset(g_cu, 0, sizeof(g_cu));
    g_gen++;
}

int theme_count(void) { return g_n; }
const theme_t* theme_get(int i) { return i >= 0 && i < g_n ? &g_themes[i] : NULL; }
int theme_current(void) { return g_cur; }
uint32_t theme_generation(void) { return g_gen; }

int theme_taskmgr_style_find(const char* key) {
    static const char* const ALT[TM_STYLES][3] = {
        { "banana", "bananaos", "banana os" }, { "classic", "windows", "windows classic" },
        { "windows10", "win10", "10" }, { "windows11", "win11", "11" },
    };
    for (int s = 0; s < TM_STYLES; s++)
        for (int k = 0; k < 3; k++) if (!strcasecmp(key, ALT[s][k])) return s;
    return -1;
}

int theme_taskmgr_override(void) { return g_tm_override; }

int theme_taskmgr_style(void) {
    if (g_tm_override >= 0) return g_tm_override;
    const theme_t* t = theme_get(g_cur);
    return t ? t->taskmgr : TM_STYLE_WIN10;
}

void theme_set_taskmgr_override(int style) {
    g_tm_override = style >= 0 && style < TM_STYLES ? style : -1;
    settings_set("taskmgr_style", g_tm_override >= 0 ? TM_STYLE_KEYS[g_tm_override] : "theme");
    g_gen++;
}

int theme_find(const char* name) {
    for (int i = 0; i < g_n; i++) if (!strcasecmp(g_themes[i].name, name)) return i;
    return -1;
}

int theme_apply(int i) {
    if (i < 0 || i >= g_n) return -1;
    g_cur = i;
    g_on = i != 0;
    memset(g_cu, 0, sizeof(g_cu));
    settings_set("theme", g_themes[i].name);
    const char* w = g_themes[i].wallpaper;
    if (w[0]) {
        int p = wallpaper_find_preset(w);
        if (p >= 0) wallpaper_set_preset(p);
        else {
            char path[160], err[64];
            /* (a picture beside an installed theme's file, or a path) */
            if (w[0] != '/' && g_themes[i].path[0]) {
                kstrlcpy(path, g_themes[i].path, sizeof(path));
                char* sl = strrchr(path, '/');
                if (sl) { sl[1] = 0; kstrlcat(path, w, sizeof(path)); }
            } else kstrlcpy(path, w, sizeof(path));
            wallpaper_set_file(path, IMAGE_FILL, err, sizeof(err));
        }
    }
    g_gen++;
    return 0;
}

/* ── installing ── */
static int ends_with(const char* s, const char* e) {
    int a = (int)strlen(s), b = (int)strlen(e);
    return a >= b && !strcasecmp(s + a - b, e);
}

static int install_file(const char* path, char* err, int cap) {
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) { ksnprintf(err, (size_t)cap, "cannot read %s", path); return -1; }
    theme_t t;
    if (theme_parse(f->content, f->size, &t, err, cap) < 0) return -1;
    char dst[FS_PATH_LEN];
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    ksnprintf(dst, sizeof(dst), "%s/%s", THEME_DIR, base);
    fs_mkdir_p(THEME_DIR);
    if (strcmp(dst, path) && fs_write_path(dst, f->content, f->size) < 0) { ksnprintf(err, (size_t)cap, "cannot write %s", dst); return -1; }
    char name[32];
    kstrlcpy(name, t.name, sizeof(name));
    int keep = g_cur;
    char curname[32];
    kstrlcpy(curname, g_themes[keep].name, sizeof(curname));
    g_n = 0;
    for (int i = 0; i < NBUILTIN; i++) g_themes[g_n++] = BUILTIN[i];
    scan_dir();
    int c = theme_find(curname);
    g_cur = c >= 0 ? c : 0;
    return theme_find(name);
}

/* a .btheme: its .theme and picture go into THEME_DIR */
static int found_theme;
static char found_name[FS_PATH_LEN];
static void zip_seen(const zip_entry_t* e, const char* note, void* arg) {
    (void)note; (void)arg;
    if (!e->is_dir && ends_with(e->name, ".theme") && !found_theme) { found_theme = 1; kstrlcpy(found_name, e->name, sizeof(found_name)); }
}

int theme_install(const char* path, char* err, int cap) {
    if (ends_with(path, ".theme")) return install_file(path, err, cap);
    if (!ends_with(path, ".btheme") && !ends_with(path, ".zip")) { ksnprintf(err, (size_t)cap, "not a .theme or .btheme file"); return -1; }
    found_theme = 0;
    if (zip_list(path, zip_seen, NULL, err, cap) < 0) return -1;
    if (!found_theme) { ksnprintf(err, (size_t)cap, "the archive holds no .theme file"); return -1; }
    fs_mkdir_p(THEME_DIR);
    zip_stats_t st;
    if (zip_extract(path, THEME_DIR, ZIP_OVERWRITE, NULL, NULL, &st, err, cap) < 0) return -1;
    char p[FS_PATH_LEN];
    ksnprintf(p, sizeof(p), "%s/%s", THEME_DIR, found_name);
    return install_file(p, err, cap);
}

int theme_export(int i, const char* name, const char* path) {
    const theme_t* t = theme_get(i);
    if (!t) return -1;
    char buf[1024];
    int n = ksnprintf(buf, sizeof(buf),
        "# A Banana OS theme (sdk/themes/README.md). Change the colours, then\n"
        "# `theme install %s` - or check it on Linux: bananatheme check\n"
        "name=%s%s\nauthor=%s\n%s%s%s",
        path, name ? name : t->name, name ? "" : " (copy)", t->author[0] ? t->author : "you", t->wallpaper[0] ? "wallpaper=" : "", t->wallpaper, t->wallpaper[0] ? "\n" : "");
    for (int k = 0; k < TH_SHADES && n < (int)sizeof(buf) - 40; k++)
        n += ksnprintf(buf + n, sizeof(buf) - (size_t)n, "%s=#%06X\n", SHADE_KEYS[k], t->shade[k]);
    n += ksnprintf(buf + n, sizeof(buf) - (size_t)n, "accent=#%06X\n", t->accent);
    n += ksnprintf(buf + n, sizeof(buf) - (size_t)n, "# Task Manager: banana, classic, windows10 or windows11\ntaskmgr=%s\n", TM_STYLE_KEYS[t->taskmgr]);
    return fs_write_path(path, buf, (uint32_t)n) < 0 ? -1 : 0;
}
