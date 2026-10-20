/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DEVMGR_H
#define DEVMGR_H

#include "types.h"
#include "winframe.h"
#include "fb.h"

/*
 * "Device Manager", like Windows: the computer's devices by type or by
 * connection, each one's driver, status and resources (kernel/devices.c).
 * Open it from the Start menu (All Programs), the desktop's right-click
 * menu, or `devmgr gui` (devmgmt.msc) in a terminal; `devmgr` lists the
 * same in the terminal.
 */

void devmgr_open(void);
void devmgr_close(void);
int  devmgr_is_open(void);
void devmgr_draw(const fb_info_t* fi);
int  devmgr_contains(int mx, int my);
void devmgr_click(int mx, int my);
void devmgr_mouse(int mx, int my, int left);
void devmgr_rclick(int mx, int my);
void devmgr_wheel(int mx, int my, int dz);
uint32_t devmgr_signature(void);
int  devmgr_navkey(int code);       /* kbnav.h keys */
win_geom_t* devmgr_geom(void);

#endif
