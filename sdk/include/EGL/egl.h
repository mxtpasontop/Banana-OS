/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BANANA_EGL_H
#define BANANA_EGL_H

/*
 * EGL 1.5 for Banana OS apps (USE_EGL = 1, 64-bit): contexts for OpenGL ES
 * 1.1, 2.0 and 3.x (<GLES/gl.h>, <GLES2/gl2.h>, <GLES3/gl3.h>) and desktop
 * OpenGL (<GL/gl3.h>) from Mesa (libOSMesa.so, as USE_GL3), drawing into a
 * Banana OS window or an offscreen pbuffer:
 *
 *     EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
 *     eglInitialize(d, NULL, NULL);
 *     EGLConfig cfg; EGLint n;
 *     eglChooseConfig(d, (EGLint[]){ EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE }, &cfg, 1, &n);
 *     EGLSurface s = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)&win, NULL);
 *     EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE });
 *     eglMakeCurrent(d, s, s, c);
 *     ... draw ... eglSwapBuffers(d, s);
 *
 * The native window is a bwin_t*; a window that changed size is followed at
 * the next eglSwapBuffers. One display, one configuration (RGBA 8888,
 * depth 24, stencil 8).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t      EGLint;
typedef intptr_t     EGLAttrib;
typedef void*        EGLConfig;
typedef void*        EGLContext;
typedef void*        EGLDisplay;
typedef void*        EGLSurface;
typedef void*        EGLClientBuffer;
typedef void*        EGLNativeDisplayType;
typedef void*        EGLNativeWindowType;     /* a bwin_t* */
typedef void*        EGLNativePixmapType;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_FALSE 0
#define EGL_TRUE  1
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DONT_CARE  ((EGLint)-1)
#define EGL_UNKNOWN    ((EGLint)-1)

/* errors */
#define EGL_SUCCESS             0x3000
#define EGL_NOT_INITIALIZED     0x3001
#define EGL_BAD_ACCESS          0x3002
#define EGL_BAD_ALLOC           0x3003
#define EGL_BAD_ATTRIBUTE       0x3004
#define EGL_BAD_CONFIG          0x3005
#define EGL_BAD_CONTEXT         0x3006
#define EGL_BAD_CURRENT_SURFACE 0x3007
#define EGL_BAD_DISPLAY         0x3008
#define EGL_BAD_MATCH           0x3009
#define EGL_BAD_NATIVE_PIXMAP   0x300A
#define EGL_BAD_NATIVE_WINDOW   0x300B
#define EGL_BAD_PARAMETER       0x300C
#define EGL_BAD_SURFACE         0x300D
#define EGL_CONTEXT_LOST        0x300E

/* config attributes */
#define EGL_BUFFER_SIZE             0x3020
#define EGL_ALPHA_SIZE              0x3021
#define EGL_BLUE_SIZE               0x3022
#define EGL_GREEN_SIZE              0x3023
#define EGL_RED_SIZE                0x3024
#define EGL_DEPTH_SIZE              0x3025
#define EGL_STENCIL_SIZE            0x3026
#define EGL_CONFIG_CAVEAT           0x3027
#define EGL_CONFIG_ID               0x3028
#define EGL_LEVEL                   0x3029
#define EGL_MAX_PBUFFER_HEIGHT      0x302A
#define EGL_MAX_PBUFFER_PIXELS      0x302B
#define EGL_MAX_PBUFFER_WIDTH       0x302C
#define EGL_NATIVE_RENDERABLE       0x302D
#define EGL_NATIVE_VISUAL_ID        0x302E
#define EGL_NATIVE_VISUAL_TYPE      0x302F
#define EGL_SAMPLES                 0x3031
#define EGL_SAMPLE_BUFFERS          0x3032
#define EGL_SURFACE_TYPE            0x3033
#define EGL_TRANSPARENT_TYPE        0x3034
#define EGL_TRANSPARENT_BLUE_VALUE  0x3035
#define EGL_TRANSPARENT_GREEN_VALUE 0x3036
#define EGL_TRANSPARENT_RED_VALUE   0x3037
#define EGL_NONE                    0x3038
#define EGL_BIND_TO_TEXTURE_RGB     0x3039
#define EGL_BIND_TO_TEXTURE_RGBA    0x303A
#define EGL_MIN_SWAP_INTERVAL       0x303B
#define EGL_MAX_SWAP_INTERVAL       0x303C
#define EGL_LUMINANCE_SIZE          0x303D
#define EGL_ALPHA_MASK_SIZE         0x303E
#define EGL_COLOR_BUFFER_TYPE       0x303F
#define EGL_RENDERABLE_TYPE         0x3040
#define EGL_MATCH_NATIVE_PIXMAP     0x3041
#define EGL_CONFORMANT              0x3042
#define EGL_SLOW_CONFIG             0x3050
#define EGL_NON_CONFORMANT_CONFIG   0x3051
#define EGL_TRANSPARENT_RGB         0x3052
#define EGL_RGB_BUFFER              0x308E
#define EGL_LUMINANCE_BUFFER        0x308F

