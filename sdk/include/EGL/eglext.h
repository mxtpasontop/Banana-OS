/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BANANA_EGLEXT_H
#define BANANA_EGLEXT_H
#include <EGL/egl.h>
#define EGL_KHR_create_context 1
#define EGL_KHR_surfaceless_context 1
#define EGL_MESA_platform_surfaceless 1
#define EGL_EXT_platform_base 1
#define EGL_CONTEXT_FLAGS_KHR 0x30FC
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR 0x00000001
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR 0x00000002
typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC)(EGLenum platform, void* native_display, const EGLint* attrib_list);
#endif
