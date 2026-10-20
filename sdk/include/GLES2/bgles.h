/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BGLES_H
#define BGLES_H

/*
 * OpenGL ES 2.0 - 3.2 for Banana OS apps (USE_EGL = 1): every function of
 * OpenGL 3.3 core comes from <GL/gl3.h>; these are the ES functions beyond
 * it, looked up in Mesa when an EGL context is made current (NULL when the
 * renderer lacks one: ask eglGetProcAddress, or check the GL_VERSION).
 * Include <GLES2/gl2.h> or <GLES3/gl3.h>, not this.
 */
#include <GL/gl3.h>

#define BGLES_PROCS(X) \
    X(PFNGLCLEARDEPTHFPROC, glClearDepthf) \
    X(PFNGLDEPTHRANGEFPROC, glDepthRangef) \
    X(PFNGLGETSHADERPRECISIONFORMATPROC, glGetShaderPrecisionFormat) \
    X(PFNGLRELEASESHADERCOMPILERPROC, glReleaseShaderCompiler) \
    X(PFNGLSHADERBINARYPROC, glShaderBinary) \
    X(PFNGLPROGRAMBINARYPROC, glProgramBinary) \
    X(PFNGLGETPROGRAMBINARYPROC, glGetProgramBinary) \
    X(PFNGLPROGRAMPARAMETERIPROC, glProgramParameteri) \
    X(PFNGLINVALIDATEFRAMEBUFFERPROC, glInvalidateFramebuffer) \
    X(PFNGLINVALIDATESUBFRAMEBUFFERPROC, glInvalidateSubFramebuffer) \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D) \
    X(PFNGLTEXSTORAGE3DPROC, glTexStorage3D) \
    X(PFNGLGETINTERNALFORMATIVPROC, glGetInternalformativ) \
    X(PFNGLBINDTRANSFORMFEEDBACKPROC, glBindTransformFeedback) \
    X(PFNGLDELETETRANSFORMFEEDBACKSPROC, glDeleteTransformFeedbacks) \
    X(PFNGLGENTRANSFORMFEEDBACKSPROC, glGenTransformFeedbacks) \
    X(PFNGLISTRANSFORMFEEDBACKPROC, glIsTransformFeedback) \
    X(PFNGLPAUSETRANSFORMFEEDBACKPROC, glPauseTransformFeedback) \
    X(PFNGLRESUMETRANSFORMFEEDBACKPROC, glResumeTransformFeedback) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) \
    X(PFNGLDISPATCHCOMPUTEINDIRECTPROC, glDispatchComputeIndirect) \
    X(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect) \
    X(PFNGLDRAWELEMENTSINDIRECTPROC, glDrawElementsIndirect) \
    X(PFNGLFRAMEBUFFERPARAMETERIPROC, glFramebufferParameteri) \
    X(PFNGLGETFRAMEBUFFERPARAMETERIVPROC, glGetFramebufferParameteriv) \
    X(PFNGLGETPROGRAMINTERFACEIVPROC, glGetProgramInterfaceiv) \
    X(PFNGLGETPROGRAMRESOURCEINDEXPROC, glGetProgramResourceIndex) \
    X(PFNGLGETPROGRAMRESOURCENAMEPROC, glGetProgramResourceName) \
    X(PFNGLGETPROGRAMRESOURCEIVPROC, glGetProgramResourceiv) \
    X(PFNGLGETPROGRAMRESOURCELOCATIONPROC, glGetProgramResourceLocation) \
    X(PFNGLUSEPROGRAMSTAGESPROC, glUseProgramStages) \
    X(PFNGLACTIVESHADERPROGRAMPROC, glActiveShaderProgram) \
    X(PFNGLCREATESHADERPROGRAMVPROC, glCreateShaderProgramv) \
    X(PFNGLBINDPROGRAMPIPELINEPROC, glBindProgramPipeline) \
    X(PFNGLDELETEPROGRAMPIPELINESPROC, glDeleteProgramPipelines) \
    X(PFNGLGENPROGRAMPIPELINESPROC, glGenProgramPipelines) \
    X(PFNGLISPROGRAMPIPELINEPROC, glIsProgramPipeline) \
    X(PFNGLGETPROGRAMPIPELINEIVPROC, glGetProgramPipelineiv) \
    X(PFNGLVALIDATEPROGRAMPIPELINEPROC, glValidateProgramPipeline) \
    X(PFNGLGETPROGRAMPIPELINEINFOLOGPROC, glGetProgramPipelineInfoLog) \
    X(PFNGLPROGRAMUNIFORM1IPROC, glProgramUniform1i) \
    X(PFNGLPROGRAMUNIFORM1FPROC, glProgramUniform1f) \
    X(PFNGLPROGRAMUNIFORM2FPROC, glProgramUniform2f) \
    X(PFNGLPROGRAMUNIFORM3FPROC, glProgramUniform3f) \
    X(PFNGLPROGRAMUNIFORM4FPROC, glProgramUniform4f) \
    X(PFNGLPROGRAMUNIFORM4FVPROC, glProgramUniform4fv) \
    X(PFNGLPROGRAMUNIFORMMATRIX4FVPROC, glProgramUniformMatrix4fv) \
    X(PFNGLBINDIMAGETEXTUREPROC, glBindImageTexture) \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) \
    X(PFNGLMEMORYBARRIERBYREGIONPROC, glMemoryBarrierByRegion) \
    X(PFNGLTEXSTORAGE2DMULTISAMPLEPROC, glTexStorage2DMultisample) \
    X(PFNGLTEXSTORAGE3DMULTISAMPLEPROC, glTexStorage3DMultisample) \
    X(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer) \
    X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat) \
    X(PFNGLVERTEXATTRIBIFORMATPROC, glVertexAttribIFormat) \
    X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding) \
    X(PFNGLVERTEXBINDINGDIVISORPROC, glVertexBindingDivisor) \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData) \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl) \
    X(PFNGLDEBUGMESSAGEINSERTPROC, glDebugMessageInsert) \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback) \
    X(PFNGLGETDEBUGMESSAGELOGPROC, glGetDebugMessageLog) \
    X(PFNGLPUSHDEBUGGROUPPROC, glPushDebugGroup) \
    X(PFNGLPOPDEBUGGROUPPROC, glPopDebugGroup) \
    X(PFNGLOBJECTLABELPROC, glObjectLabel) \
    X(PFNGLGETOBJECTLABELPROC, glGetObjectLabel) \
    X(PFNGLGETPOINTERVPROC, glGetPointerv) \
    X(PFNGLBLENDEQUATIONIPROC, glBlendEquationi) \
    X(PFNGLBLENDEQUATIONSEPARATEIPROC, glBlendEquationSeparatei) \
    X(PFNGLBLENDFUNCIPROC, glBlendFunci) \
    X(PFNGLBLENDFUNCSEPARATEIPROC, glBlendFuncSeparatei) \
    X(PFNGLGETGRAPHICSRESETSTATUSPROC, glGetGraphicsResetStatus) \
    X(PFNGLREADNPIXELSPROC, glReadnPixels) \
    X(PFNGLMINSAMPLESHADINGPROC, glMinSampleShading) \
    X(PFNGLPATCHPARAMETERIPROC, glPatchParameteri) \
    X(PFNGLTEXBUFFERRANGEPROC, glTexBufferRange)

