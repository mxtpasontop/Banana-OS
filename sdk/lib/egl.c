/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * EGL 1.5 for Banana OS apps (USE_EGL = 1): OpenGL ES 1.1 / 2.0 / 3.x and
 * desktop OpenGL contexts from Mesa (libOSMesa.so, through sdk/lib/gl3.c),
 * drawing into a window (a bwin_t*) or a pbuffer.
 *
 * OpenGL ES runs on Mesa's compatibility profile, which takes everything
 * OpenGL ES does (client-side vertex arrays, no vertex array object needed,
 * GLSL ES shaders: "#version 100" / "300 es" - a shader without a #version
 * line is given "#version 100", as OpenGL ES reads it). glGetString says
 * "OpenGL ES ..." in an OpenGL ES context.
 */
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(type, name) type bgles_##name;
BGLES_PROCS(X)
#undef X
#define X(ret, name, args) ret (*bgles1_##name) args;
BGLES1_PROCS(X)
#undef X

#define DPY ((EGLDisplay)0xB001)
#define CFG ((EGLConfig)0xC001)

typedef struct {
    int magic;                     /* 'S' */
    bwin_t* win;                   /* NULL: a pbuffer */
    int w, h;
    unsigned int* px;
} surface_t;

typedef struct {
    int magic;                     /* 'C' */
    bgl3_ctx_t* gl;
    EGLenum api;                   /* EGL_OPENGL_ES_API or EGL_OPENGL_API */
    int major, minor;              /* (OpenGL ES: what was asked) */
    int bound;                     /* made current on a surface once: its viewport set */
} context_t;

static EGLint g_error = EGL_SUCCESS;
static EGLenum g_api = EGL_OPENGL_ES_API;
static int g_init;
static context_t* g_ctx;
static surface_t* g_draw;
static surface_t* g_read;

static EGLBoolean fail(EGLint e) { g_error = e; return EGL_FALSE; }
static int good_dpy(EGLDisplay d) { if (d != DPY) { g_error = EGL_BAD_DISPLAY; return 0; } if (!g_init) { g_error = EGL_NOT_INITIALIZED; return 0; } return 1; }

EGLint eglGetError(void) { EGLint e = g_error; g_error = EGL_SUCCESS; return e; }

EGLDisplay eglGetDisplay(EGLNativeDisplayType id) { (void)id; return DPY; }
EGLDisplay eglGetPlatformDisplay(EGLenum platform, void* native, const EGLAttrib* attrs) {
    (void)native; (void)attrs;
    if (platform != EGL_PLATFORM_SURFACELESS_MESA && platform != 0) { g_error = EGL_BAD_PARAMETER; return EGL_NO_DISPLAY; }
    return DPY;
}

EGLBoolean eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
    if (d != DPY) return fail(EGL_BAD_DISPLAY);
    /* (Mesa is loaded when the first context is made: a probe here) */
    bgl3_ctx_t* probe = bgl3_create_version(NULL, 1, 1, BGL3_COMPAT, 0, 0, NULL);
    if (!probe) { fprintf(stderr, "EGL: %s\n", bgl3_error()); return fail(EGL_NOT_INITIALIZED); }
    bgl3_destroy(probe);
    g_init = 1;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay d) {
    if (d != DPY) return fail(EGL_BAD_DISPLAY);
    g_init = 0;
    return EGL_TRUE;
}

const char* eglQueryString(EGLDisplay d, EGLint name) {
    if (name == EGL_EXTENSIONS && d == EGL_NO_DISPLAY)
        return "EGL_EXT_client_extensions EGL_EXT_platform_base EGL_MESA_platform_surfaceless";
    if (!good_dpy(d)) return NULL;
    switch (name) {
    case EGL_VENDOR: return "Mesa Project (Banana OS)";
    case EGL_VERSION: return "1.5 Banana OS";
    case EGL_CLIENT_APIS: return "OpenGL OpenGL_ES";
    case EGL_EXTENSIONS: return "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_KHR_no_config_context "
                                "EGL_MESA_configless_context EGL_KHR_get_all_proc_addresses";
    }
    g_error = EGL_BAD_PARAMETER;
    return NULL;
}

