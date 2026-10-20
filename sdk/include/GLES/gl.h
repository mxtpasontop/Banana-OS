/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BANANA_GLES1_H
#define BANANA_GLES1_H

/*
 * OpenGL ES 1.1 (USE_EGL = 1): the fixed-function pipeline - matrices,
 * lighting, fog, texture environments, client-side vertex arrays - in an
 * EGL context made with EGL_CONTEXT_CLIENT_VERSION 1 (Mesa's compatibility
 * profile). The functions shared with OpenGL ES 2 / OpenGL 3 come from
 * <GL/gl3.h>; these are looked up when the context is made current.
 */
#define GL_VERSION_ES_CM_1_0 1
#define GL_VERSION_ES_CL_1_0 1
#define GL_VERSION_ES_CM_1_1 1
#define GL_VERSION_ES_CL_1_1 1
#include <GLES2/bgles.h>

#define BGLES1_PROCS(X) \
    X(void, glAlphaFunc, (GLenum func, GLfloat ref)) \
    X(void, glClientActiveTexture, (GLenum texture)) \
    X(void, glColor4f, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glColor4ub, (GLubyte r, GLubyte g, GLubyte b, GLubyte a)) \
    X(void, glColorPointer, (GLint size, GLenum type, GLsizei stride, const void* p)) \
    X(void, glDisableClientState, (GLenum array)) \
    X(void, glEnableClientState, (GLenum array)) \
    X(void, glFogf, (GLenum pname, GLfloat param)) \
    X(void, glFogfv, (GLenum pname, const GLfloat* params)) \
    X(void, glGetLightfv, (GLenum light, GLenum pname, GLfloat* params)) \
    X(void, glGetMaterialfv, (GLenum face, GLenum pname, GLfloat* params)) \
    X(void, glGetTexEnvfv, (GLenum env, GLenum pname, GLfloat* params)) \
    X(void, glGetTexEnviv, (GLenum env, GLenum pname, GLint* params)) \
    X(void, glLightModelf, (GLenum pname, GLfloat param)) \
    X(void, glLightModelfv, (GLenum pname, const GLfloat* params)) \
    X(void, glLightf, (GLenum light, GLenum pname, GLfloat param)) \
    X(void, glLightfv, (GLenum light, GLenum pname, const GLfloat* params)) \
    X(void, glLoadIdentity, (void)) \
    X(void, glLoadMatrixf, (const GLfloat* m)) \
    X(void, glMaterialf, (GLenum face, GLenum pname, GLfloat param)) \
    X(void, glMaterialfv, (GLenum face, GLenum pname, const GLfloat* params)) \
    X(void, glMatrixMode, (GLenum mode)) \
    X(void, glMultMatrixf, (const GLfloat* m)) \
    X(void, glMultiTexCoord4f, (GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q)) \
    X(void, glNormal3f, (GLfloat nx, GLfloat ny, GLfloat nz)) \
    X(void, glNormalPointer, (GLenum type, GLsizei stride, const void* p)) \
    X(void, glPopMatrix, (void)) \
    X(void, glPushMatrix, (void)) \
    X(void, glRotatef, (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)) \
    X(void, glScalef, (GLfloat x, GLfloat y, GLfloat z)) \
    X(void, glShadeModel, (GLenum mode)) \
    X(void, glTexCoordPointer, (GLint size, GLenum type, GLsizei stride, const void* p)) \
    X(void, glTexEnvf, (GLenum target, GLenum pname, GLfloat param)) \
    X(void, glTexEnvfv, (GLenum target, GLenum pname, const GLfloat* params)) \
    X(void, glTexEnvi, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexEnviv, (GLenum target, GLenum pname, const GLint* params)) \
    X(void, glTranslatef, (GLfloat x, GLfloat y, GLfloat z)) \
    X(void, glVertexPointer, (GLint size, GLenum type, GLsizei stride, const void* p)) \
    X(void, glOrthof, (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)) \
    X(void, glFrustumf, (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)) \
    X(void, glClipPlanef, (GLenum plane, const GLfloat* equation))

#define X(ret, name, args) extern ret (*bgles1_##name) args;
BGLES1_PROCS(X)
#undef X

