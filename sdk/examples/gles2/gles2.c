/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * gles2: an OpenGL ES 2.0 window through EGL - a spinning triangle with a
 * colour per corner, written as a phone or Raspberry Pi app would be
 * (shaders in GLSL ES 1.00, client-side vertex arrays). Esc or closing the
 * window quits.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <banana.h>
#include <math.h>
#include <stdio.h>

static const char* VS =
    "attribute vec2 pos;\n"
    "attribute vec3 col;\n"
    "uniform float angle;\n"
    "uniform float aspect;\n"
    "varying vec3 v_col;\n"
    "void main() {\n"
    "    float c = cos(angle), s = sin(angle);\n"
    "    vec2 p = vec2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);\n"
    "    gl_Position = vec4(p.x / aspect, p.y, 0.0, 1.0);\n"
    "    v_col = col;\n"
    "}\n";
static const char* FS =
    "precision mediump float;\n"
    "varying vec3 v_col;\n"
    "void main() { gl_FragColor = vec4(v_col, 1.0); }\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[400]; glGetShaderInfoLog(s, sizeof(log), NULL, log); printf("shader: %s\n", log); }
    return s;
}

int main(void) {
    bwin_t win;
    if (bwin_open(&win, "OpenGL ES 2.0", 480, 360) < 0) { printf("gles2: no desktop (run it in a terminal window)\n"); return 1; }
    bwin_resizable(&win, 160, 120);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) { printf("gles2: EGL could not start - is libOSMesa.so installed?\n"); return 1; }
    EGLConfig cfg;
    EGLint n;
    EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 16, EGL_NONE };
    eglChooseConfig(d, want, &cfg, 1, &n);
    EGLSurface s = eglCreateWindowSurface(d, cfg, (EGLNativeWindowType)&win, NULL);
    EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
    if (!eglMakeCurrent(d, s, s, c)) { printf("gles2: no OpenGL ES 2.0 context (0x%x)\n", eglGetError()); return 1; }
    printf("%s\n%s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "col");
    glLinkProgram(p);
    glUseProgram(p);
    GLint u_angle = glGetUniformLocation(p, "angle"), u_aspect = glGetUniformLocation(p, "aspect");
    static const GLfloat pos[] = { 0.0f, 0.8f, -0.7f, -0.45f, 0.7f, -0.45f };
    static const GLfloat col[] = { 1, 0.85f, 0.2f, 0.2f, 0.6f, 1, 1, 0.3f, 0.4f };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, col);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    float angle = 0;
    for (;;) {
        banana_event_t ev;
        while (bwin_event(&win, &ev)) {
            if (ev.type == BANANA_EV_CLOSE || (ev.type == BANANA_EV_KEY && (ev.key == 27 || ev.key == 'q'))) { eglTerminate(d); bwin_close(&win); return 0; }
        }
        glViewport(0, 0, win.w, win.h);
        glClearColor(0.08f, 0.10f, 0.16f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1f(u_angle, angle);
        glUniform1f(u_aspect, win.h ? (float)win.w / (float)win.h : 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        eglSwapBuffers(d, s);
        angle += 0.03f;
    }
}
