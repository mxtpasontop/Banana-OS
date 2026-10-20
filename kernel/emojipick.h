/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EMOJIPICK_H
#define EMOJIPICK_H

#include "types.h"
#include "fb.h"

/*
 * Win+. (or Win+;): the emoji panel, like Windows'. A panel above the
 * taskbar with the emoji in groups - the ones used lately first - and a
 * search: typing filters them by name. A click, Enter or Space (with no
 * search typed) types the emoji into the window in front, as if it came
 * from the keyboard; Tab / Shift+Tab change the group, Esc clears the
 * search and then closes. The emoji used lately are kept in
 * /etc/emoji-recent.
 */

int  ep_is_open(void);
void ep_toggle(void);
void ep_close(void);
/* bar_h: the taskbar's height (the panel sits above it) */
void ep_draw(const fb_info_t* fi, int bar_h);
/* a key: KB_* (kbnav.h), '\b' or a typed character; 1 (always ours) */
int  ep_key(int code);
/* a click: on an emoji types it, on a group shows it; outside, the panel closes */
void ep_click(const fb_info_t* fi, int bar_h, int mx, int my);
/* the wheel over the panel scrolls it: 1 if it was over it */
int  ep_wheel(const fb_info_t* fi, int bar_h, int mx, int my, int dz);
/* (selftest) how many emoji match a search, and the first one's UTF-8 */
int  ep_search_count(const char* query, const char** first);

#endif
