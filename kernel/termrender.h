/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TERMRENDER_H
#define TERMRENDER_H

#include "types.h"

/*
 * How terminal windows are drawn (kernel/termrender.c), as in other
 * systems' terminals: "GPU rendering" on or off, and with it on, which
 * renderer:
 *   cells  only the cells that changed are drawn again, then only the
 *          parts of the screen that changed are sent to it (otherwise
 *          every change draws the whole desktop again);
 *   virgl  the same cells, put together by the graphics card through
 *          VirGL (virtio-gpu 3D, in a VM): each glyph drawn once into a
 *          texture on the card, the window's text copied from it there
 *          and read back. Without VirGL it works as "cells".
 * Off: the whole desktop is drawn for every change, as before.
 * Kept in /etc/settings.conf as term_gpu (on / off) and term_renderer.
 */

enum { TR_CELLS, TR_VIRGL };
int  termrender_gpu(void);
void termrender_set_gpu(int on);
int  termrender_method(void);
void termrender_set_method(int m);
const char* termrender_method_name(int m);
uint32_t termrender_generation(void);

/* what was drawn how: whole desktops, frames of changed cells (and how
 * many cells), frames put together by VirGL */
typedef struct { uint32_t full, cells, cells_drawn, virgl; } termrender_stats_t;
termrender_stats_t* termrender_stats(void);

/* ── the VirGL renderer ── */
int  trv_available(void);           /* VirGL is there and has not failed */
const char* trv_status(void);       /* why not, or what it uses */
/* a frame: cells cw x ch on a screen w x h; 0 if it cannot (use software) */
int  trv_begin(int cw, int ch, int screen_w, int screen_h);
/* the tile for key is on the card already */
int  trv_has(uint64_t key);
/* ... put there from cw x ch pixels (stride: in pixels) */
int  trv_upload(uint64_t key, const uint32_t* px, int stride);
/* the tile for key copied to (x, y) of the frame */
int  trv_cell(uint64_t key, int x, int y);
/* the frame's copies done on the card, and the rectangle read back into
 * dst (the screen's image, stride in pixels): 0 if it failed */
int  trv_finish(int x, int y, int w, int h, uint32_t* dst, int stride);
void trv_tests(void (*report)(const char*, int, const char*));

#endif