/* bits */
#define EGL_PBUFFER_BIT             0x0001
#define EGL_PIXMAP_BIT              0x0002
#define EGL_WINDOW_BIT              0x0004
#define EGL_OPENGL_ES_BIT           0x0001
#define EGL_OPENVG_BIT              0x0002
#define EGL_OPENGL_ES2_BIT          0x0004
#define EGL_OPENGL_BIT              0x0008
#define EGL_OPENGL_ES3_BIT          0x0040

/* strings */
#define EGL_VENDOR                  0x3053
#define EGL_VERSION                 0x3054
#define EGL_EXTENSIONS              0x3055
#define EGL_CLIENT_APIS             0x308D

/* surfaces */
#define EGL_HEIGHT                  0x3056
#define EGL_WIDTH                   0x3057
#define EGL_LARGEST_PBUFFER         0x3058
#define EGL_TEXTURE_FORMAT          0x3080
#define EGL_TEXTURE_TARGET          0x3081
#define EGL_MIPMAP_TEXTURE          0x3082
#define EGL_RENDER_BUFFER           0x3086
#define EGL_SWAP_BEHAVIOR           0x3093
#define EGL_BACK_BUFFER             0x3084
#define EGL_SINGLE_BUFFER           0x3085
#define EGL_BUFFER_PRESERVED        0x3094
#define EGL_BUFFER_DESTROYED        0x3095
#define EGL_GL_COLORSPACE           0x309D
#define EGL_HORIZONTAL_RESOLUTION   0x3090
#define EGL_VERTICAL_RESOLUTION     0x3091
#define EGL_PIXEL_ASPECT_RATIO      0x3092

/* APIs, contexts */
#define EGL_OPENGL_ES_API           0x30A0
#define EGL_OPENVG_API              0x30A1
#define EGL_OPENGL_API              0x30A2
#define EGL_CONTEXT_CLIENT_TYPE     0x3097
#define EGL_CONTEXT_CLIENT_VERSION  0x3098
#define EGL_CONTEXT_MAJOR_VERSION   0x3098
#define EGL_CONTEXT_MINOR_VERSION   0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 0x00000002
#define EGL_CONTEXT_OPENGL_DEBUG    0x31B0
#define EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE 0x31B1
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS 0x31B2
#define EGL_CONTEXT_FLAGS_KHR       0x30FC
#define EGL_DRAW                    0x3059
#define EGL_READ                    0x305A
#define EGL_CORE_NATIVE_ENGINE      0x305B
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

EGLBoolean  eglBindAPI(EGLenum api);
EGLenum     eglQueryAPI(void);
EGLBoolean  eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config);
EGLBoolean  eglGetConfigs(EGLDisplay dpy, EGLConfig* configs, EGLint config_size, EGLint* num_config);
EGLBoolean  eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint* value);
EGLContext  eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint* attrib_list);
EGLSurface  eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attrib_list);
EGLSurface  eglCreatePlatformWindowSurface(EGLDisplay dpy, EGLConfig config, void* native_window, const EGLAttrib* attrib_list);
EGLSurface  eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config, const EGLint* attrib_list);
EGLSurface  eglCreatePixmapSurface(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap, const EGLint* attrib_list);
EGLBoolean  eglDestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean  eglDestroySurface(EGLDisplay dpy, EGLSurface surface);
EGLDisplay  eglGetCurrentDisplay(void);
EGLContext  eglGetCurrentContext(void);
EGLSurface  eglGetCurrentSurface(EGLint readdraw);
EGLDisplay  eglGetDisplay(EGLNativeDisplayType display_id);
EGLDisplay  eglGetPlatformDisplay(EGLenum platform, void* native_display, const EGLAttrib* attrib_list);
EGLint      eglGetError(void);
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* procname);
EGLBoolean  eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
EGLBoolean  eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean  eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint* value);
const char* eglQueryString(EGLDisplay dpy, EGLint name);
EGLBoolean  eglQuerySurface(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint* value);
EGLBoolean  eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value);
EGLBoolean  eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);
EGLBoolean  eglSwapInterval(EGLDisplay dpy, EGLint interval);
EGLBoolean  eglTerminate(EGLDisplay dpy);
EGLBoolean  eglReleaseThread(void);
EGLBoolean  eglWaitClient(void);
EGLBoolean  eglWaitGL(void);
EGLBoolean  eglWaitNative(EGLint engine);

#ifdef __cplusplus
}
#endif

#endif