/* ── the one configuration: RGBA 8888, depth 24, stencil 8 ── */
static int cfg_attr(EGLint a, EGLint* v) {
    switch (a) {
    case EGL_BUFFER_SIZE: *v = 32; return 1;
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: *v = 8; return 1;
    case EGL_DEPTH_SIZE: *v = 24; return 1;
    case EGL_STENCIL_SIZE: *v = 8; return 1;
    case EGL_CONFIG_ID: *v = 1; return 1;
    case EGL_CONFIG_CAVEAT: *v = EGL_NONE; return 1;
    case EGL_SURFACE_TYPE: *v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; return 1;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: *v = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT | EGL_OPENGL_BIT; return 1;
    case EGL_COLOR_BUFFER_TYPE: *v = EGL_RGB_BUFFER; return 1;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: case EGL_LEVEL: case EGL_LUMINANCE_SIZE: case EGL_ALPHA_MASK_SIZE:
    case EGL_NATIVE_RENDERABLE: case EGL_NATIVE_VISUAL_ID: case EGL_NATIVE_VISUAL_TYPE: *v = 0; return 1;
    case EGL_TRANSPARENT_TYPE: *v = EGL_NONE; return 1;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: *v = 8192; return 1;
    case EGL_MAX_PBUFFER_PIXELS: *v = 8192 * 8192; return 1;
    case EGL_MIN_SWAP_INTERVAL: *v = 0; return 1;
    case EGL_MAX_SWAP_INTERVAL: *v = 1; return 1;
    case EGL_BIND_TO_TEXTURE_RGB: case EGL_BIND_TO_TEXTURE_RGBA: *v = EGL_FALSE; return 1;
    }
    return 0;
}

EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig* configs, EGLint size, EGLint* num) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!num) return fail(EGL_BAD_PARAMETER);
    *num = 1;
    if (configs && size > 0) configs[0] = CFG;
    else if (configs) *num = 0;
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint* attrs, EGLConfig* configs, EGLint size, EGLint* num) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!num) return fail(EGL_BAD_PARAMETER);
    int ok = 1;
    for (const EGLint* a = attrs; a && a[0] != EGL_NONE; a += 2) {
        EGLint want = a[1], have;
        if (want == EGL_DONT_CARE) continue;
        if (!cfg_attr(a[0], &have)) continue;
        switch (a[0]) {
        case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: case EGL_DEPTH_SIZE:
        case EGL_STENCIL_SIZE: case EGL_BUFFER_SIZE:
            if (want > have) ok = 0;
            break;
        case EGL_SURFACE_TYPE: case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT:
            if ((want & have) != want) ok = 0;
            break;
        case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS:
            if (want > 0) ok = 0;
            break;
        case EGL_CONFIG_ID:
            if (want != have) ok = 0;
            break;
        }
    }
    *num = ok ? 1 : 0;
    if (ok && configs && size > 0) configs[0] = CFG;
    else if (configs) *num = ok && size > 0;
    return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attribute, EGLint* value) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (c != CFG) return fail(EGL_BAD_CONFIG);
    if (!value || !cfg_attr(attribute, value)) return fail(EGL_BAD_ATTRIBUTE);
    return EGL_TRUE;
}

/* ── surfaces ── */
static surface_t* new_surface(bwin_t* win, int w, int h) {
    surface_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->magic = 'S';
    s->win = win;
    s->w = w < 1 ? 1 : w;
    s->h = h < 1 ? 1 : h;
    s->px = calloc((size_t)s->w * s->h, 4);
    if (!s->px) { free(s); return NULL; }
    return s;
}

static int is_surface(EGLSurface s) { return s && ((surface_t*)s)->magic == 'S'; }
static int is_context(EGLContext c) { return c && ((context_t*)c)->magic == 'C'; }

EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType win, const EGLint* attrs) {
    (void)attrs;
    if (!good_dpy(d)) return EGL_NO_SURFACE;
    if (c != CFG) { g_error = EGL_BAD_CONFIG; return EGL_NO_SURFACE; }
    bwin_t* w = (bwin_t*)win;
    if (!w || w->id < 0) { g_error = EGL_BAD_NATIVE_WINDOW; return EGL_NO_SURFACE; }
    surface_t* s = new_surface(w, w->w, w->h);
    if (!s) g_error = EGL_BAD_ALLOC;
    return s;
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void* win, const EGLAttrib* attrs) {
    (void)attrs;
    return eglCreateWindowSurface(d, c, (EGLNativeWindowType)win, NULL);
}

EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* attrs) {
    if (!good_dpy(d)) return EGL_NO_SURFACE;
    if (c != CFG) { g_error = EGL_BAD_CONFIG; return EGL_NO_SURFACE; }
    int w = 0, h = 0;
    for (const EGLint* a = attrs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH) w = a[1];
        else if (a[0] == EGL_HEIGHT) h = a[1];
    }
    surface_t* s = new_surface(NULL, w, h);
    if (!s) g_error = EGL_BAD_ALLOC;
    return s;
}

EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint* attrs) {
    (void)d; (void)c; (void)p; (void)attrs;
    g_error = EGL_BAD_NATIVE_PIXMAP;
    return EGL_NO_SURFACE;
}

EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface surface) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_surface(surface)) return fail(EGL_BAD_SURFACE);
    surface_t* s = surface;
    if (s == g_draw || s == g_read) return EGL_TRUE;          /* (freed when no longer current: kept) */
    s->magic = 0;
    free(s->px);
    free(s);
    return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface surface, EGLint attribute, EGLint* value) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_surface(surface)) return fail(EGL_BAD_SURFACE);
    if (!value) return fail(EGL_BAD_PARAMETER);
    surface_t* s = surface;
    switch (attribute) {
    case EGL_WIDTH: *value = s->win ? s->win->w : s->w; return EGL_TRUE;
    case EGL_HEIGHT: *value = s->win ? s->win->h : s->h; return EGL_TRUE;
    case EGL_CONFIG_ID: *value = 1; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *value = s->win ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER; return EGL_TRUE;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_PRESERVED; return EGL_TRUE;
    case EGL_LARGEST_PBUFFER: case EGL_MIPMAP_TEXTURE: *value = 0; return EGL_TRUE;
    case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: case EGL_PIXEL_ASPECT_RATIO: *value = EGL_UNKNOWN; return EGL_TRUE;
    }
    return fail(EGL_BAD_ATTRIBUTE);
}

EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface surface, EGLint attribute, EGLint value) {
    (void)attribute; (void)value;
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_surface(surface)) return fail(EGL_BAD_SURFACE);
    return EGL_TRUE;
}

/* ── contexts ── */
EGLBoolean eglBindAPI(EGLenum api) {
    if (api != EGL_OPENGL_ES_API && api != EGL_OPENGL_API) return fail(EGL_BAD_PARAMETER);
    g_api = api;
    return EGL_TRUE;
}
EGLenum eglQueryAPI(void) { return g_api; }

EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint* attrs) {
    if (!good_dpy(d)) return EGL_NO_CONTEXT;
    if (c != CFG && c != (EGLConfig)0) { g_error = EGL_BAD_CONFIG; return EGL_NO_CONTEXT; }
    int major = g_api == EGL_OPENGL_ES_API ? 1 : 0, minor = 0, profile_mask = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
    for (const EGLint* a = attrs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_CONTEXT_MAJOR_VERSION) major = a[1];
        else if (a[0] == EGL_CONTEXT_MINOR_VERSION) minor = a[1];
        else if (a[0] == EGL_CONTEXT_OPENGL_PROFILE_MASK) profile_mask = a[1];
    }
    context_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { g_error = EGL_BAD_ALLOC; return EGL_NO_CONTEXT; }
    ctx->magic = 'C';
    ctx->api = g_api;
    ctx->major = major;
    ctx->minor = minor;
    bgl3_ctx_t* sh = is_context(share) ? ((context_t*)share)->gl : NULL;
    if (g_api == EGL_OPENGL_ES_API) {
        if (major < 1 || major > 3) { free(ctx); g_error = EGL_BAD_MATCH; return EGL_NO_CONTEXT; }
        ctx->gl = bgl3_create_version(NULL, 1, 1, BGL3_COMPAT, 0, 0, sh);
    } else {
        int compat = (profile_mask & EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT) != 0 || (major && major < 3) || (major == 3 && minor < 2);
        ctx->gl = bgl3_create_version(NULL, 1, 1, compat ? BGL3_COMPAT : BGL3_CORE, compat ? 0 : major, compat ? 0 : minor, sh);
    }
    if (!ctx->gl) { fprintf(stderr, "EGL: %s\n", bgl3_error()); free(ctx); g_error = EGL_BAD_MATCH; return EGL_NO_CONTEXT; }
    return ctx;
}

EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext context) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_context(context)) return fail(EGL_BAD_CONTEXT);
    context_t* c = context;
    if (c == g_ctx) { bgl3_make_current(NULL); g_ctx = NULL; g_draw = g_read = NULL; }
    c->magic = 0;
    bgl3_destroy(c->gl);
    free(c);
    return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay d, EGLContext context, EGLint attribute, EGLint* value) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_context(context)) return fail(EGL_BAD_CONTEXT);
    context_t* c = context;
    if (!value) return fail(EGL_BAD_PARAMETER);
    switch (attribute) {
    case EGL_CONFIG_ID: *value = 1; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_TYPE: *value = (EGLint)c->api; return EGL_TRUE;
    case EGL_CONTEXT_CLIENT_VERSION: *value = c->major; return EGL_TRUE;
    case EGL_RENDER_BUFFER: *value = g_draw && g_draw->win ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER; return EGL_TRUE;
    }
    return fail(EGL_BAD_ATTRIBUTE);
}

/* ── OpenGL ES's own behaviour on top of Mesa's desktop context ── */
static PFNGLGETSTRINGPROC g_real_getstring;
static PFNGLSHADERSOURCEPROC g_real_shadersource;
static char g_es_version[96], g_es_glsl[64];

static const GLubyte* es_get_string(GLenum name) {
    if (g_ctx && g_ctx->api == EGL_OPENGL_ES_API) {
        if (name == GL_VERSION) return (const GLubyte*)g_es_version;
        if (name == GL_SHADING_LANGUAGE_VERSION) return (const GLubyte*)g_es_glsl;
    }
    return g_real_getstring(name);
}

/* a shader without "#version": OpenGL ES's 1.00 */
static void es_shader_source(GLuint shader, GLsizei count, const GLchar* const* str, const GLint* len) {
    int has = 0;
    for (GLsizei i = 0; i < count && !has; i++) {
        const char* s = str[i];
        int n = len && len[i] >= 0 ? len[i] : (int)strlen(s);
        for (int k = 0; k + 8 <= n; k++) if (!memcmp(s + k, "#version", 8)) { has = 1; break; }
    }
    if (has || !g_ctx || g_ctx->api != EGL_OPENGL_ES_API) { g_real_shadersource(shader, count, str, len); return; }
    const GLchar** s2 = malloc(sizeof(GLchar*) * (size_t)(count + 1));
    GLint* l2 = malloc(sizeof(GLint) * (size_t)(count + 1));
    if (!s2 || !l2) { free(s2); free(l2); g_real_shadersource(shader, count, str, len); return; }
    s2[0] = "#version 100\n";
    l2[0] = 13;
    for (GLsizei i = 0; i < count; i++) { s2[i + 1] = str[i]; l2[i + 1] = len ? len[i] : -1; }
    g_real_shadersource(shader, count + 1, s2, l2);
    free(s2);
    free(l2);
}

