#pragma once
// Minimal OpenGL 3.3 core loader (avoids glad/glew dependency).
// Function pointers are resolved via glfwGetProcAddress at startup.
#include <GL/gl.h>
#include <GL/glext.h>

// ---- constants that may be missing from old gl.h ----
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

// ---- function pointer declarations ----
#define CE_GL_FUNCS(X) \
    X(PFNGLGENVERTEXARRAYSPROC,    glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,    glBindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays) \
    X(PFNGLGENBUFFERSPROC,         glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,         glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,         glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,      glBufferSubData) \
    X(PFNGLDELETEBUFFERSPROC,      glDeleteBuffers) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,    glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLCREATESHADERPROC,       glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,       glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,      glCompileShader) \
    X(PFNGLGETSHADERIVPROC,        glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,   glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,       glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,      glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,       glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,        glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,       glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,  glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,         glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC,      glDeleteProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,          glUniform1i) \
    X(PFNGLUNIFORM1FPROC,          glUniform1f) \
    X(PFNGLUNIFORM2FPROC,          glUniform2f) \
    X(PFNGLUNIFORM3FPROC,          glUniform3f) \
    X(PFNGLUNIFORM4FPROC,          glUniform4f) \
    X(PFNGLUNIFORMMATRIX3FVPROC,   glUniformMatrix3fv) \
    X(PFNGLUNIFORMMATRIX4FVPROC,   glUniformMatrix4fv) \
    X(PFNGLACTIVETEXTUREPROC,      glActiveTexture) \
    X(PFNGLGENERATEMIPMAPPROC,     glGenerateMipmap)

#define CE_GL_DECLARE(type, name) extern type name;
CE_GL_FUNCS(CE_GL_DECLARE)
#undef CE_GL_DECLARE

namespace ce {
// Loads all function pointers. Must be called after a GL context is current.
bool loadGLFunctions();
} // namespace ce
