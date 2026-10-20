/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BGL3_H
#define BGL3_H

/*
 * OpenGL 3.3 (core profile, GLSL 3.30) on Banana OS windows: USE_GL3 = 1 in
 * the Makefile (64-bit apps). The renderer is Mesa's (softpipe, on the
 * processor), in the shared library libOSMesa.so, which bgl3 loads the
 * first time: from BGL3_LIBRARY (the environment), the app's folder, /lib,
 * /usr/lib, or a lib folder on a USB stick or another disk (/mnt/usb/lib,
 * /mnt/virtio/lib, /mnt/sata/lib).
 *
 *     bwin_t win;
 *     bwin_open(&win, "Triangle", 640, 480);
 *     bgl3_ctx_t* gl = bgl3_create(&win);     (it is now current; NULL: see bgl3_error())
 *     ... glCreateShader, glGenVertexArrays, glBufferData ...
 *     for (;;) {
 *         glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(...);
 *         bgl3_swap(gl);                      (the frame shows up in the window)
 *         on BANANA_EV_RESIZE: bgl3_resize(gl) and glViewport
 *     }
 *
 * Every OpenGL 1.0 - 3.3 core function is there under its name (a pointer
 * that bgl3 fills); bgl3_proc() gives the others Mesa has (extensions).
 * This is not the OpenGL 1.x of <GL/bgl.h> (USE_GL): an app uses one or
 * the other.
 */

#include "glcorearb.h"
#include "gl3procs.h"
#include <banana.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X(type, name) extern type bgl3_##name;
BGL3_PROCS(X)
#undef X

typedef struct bgl3_ctx bgl3_ctx_t;

bgl3_ctx_t* bgl3_create(bwin_t* win);            /* a GL 3.3 core context drawing into win */
bgl3_ctx_t* bgl3_create_compat(bwin_t* win);     /* ... a compatibility one (fixed pipeline, glBegin...) */
bgl3_ctx_t* bgl3_create_offscreen(int w, int h); /* no window: read the frame with bgl3_pixels() */
/* any version and profile Mesa makes: BGL3_CORE or BGL3_COMPAT, major 0 =
 * the highest; win NULL: offscreen (w x h); share: objects in common, or NULL */
#define BGL3_CORE   0
#define BGL3_COMPAT 1
bgl3_ctx_t* bgl3_create_version(bwin_t* win, int w, int h, int profile, int major, int minor, bgl3_ctx_t* share);
/* draws into px (w x h, top row first) instead of the context's own buffer (EGL surfaces) */
int  bgl3_make_current_buffer(bgl3_ctx_t* ctx, unsigned int* px, int w, int h);
int  bgl3_make_current(bgl3_ctx_t* ctx);         /* 0, or -1 */
bgl3_ctx_t* bgl3_current(void);
void bgl3_resize(bgl3_ctx_t* ctx);               /* the window changed size (after BANANA_EV_RESIZE) */
void bgl3_swap(bgl3_ctx_t* ctx);                 /* finish the frame, show it */
const unsigned int* bgl3_pixels(bgl3_ctx_t* ctx, int* w, int* h);  /* the frame, 0x00RRGGBB, top row first */
void bgl3_destroy(bgl3_ctx_t* ctx);
void* bgl3_proc(const char* name);               /* any GL function Mesa has, or NULL */
const char* bgl3_error(void);                    /* why bgl3_create failed */

#ifdef __cplusplus
}
#endif

#endif