static void APIENTRY sh_clear_depthf(GLfloat d) { bgl3_glClearDepth(d); }
static void APIENTRY sh_depth_rangef(GLfloat n, GLfloat f) { bgl3_glDepthRange(n, f); }
static void (*os_ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void (*os_frustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void (*os_clip_plane)(GLenum, const GLdouble*);
static void sh_orthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { if (os_ortho) os_ortho(l, r, b, t, n, f); }
static void sh_frustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { if (os_frustum) os_frustum(l, r, b, t, n, f); }
static void sh_clip_planef(GLenum p, const GLfloat* e) { GLdouble d[4] = { e[0], e[1], e[2], e[3] }; if (os_clip_plane) os_clip_plane(p, d); }

static void load_es(void) {
#define X(type, name) bgles_##name = (type)bgl3_proc(#name);
    BGLES_PROCS(X)
#undef X
#define X(ret, name, args) *(void**)&bgles1_##name = bgl3_proc(#name);
    BGLES1_PROCS(X)
#undef X
    if (!bgles_glClearDepthf) bgles_glClearDepthf = sh_clear_depthf;
    if (!bgles_glDepthRangef) bgles_glDepthRangef = sh_depth_rangef;
    *(void**)&os_ortho = bgl3_proc("glOrtho");
    *(void**)&os_frustum = bgl3_proc("glFrustum");
    *(void**)&os_clip_plane = bgl3_proc("glClipPlane");
    bgles1_glOrthof = sh_orthof;
    bgles1_glFrustumf = sh_frustumf;
    bgles1_glClipPlanef = sh_clip_planef;
    /* the wrappers (only OpenGL ES contexts see a difference) */
    if (bgl3_glGetString != es_get_string) { g_real_getstring = bgl3_glGetString; bgl3_glGetString = es_get_string; }
    if (bgl3_glShaderSource != es_shader_source) { g_real_shadersource = bgl3_glShaderSource; bgl3_glShaderSource = es_shader_source; }
}

EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext context) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (context == EGL_NO_CONTEXT) {
        bgl3_make_current(NULL);
        g_ctx = NULL;
        g_draw = g_read = NULL;
        return EGL_TRUE;
    }
    if (!is_context(context)) return fail(EGL_BAD_CONTEXT);
    if (draw != EGL_NO_SURFACE && !is_surface(draw)) return fail(EGL_BAD_SURFACE);
    if (read != EGL_NO_SURFACE && !is_surface(read)) return fail(EGL_BAD_SURFACE);
    context_t* c = context;
    surface_t* s = draw;
    int rc = s ? bgl3_make_current_buffer(c->gl, s->px, s->w, s->h) : bgl3_make_current(c->gl);
    if (rc) return fail(EGL_BAD_MATCH);
    g_ctx = c;
    g_draw = s;
    g_read = read;
    load_es();
    if (s && !c->bound) {                      /* (EGL: the first surface sets the viewport and scissor box) */
        bgl3_glViewport(0, 0, s->w, s->h);
        bgl3_glScissor(0, 0, s->w, s->h);
        c->bound = 1;
    }
    if (c->api == EGL_OPENGL_ES_API) {
        /* what OpenGL ES says it is: the version asked (a 3.x context: 3.x where Mesa has it) */
        const char* real = (const char*)g_real_getstring(GL_VERSION);
        int es = c->major >= 3 ? 3 : c->major == 2 ? 2 : 1, minor = c->major >= 3 ? c->minor : c->major == 1 ? 1 : 0;
        if (es == 1) snprintf(g_es_version, sizeof(g_es_version), "OpenGL ES-CM 1.1 (%s)", real ? real : "Mesa");
        else snprintf(g_es_version, sizeof(g_es_version), "OpenGL ES %d.%d (%s)", es, minor, real ? real : "Mesa");
        snprintf(g_es_glsl, sizeof(g_es_glsl), "OpenGL ES GLSL ES %s", es >= 3 ? "3.00" : "1.00");
    }
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext(void) { return g_ctx ? (EGLContext)g_ctx : EGL_NO_CONTEXT; }
EGLDisplay eglGetCurrentDisplay(void) { return g_ctx ? DPY : EGL_NO_DISPLAY; }
EGLSurface eglGetCurrentSurface(EGLint which) { return which == EGL_READ ? (EGLSurface)g_read : (EGLSurface)g_draw; }

/* the frame into the window; a window that changed size gets a new buffer */
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface surface) {
    if (!good_dpy(d)) return EGL_FALSE;
    if (!is_surface(surface)) return fail(EGL_BAD_SURFACE);
    surface_t* s = surface;
    if (!s->win) return EGL_TRUE;
    bgl3_glFinish();
    int w = s->w < s->win->w ? s->w : s->win->w, h = s->h < s->win->h ? s->h : s->win->h;
    if (s->win->px) {
        for (int y = 0; y < h; y++) {
            const unsigned int* src = s->px + (long)y * s->w;
            unsigned int* dst = s->win->px + (long)y * s->win->w;
            for (int x = 0; x < w; x++) dst[x] = src[x] & 0x00FFFFFFu;
        }
        bwin_update(s->win);
    }
    if (s->win->w != s->w || s->win->h != s->h) {
        unsigned int* np = calloc((size_t)(s->win->w > 0 ? s->win->w : 1) * (s->win->h > 0 ? s->win->h : 1), 4);
        if (np) {
            free(s->px);
            s->px = np;
            s->w = s->win->w > 0 ? s->win->w : 1;
            s->h = s->win->h > 0 ? s->win->h : 1;
            if (g_ctx && g_draw == s) bgl3_make_current_buffer(g_ctx->gl, s->px, s->w, s->h);
        }
    }
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay d, EGLint interval) { (void)interval; return good_dpy(d) ? EGL_TRUE : EGL_FALSE; }
EGLBoolean eglReleaseThread(void) { if (g_init) eglMakeCurrent(DPY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); return EGL_TRUE; }
EGLBoolean eglWaitClient(void) { if (g_ctx) bgl3_glFinish(); return EGL_TRUE; }
EGLBoolean eglWaitGL(void) { return eglWaitClient(); }
EGLBoolean eglWaitNative(EGLint engine) { (void)engine; return EGL_TRUE; }

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name) {
    if (!name) return NULL;
    if (!strncmp(name, "egl", 3)) {
        static const struct { const char* n; void* f; } E[] = {
            { "eglGetPlatformDisplay", (void*)eglGetPlatformDisplay }, { "eglGetPlatformDisplayEXT", (void*)eglGetPlatformDisplay },
            { "eglCreatePlatformWindowSurface", (void*)eglCreatePlatformWindowSurface }, { "eglSwapInterval", (void*)eglSwapInterval },
            { "eglQueryContext", (void*)eglQueryContext }, { "eglGetCurrentContext", (void*)eglGetCurrentContext },
        };
        for (unsigned i = 0; i < sizeof(E) / sizeof(E[0]); i++) if (!strcmp(name, E[i].n)) return (__eglMustCastToProperFunctionPointerType)E[i].f;
        return NULL;
    }
    /* (the wrapped ones, then Mesa's) */
    if (!strcmp(name, "glGetString")) return (__eglMustCastToProperFunctionPointerType)es_get_string;
    if (!strcmp(name, "glShaderSource")) return (__eglMustCastToProperFunctionPointerType)es_shader_source;
    if (!strcmp(name, "glClearDepthf") && bgles_glClearDepthf) return (__eglMustCastToProperFunctionPointerType)bgles_glClearDepthf;
    if (!strcmp(name, "glDepthRangef") && bgles_glDepthRangef) return (__eglMustCastToProperFunctionPointerType)bgles_glDepthRangef;
    if (!strcmp(name, "glOrthof")) return (__eglMustCastToProperFunctionPointerType)sh_orthof;
    if (!strcmp(name, "glFrustumf")) return (__eglMustCastToProperFunctionPointerType)sh_frustumf;
    return (__eglMustCastToProperFunctionPointerType)bgl3_proc(name);
}
