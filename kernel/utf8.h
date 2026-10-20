/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef UTF8_H
#define UTF8_H

#include "types.h"

/*
 * Typed text is UTF-8 (the keyboard sends é as two bytes). Line editors
 * keep bytes but move, erase and draw whole characters: one character is
 * one column.
 */

static inline int u8_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* columns (characters) in the first n bytes */
static inline int u8_cols(const char* s, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (!u8_cont(s[i])) c++;
    return c;
}

/* the byte where column col starts (len when past the end) */
static inline int u8_byte_at(const char* s, int len, int col) {
    int i = 0;
    while (i < len && col > 0) {
        i++;
        while (i < len && u8_cont(s[i])) i++;
        col--;
    }
    return i;
}

/* ── display width, like wcwidth(): East Asian Wide and Fullwidth
 * characters (CJK, Hangul, kana, fullwidth forms) and emoji take two
 * columns of a text grid, combining marks and joiners none, the rest one */
typedef struct { uint32_t lo, hi; } u8_range_t;

static const u8_range_t U8_WIDE[] = {
    { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 },
    { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F },
    { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
    { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 },
    { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 },
    { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 },
    { 0x2E80, 0x303E }, { 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF },
    { 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F },
    { 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FE4 }, { 0x17000, 0x18CFF }, { 0x1AFF0, 0x1B2FF },
    { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A },
    { 0x1F200, 0x1F202 }, { 0x1F210, 0x1F23B }, { 0x1F240, 0x1F248 }, { 0x1F250, 0x1F251 },
    { 0x1F260, 0x1F265 }, { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C },
    { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 },
    { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC },
    { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A },
    { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 },
    { 0x1F6CC, 0x1F6CC }, { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF },
    { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 },
    { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 }, { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FAFF },
    { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};
static const u8_range_t U8_ZERO[] = {
    { 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x200B, 0x200F }, { 0x2060, 0x2064 }, { 0x20D0, 0x20FF },
    { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF }, { 0xE0000, 0xE0FFF },
};

static inline int u8_in(const u8_range_t* r, int n, uint32_t cp) {
    if (cp < r[0].lo || cp > r[n - 1].hi) return 0;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cp > r[m].hi) lo = m + 1;
        else if (cp < r[m].lo) hi = m - 1;
        else return 1;
    }
    return 0;
}

/* columns a character takes: 0, 1 or 2 */
static inline int u8_wcwidth(uint32_t cp) {
    if (cp < 0x300) return 1;
    if (u8_in(U8_WIDE, (int)(sizeof(U8_WIDE) / sizeof(U8_WIDE[0])), cp)) return 2;
    if (u8_in(U8_ZERO, (int)(sizeof(U8_ZERO) / sizeof(U8_ZERO[0])), cp)) return 0;
    return 1;
}

static inline uint32_t u8_decode(const char* s, int len, int* i);

/* display columns of the first n bytes (a wide character counts two) */
static inline int u8_width(const char* s, int n) {
    int c = 0;
    for (int i = 0; i < n; ) {
        if ((unsigned char)s[i] < 0x80) { c++; i++; continue; }
        c += u8_wcwidth(u8_decode(s, n, &i));
    }
    return c;
}

/* the byte where display column col starts (len when past the end; a
 * column inside a wide character gives that character's start) */
static inline int u8_byte_at_col(const char* s, int len, int col) {
    int i = 0, c = 0;
    while (i < len) {
        int j = i;
        int w = (unsigned char)s[i] < 0x80 ? (j++, 1) : u8_wcwidth(u8_decode(s, len, &j));
        if (c + w > col) break;
        c += w;
        i = j;
    }
    return i;
}

/* the start of the character before / after byte i */
static inline int u8_prev(const char* s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && u8_cont(s[i])) i--;
    return i;
}
static inline int u8_next(const char* s, int len, int i) {
    if (i >= len) return len;
    i++;
    while (i < len && u8_cont(s[i])) i++;
    return i;
}

/* the character at byte *i (advancing it); 0xFFFD for a broken one */
static inline uint32_t u8_decode(const char* s, int len, int* i) {
    unsigned char b = (unsigned char)s[*i];
    (*i)++;
    if (b < 0x80) return b;
    int need = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (!need) return 0xFFFD;
    uint32_t cp = b & (0x3F >> need);
    for (int k = 0; k < need; k++) {
        if (*i >= len || !u8_cont(s[*i])) return 0xFFFD;
        cp = cp << 6 | ((unsigned char)s[(*i)++] & 0x3F);
    }
    return cp;
}

/* a character as UTF-8 (out: 4 bytes); the byte count */
static inline int u8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* UTF-8 (len bytes) -> UTF-16 units, characters past U+FFFF as surrogate
 * pairs: how many units (at most cap), -1 if they do not fit */
static inline int u8_to_utf16(const char* s, int len, uint16_t* out, int cap) {
    int n = 0;
    for (int i = 0; i < len; ) {
        uint32_t cp = u8_decode(s, len, &i);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            if (n + 2 > cap) return -1;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (uint16_t)(cp > 0xFFFF ? 0xFFFD : cp);
        }
    }
    return n;
}

/* UTF-16 units -> UTF-8 in out (cap bytes, NUL-terminated, cut short at a
 * whole character): the byte count. A lone surrogate becomes U+FFFD */
static inline int u8_from_utf16(const uint16_t* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        char u[4];
        int k = u8_encode(cp, u);
        if (o + k + 1 > cap) break;
        for (int j = 0; j < k; j++) out[o++] = u[j];
    }
    if (cap > 0) out[o] = 0;
    return o;
}

/* cuts s (NUL-terminated) to fewer than cap bytes, at a whole character */
static inline void u8_truncate(char* s, int cap) {
    int n = 0;
    while (s[n]) n++;
    if (n < cap) return;
    n = cap - 1;
    while (n > 0 && u8_cont(s[n])) n--;
    s[n] = 0;
}

/* drops the last character of a NUL-terminated string of n bytes; the new length */
static inline int u8_backspace(char* s, int n) {
    n = u8_prev(s, n);
    s[n] = 0;
    return n;
}

#endif
