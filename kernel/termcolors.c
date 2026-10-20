/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "termcolors.h"
#include "settings.h"

/* VGA's order: black, blue, green, cyan, red, magenta, brown, light grey,
 * then the bright ones. 0 is also the window's background, 7 the usual text. */
typedef struct {
    const char* name;
    uint32_t    pal[16];
} scheme_t;

static const scheme_t SCHEMES[] = {
    { "Classic", {                                       /* VGA's own */
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF } },
    { "Campbell", {                                      /* Windows Terminal's */
        0x0C0C0C, 0x0037DA, 0x13A10E, 0x3A96DD, 0xC50F1F, 0x881798, 0xC19C00, 0xCCCCCC,
        0x767676, 0x3B78FF, 0x16C60C, 0x61D6D6, 0xE74856, 0xB4009E, 0xF9F1A5, 0xF2F2F2 } },
    { "Solarized Dark", {
        0x002B36, 0x268BD2, 0x859900, 0x2AA198, 0xDC322F, 0xD33682, 0xB58900, 0x93A1A1,
        0x586E75, 0x839496, 0x9EAD1A, 0x56C4BA, 0xCB4B16, 0x6C71C4, 0xD9AE2E, 0xFDF6E3 } },
    { "Dracula", {
        0x282A36, 0xBD93F9, 0x50FA7B, 0x8BE9FD, 0xFF5555, 0xFF79C6, 0xF1FA8C, 0xF8F8F2,
        0x6272A4, 0xD6ACFF, 0x69FF94, 0xA4FFFF, 0xFF6E6E, 0xFF92DF, 0xFFFFA5, 0xFFFFFF } },
    { "Gruvbox Dark", {
        0x282828, 0x458588, 0x98971A, 0x689D6A, 0xCC241D, 0xB16286, 0xD79921, 0xEBDBB2,
        0x928374, 0x83A598, 0xB8BB26, 0x8EC07C, 0xFB4934, 0xD3869B, 0xFABD2F, 0xFBF1C7 } },
    { "One Half Light", {                                /* dark text on white */
        0xFAFAFA, 0x0184BC, 0x50A14F, 0x0997B3, 0xE45649, 0xA626A4, 0xC18401, 0x383A42,
        0x82848C, 0x2A6FD1, 0x3E8A3D, 0x078094, 0xCA1243, 0x8E1F8C, 0x986801, 0x101114 } },
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

static int      g_scheme = -1;               /* -1: not read from the settings yet */
static uint32_t g_gen;

int termcolors_count(void) { return NSCHEMES; }
const char* termcolors_name(int i) { return i >= 0 && i < NSCHEMES ? SCHEMES[i].name : ""; }

static int same_name(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x == '-' || x == '_') x = ' ';
        if (y == '-' || y == '_') y = ' ';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

int termcolors_find(const char* name) {
    for (int i = 0; i < NSCHEMES; i++) if (same_name(name, SCHEMES[i].name)) return i;
    return -1;
}

int termcolors_current(void) {
    if (g_scheme < 0) {
        char v[32];
        g_scheme = 0;
        if (settings_get("term_colors", v, sizeof(v))) {
            int i = termcolors_find(v);
            if (i >= 0) g_scheme = i;
        }
    }
    return g_scheme;
}

void termcolors_set(int i) {
    if (i < 0 || i >= NSCHEMES) return;
    g_scheme = i;
    g_gen++;
    settings_set("term_colors", SCHEMES[i].name);
}

uint32_t termcolors_scheme_rgb(int scheme, uint8_t c) {
    if (scheme < 0 || scheme >= NSCHEMES) scheme = 0;
    return SCHEMES[scheme].pal[c & 0x0F];
}
uint32_t termcolors_rgb(uint8_t c) { return termcolors_scheme_rgb(termcolors_current(), c); }
uint32_t termcolors_generation(void) { return g_gen; }
