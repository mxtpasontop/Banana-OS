/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKMGR_H
#define DISKMGR_H

#include "types.h"
#include "winframe.h"
#include "fb.h"

/*
 * "Disk Management", like Windows': the volumes and the disks' partitions,
 * with New volume, Format, Delete, Mount / Unmount and a new partition
 * table (kernel/diskpart.c). Open it from the Start menu (All Programs),
 * the desktop's right-click menu, `diskmgr gui` or diskmgmt.msc.
 */

void diskmgr_open(void);
void diskmgr_close(void);
int  diskmgr_is_open(void);
void diskmgr_draw(const fb_info_t* fi);
int  diskmgr_contains(int mx, int my);
void diskmgr_click(int mx, int my);
void diskmgr_mouse(int mx, int my, int left);
void diskmgr_rclick(int mx, int my);
void diskmgr_wheel(int mx, int my, int dz);
void diskmgr_key(char c);            /* typed characters (its dialog) */
uint32_t diskmgr_signature(void);
int  diskmgr_navkey(int code);       /* kbnav.h keys */
win_geom_t* diskmgr_geom(void);

#endif