#define X(type, name) extern type bgles_##name;
BGLES_PROCS(X)
#undef X

#define glClearDepthf bgles_glClearDepthf
#define glDepthRangef bgles_glDepthRangef
#define glGetShaderPrecisionFormat bgles_glGetShaderPrecisionFormat
#define glReleaseShaderCompiler bgles_glReleaseShaderCompiler
#define glShaderBinary bgles_glShaderBinary
#define glProgramBinary bgles_glProgramBinary
#define glGetProgramBinary bgles_glGetProgramBinary
#define glProgramParameteri bgles_glProgramParameteri
#define glInvalidateFramebuffer bgles_glInvalidateFramebuffer
#define glInvalidateSubFramebuffer bgles_glInvalidateSubFramebuffer
#define glTexStorage2D bgles_glTexStorage2D
#define glTexStorage3D bgles_glTexStorage3D
#define glGetInternalformativ bgles_glGetInternalformativ
#define glBindTransformFeedback bgles_glBindTransformFeedback
#define glDeleteTransformFeedbacks bgles_glDeleteTransformFeedbacks
#define glGenTransformFeedbacks bgles_glGenTransformFeedbacks
#define glIsTransformFeedback bgles_glIsTransformFeedback
#define glPauseTransformFeedback bgles_glPauseTransformFeedback
#define glResumeTransformFeedback bgles_glResumeTransformFeedback
#define glDispatchCompute bgles_glDispatchCompute
#define glDispatchComputeIndirect bgles_glDispatchComputeIndirect
#define glDrawArraysIndirect bgles_glDrawArraysIndirect
#define glDrawElementsIndirect bgles_glDrawElementsIndirect
#define glFramebufferParameteri bgles_glFramebufferParameteri
#define glGetFramebufferParameteriv bgles_glGetFramebufferParameteriv
#define glGetProgramInterfaceiv bgles_glGetProgramInterfaceiv
#define glGetProgramResourceIndex bgles_glGetProgramResourceIndex
#define glGetProgramResourceName bgles_glGetProgramResourceName
#define glGetProgramResourceiv bgles_glGetProgramResourceiv
#define glGetProgramResourceLocation bgles_glGetProgramResourceLocation
#define glUseProgramStages bgles_glUseProgramStages
#define glActiveShaderProgram bgles_glActiveShaderProgram
#define glCreateShaderProgramv bgles_glCreateShaderProgramv
#define glBindProgramPipeline bgles_glBindProgramPipeline
#define glDeleteProgramPipelines bgles_glDeleteProgramPipelines
#define glGenProgramPipelines bgles_glGenProgramPipelines
#define glIsProgramPipeline bgles_glIsProgramPipeline
#define glGetProgramPipelineiv bgles_glGetProgramPipelineiv
#define glValidateProgramPipeline bgles_glValidateProgramPipeline
#define glGetProgramPipelineInfoLog bgles_glGetProgramPipelineInfoLog
#define glProgramUniform1i bgles_glProgramUniform1i
#define glProgramUniform1f bgles_glProgramUniform1f
#define glProgramUniform2f bgles_glProgramUniform2f
#define glProgramUniform3f bgles_glProgramUniform3f
#define glProgramUniform4f bgles_glProgramUniform4f
#define glProgramUniform4fv bgles_glProgramUniform4fv
#define glProgramUniformMatrix4fv bgles_glProgramUniformMatrix4fv
#define glBindImageTexture bgles_glBindImageTexture
#define glMemoryBarrier bgles_glMemoryBarrier
#define glMemoryBarrierByRegion bgles_glMemoryBarrierByRegion
#define glTexStorage2DMultisample bgles_glTexStorage2DMultisample
#define glTexStorage3DMultisample bgles_glTexStorage3DMultisample
#define glBindVertexBuffer bgles_glBindVertexBuffer
#define glVertexAttribFormat bgles_glVertexAttribFormat
#define glVertexAttribIFormat bgles_glVertexAttribIFormat
#define glVertexAttribBinding bgles_glVertexAttribBinding
#define glVertexBindingDivisor bgles_glVertexBindingDivisor
#define glCopyImageSubData bgles_glCopyImageSubData
#define glDebugMessageControl bgles_glDebugMessageControl
#define glDebugMessageInsert bgles_glDebugMessageInsert
#define glDebugMessageCallback bgles_glDebugMessageCallback
#define glGetDebugMessageLog bgles_glGetDebugMessageLog
#define glPushDebugGroup bgles_glPushDebugGroup
#define glPopDebugGroup bgles_glPopDebugGroup
#define glObjectLabel bgles_glObjectLabel
#define glGetObjectLabel bgles_glGetObjectLabel
#define glBlendEquationi bgles_glBlendEquationi
#define glBlendEquationSeparatei bgles_glBlendEquationSeparatei
#define glBlendFunci bgles_glBlendFunci
#define glBlendFuncSeparatei bgles_glBlendFuncSeparatei
#define glGetGraphicsResetStatus bgles_glGetGraphicsResetStatus
#define glReadnPixels bgles_glReadnPixels
#define glMinSampleShading bgles_glMinSampleShading
#define glPatchParameteri bgles_glPatchParameteri
#define glTexBufferRange bgles_glTexBufferRange

#endif
