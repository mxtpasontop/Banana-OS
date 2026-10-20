/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CALPANEL_H
#define CALPANEL_H

#include "types.h"

/*
 * The calendar a click on the taskbar's clock opens, like Windows': the
 * time and the date in words, then a month - weeks from Monday, today
 * marked - with arrows to the months before and after. A click on a day
 * picks it; the arrow keys move the day, Page Up / Down the month, Home
 * comes back to today, Esc or a click elsewhere closes it.
 */

/* the panel above the taskbar, its right edge at right_x */
void cal_toggle(int right_x, int bar_y);
void cal_close(void);
int  cal_is_open(void);
int  cal_contains(int mx, int my);
void cal_draw(void);
void cal_click(int mx, int my);
/* a key (KB_*, kbnav.h): 1 */
int  cal_key(int code);

/* the calendar's arithmetic (also the shell's `cal`) */
int  cal_days_in_month(int year, int month);          /* month 1..12 */
int  cal_weekday(int year, int month, int day);       /* 0 Monday .. 6 Sunday */
const char* cal_month_name(int month);                /* "January".. */
const char* cal_day_name(int wd);                     /* "Monday".. (0 Monday) */

#endif
