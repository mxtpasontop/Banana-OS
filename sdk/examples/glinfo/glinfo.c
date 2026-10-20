/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * glinfo: what Mesa gives Banana OS apps - like glxinfo / eglinfo. Every
 * kind of context (OpenGL core and compatibility at their highest version,
 * OpenGL ES 1.1, 2.0 and 3.0 through EGL) with its version, renderer and
 * extensions, and a frame drawn in each to check it works.
 *     glinfo        the summary
 *     glinfo -e     ... and every extension
 */
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_ext;

static int count_ext(void) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    if (n > 0) return n;
    const char* e = (const char*)glGetString(GL_EXTENSIONS);
    int c = 0;
    for (; e && *e; e++) if (*e == ' ') c++;
    return c;
}

static void list_ext(void) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (int i = 0; i < n; i++) printf("    %s\n", (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i));
}

static void describe(const char* what) {
    printf("%s\n", what);
    printf("  version:  %s\n", (const char*)glGetString(GL_VERSION));
    printf("  renderer: %s (%s)\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VENDOR));
    const char* sl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (sl) printf("  shaders:  %s\n", sl);
    printf("  extensions: %d\n", count_ext());
    if (g_ext) list_ext();
}

/* the middle pixel after clearing to a colour: did the context draw? */
static void check(const char* what, int ok) {
    printf("  %-34s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) g_fail++;
}

static int clear_check(void) {
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = { 0 };
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px[1] > 200 && px[0] < 50;
}

static GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[300]; glGetShaderInfoLog(s, sizeof(log), NULL, log); printf("  shader: %s\n", log); }
    return ok ? s : 0;
}

/* a red triangle over the whole frame, OpenGL ES style: client-side arrays, no vertex array object */
static int triangle_check(const char* vs, const char* fs) {
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    glUseProgram(p);
    static const float tri[] = { -1, -1, 3, -1, -1, 3 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    unsigned char px[4] = { 0 };
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return px[0] > 200 && px[1] < 50;
}

static int es_context(EGLDisplay d, EGLConfig cfg, int major, EGLSurface* s, EGLContext* c) {
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, major, EGL_NONE };
    *c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
    if (*c == EGL_NO_CONTEXT) return 0;
    return eglMakeCurrent(d, *s, *s, *c);
}

int main(int argc, char** argv) {
    g_ext = argc > 1 && !strcmp(argv[1], "-e");

    /* desktop OpenGL */
    bgl3_ctx_t* core = bgl3_create_version(NULL, 32, 32, BGL3_CORE, 0, 0, NULL);
    if (core) {
        describe("OpenGL, core profile (the highest)");
        check("a frame drawn", clear_check());
        bgl3_destroy(core);
    } else { printf("OpenGL core: %s\n", bgl3_error()); g_fail++; }
    bgl3_ctx_t* compat = bgl3_create_version(NULL, 32, 32, BGL3_COMPAT, 0, 0, NULL);
    if (compat) {
        describe("OpenGL, compatibility profile");
        check("a frame drawn", clear_check());
        bgl3_destroy(compat);
    } else { printf("OpenGL compatibility: %s\n", bgl3_error()); g_fail++; }

    /* EGL and OpenGL ES */
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(d, &maj, &min)) { printf("EGL: could not start (0x%x)\n", eglGetError()); return 1; }
    printf("EGL %d.%d: %s, %s\n  APIs: %s\n", maj, min, eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION),
           eglQueryString(d, EGL_CLIENT_APIS));
    EGLConfig cfg;
    EGLint n = 0;
    EGLint want[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    eglChooseConfig(d, want, &cfg, 1, &n);
    EGLint pb[] = { EGL_WIDTH, 32, EGL_HEIGHT, 32, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pb);
    EGLContext c;

    if (es_context(d, cfg, 1, &s, &c)) {
        describe("OpenGL ES 1.1 (EGL)");
        check("a frame drawn", clear_check());
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(-1, 1, -1, 1, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        static const float tri[] = { -1, -1, 3, -1, -1, 3 };
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(2, GL_FLOAT, 0, tri);
        glColor4f(1, 0, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        unsigned char px[4] = { 0 };
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        check("the fixed pipeline (glOrthof, arrays)", px[0] > 200 && px[1] < 50);
        eglDestroyContext(d, c);
    } else { printf("OpenGL ES 1.1: no context (0x%x)\n", eglGetError()); g_fail++; }

    if (es_context(d, cfg, 2, &s, &c)) {
        describe("OpenGL ES 2.0 (EGL)");
        check("a frame drawn", clear_check());
        check("GLSL ES 1.00 (no #version line)", triangle_check(
            "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }",
            "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }"));
        eglDestroyContext(d, c);
    } else { printf("OpenGL ES 2.0: no context (0x%x)\n", eglGetError()); g_fail++; }

    if (es_context(d, cfg, 3, &s, &c)) {
        describe("OpenGL ES 3.0 (EGL)");
        check("a frame drawn", clear_check());
        check("GLSL ES 3.00 (#version 300 es)", triangle_check(
            "#version 300 es\nin vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }",
            "#version 300 es\nprecision mediump float; out vec4 color; void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }"));
        eglDestroyContext(d, c);
    } else { printf("OpenGL ES 3.0: no context (0x%x)\n", eglGetError()); g_fail++; }

    eglDestroySurface(d, s);
    eglTerminate(d);
    printf("glinfo: %s\n", g_fail ? "some checks FAILED" : "every check passed");
    return g_fail ? 1 : 0;
}
