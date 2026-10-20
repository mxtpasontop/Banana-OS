/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKCMD_H
#define DISKCMD_H

/* diskmgr, diskpart, diskmgmt.msc, mkfs.fat / mkfs.vfat / mkfs.exfat (shell/diskcmd.c): 1 if it was one */
int diskcmd_dispatch(int argc, char** argv);

#endif