#define glAlphaFunc bgles1_glAlphaFunc
#define glClientActiveTexture bgles1_glClientActiveTexture
#define glColor4f bgles1_glColor4f
#define glColor4ub bgles1_glColor4ub
#define glColorPointer bgles1_glColorPointer
#define glDisableClientState bgles1_glDisableClientState
#define glEnableClientState bgles1_glEnableClientState
#define glFogf bgles1_glFogf
#define glFogfv bgles1_glFogfv
#define glGetLightfv bgles1_glGetLightfv
#define glGetMaterialfv bgles1_glGetMaterialfv
#define glGetTexEnvfv bgles1_glGetTexEnvfv
#define glGetTexEnviv bgles1_glGetTexEnviv
#define glLightModelf bgles1_glLightModelf
#define glLightModelfv bgles1_glLightModelfv
#define glLightf bgles1_glLightf
#define glLightfv bgles1_glLightfv
#define glLoadIdentity bgles1_glLoadIdentity
#define glLoadMatrixf bgles1_glLoadMatrixf
#define glMaterialf bgles1_glMaterialf
#define glMaterialfv bgles1_glMaterialfv
#define glMatrixMode bgles1_glMatrixMode
#define glMultMatrixf bgles1_glMultMatrixf
#define glMultiTexCoord4f bgles1_glMultiTexCoord4f
#define glNormal3f bgles1_glNormal3f
#define glNormalPointer bgles1_glNormalPointer
#define glPopMatrix bgles1_glPopMatrix
#define glPushMatrix bgles1_glPushMatrix
#define glRotatef bgles1_glRotatef
#define glScalef bgles1_glScalef
#define glShadeModel bgles1_glShadeModel
#define glTexCoordPointer bgles1_glTexCoordPointer
#define glTexEnvf bgles1_glTexEnvf
#define glTexEnvfv bgles1_glTexEnvfv
#define glTexEnvi bgles1_glTexEnvi
#define glTexEnviv bgles1_glTexEnviv
#define glTranslatef bgles1_glTranslatef
#define glVertexPointer bgles1_glVertexPointer
#define glOrthof bgles1_glOrthof
#define glFrustumf bgles1_glFrustumf
#define glClipPlanef bgles1_glClipPlanef

/* the fixed pipeline's names (not in OpenGL 3 core) */
#define GL_CURRENT_COLOR               0x0B00
#define GL_POINT_SMOOTH                0x0B10
#define GL_LIGHTING                    0x0B50
#define GL_LIGHT_MODEL_TWO_SIDE        0x0B52
#define GL_LIGHT_MODEL_AMBIENT         0x0B53
#define GL_COLOR_MATERIAL              0x0B57
#define GL_FOG                         0x0B60
#define GL_FOG_DENSITY                 0x0B62
#define GL_FOG_START                   0x0B63
#define GL_FOG_END                     0x0B64
#define GL_FOG_MODE                    0x0B65
#define GL_FOG_COLOR                   0x0B66
#define GL_MATRIX_MODE                 0x0BA0
#define GL_NORMALIZE                   0x0BA1
#define GL_MODELVIEW_MATRIX            0x0BA6
#define GL_PROJECTION_MATRIX           0x0BA7
#define GL_TEXTURE_MATRIX              0x0BA8
#define GL_ALPHA_TEST                  0x0BC0
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_POINT_SMOOTH_HINT           0x0C51
#define GL_FOG_HINT                    0x0C54
#define GL_MAX_LIGHTS                  0x0D31
#define GL_MAX_MODELVIEW_STACK_DEPTH   0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH  0x0D38
#define GL_MAX_TEXTURE_UNITS           0x84E2
#define GL_AMBIENT                     0x1200
#define GL_DIFFUSE                     0x1201
#define GL_SPECULAR                    0x1202
#define GL_POSITION                    0x1203
#define GL_SPOT_DIRECTION              0x1204
#define GL_SPOT_EXPONENT               0x1205
#define GL_SPOT_CUTOFF                 0x1206
#define GL_CONSTANT_ATTENUATION        0x1207
#define GL_LINEAR_ATTENUATION          0x1208
#define GL_QUADRATIC_ATTENUATION       0x1209
#define GL_EMISSION                    0x1600
#define GL_SHININESS                   0x1601
#define GL_AMBIENT_AND_DIFFUSE         0x1602
#define GL_MODELVIEW                   0x1700
#define GL_PROJECTION                  0x1701
#define GL_LUMINANCE                   0x1909
#define GL_LUMINANCE_ALPHA             0x190A
#define GL_FLAT                        0x1D00
#define GL_SMOOTH                      0x1D01
#define GL_MODULATE                    0x2100
#define GL_DECAL                       0x2101
#define GL_ADD                         0x0104
#define GL_TEXTURE_ENV_MODE            0x2200
#define GL_TEXTURE_ENV_COLOR           0x2201
#define GL_TEXTURE_ENV                 0x2300
#define GL_EXP                         0x0800
#define GL_EXP2                        0x0801
#define GL_LIGHT0                      0x4000
#define GL_LIGHT1                      0x4001
#define GL_LIGHT2                      0x4002
#define GL_LIGHT3                      0x4003
#define GL_LIGHT4                      0x4004
#define GL_LIGHT5                      0x4005
#define GL_LIGHT6                      0x4006
#define GL_LIGHT7                      0x4007
#define GL_RESCALE_NORMAL              0x803A
#define GL_VERTEX_ARRAY                0x8074
#define GL_NORMAL_ARRAY                0x8075
#define GL_COLOR_ARRAY                 0x8076
#define GL_TEXTURE_COORD_ARRAY         0x8078
#define GL_POINT_SIZE_MIN              0x8126
#define GL_POINT_SIZE_MAX              0x8127
#define GL_POINT_DISTANCE_ATTENUATION  0x8129
#define GL_GENERATE_MIPMAP             0x8191
#define GL_COMBINE                     0x8570
#define GL_COMBINE_RGB                 0x8571
#define GL_COMBINE_ALPHA               0x8572
#define GL_POINT_SPRITE_OES            0x8861
#define GL_COORD_REPLACE_OES           0x8862

#endif
