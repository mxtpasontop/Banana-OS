/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef THEME_H
#define THEME_H

#include "types.h"

/*
 * Themes: the desktop's colours. A theme gives a colour to each of eight
 * shades - from the darkest (shadow, base: lists and backgrounds) through
 * surface, raised (buttons), line (borders) to text_dim, text and bright -
 * and an accent. The desktop is drawn with its own fixed palette (Banana
 * Dark); each of its colours is wrapped in TC(), which turns it into the
 * theme's: a grey by its lightness, between the two shades around it (so a
 * light theme turns the whole interface over), a blue accent into the
 * theme's accent at the same lightness; other colours (red, green,
 * yellow...) stay. Icons, pictures, the Start orb and terminal text are not
 * themed.
 *
 * A theme is a text file, NAME.theme (see sdk/themes/README.md):
 *     name=Ocean
 *     base=#0B1A20   surface=#10262E   raised=#1C3A44   line=#2F5A66
 *     text_dim=#8FB3BD   text=#E3F4F7   accent=#2EC4B6
 *     wallpaper=Ocean Waves           (a preset's name, or a picture's path)
 * Installed ones live in THEME_DIR; a .btheme is a ZIP of a .theme and its
 * picture. Built-in themes come first.
 */

#define THEME_DIR "/usr/share/themes"
#define THEME_MAX 32

/* the look of Task Manager a theme comes with (taskmgr= in its file) */
enum { TM_STYLE_BANANA = 0, TM_STYLE_CLASSIC, TM_STYLE_WIN10, TM_STYLE_WIN11, TM_STYLES };
extern const char* const TM_STYLE_KEYS[TM_STYLES];   /* "banana", "classic", "windows10", "windows11" */
extern const char* const TM_STYLE_NAMES[TM_STYLES];  /* "Banana OS", "Windows classic", "Windows 10", "Windows 11" */

enum { TH_SHADOW = 0, TH_BASE, TH_SURFACE, TH_RAISED, TH_LINE, TH_DIM, TH_TEXT, TH_BRIGHT, TH_SHADES };

typedef struct {
    char     name[32];
    char     author[32];
    char     wallpaper[96];          /* "" = keep the one chosen */
    uint32_t shade[TH_SHADES];
    uint32_t accent;
    int      light;                  /* light (1) or dark (0): for the picker */
    int      taskmgr;                /* TM_STYLE_*: Task Manager's look */
    char     path[96];               /* its file ("" built in) */
} theme_t;

/* a colour of the desktop's palette, as the current theme has it */
uint32_t theme_color(uint32_t c);
#define TC(c) theme_color((uint32_t)(c))
/* the same for a window designed light (Files, the Start menu): its white
 * is the themes base, its dark text the themes text */
uint32_t theme_color_light(uint32_t c);
#define TCL(c) theme_color_light((uint32_t)(c))

void theme_init(void);                          /* the built-in ones and THEME_DIR; the saved choice */
int  theme_count(void);
const theme_t* theme_get(int i);
int  theme_current(void);
int  theme_find(const char* name);              /* by name (any case), -1 */
/* uses theme i (and its wallpaper), saves the choice: 0 / -1 */
int  theme_apply(int i);
uint32_t theme_generation(void);                /* bumped when the theme changes: redraw */
/* Task Manager's look: the one chosen in Settings > Themes, else the theme's */
int  theme_taskmgr_style(void);
int  theme_taskmgr_override(void);              /* -1: the theme's */
void theme_set_taskmgr_override(int style);     /* -1: the theme's; saved */
int  theme_taskmgr_style_find(const char* key); /* "windows10" (or "10", "win10"...), -1 */

/* reads a .theme file into t: 0, or -1 with the reason in err */
int  theme_parse(const char* text, uint32_t len, theme_t* t, char* err, int cap);
/* installs a .theme or .btheme file into THEME_DIR: its index, or -1 (err) */
int  theme_install(const char* path, char* err, int cap);
/* writes theme i as a .theme file at path, named name (NULL: "<its name> (copy)") - a start for a new one: 0 / -1 */
int  theme_export(int i, const char* name, const char* path);

#endif
