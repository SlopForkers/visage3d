#include "core/GL.h"
#include <GLFW/glfw3.h>
#include <cstdio>

#define CE_GL_DEFINE(type, name) type name = nullptr;
CE_GL_FUNCS(CE_GL_DEFINE)
#undef CE_GL_DEFINE

namespace ce {

bool loadGLFunctions() {
    bool ok = true;
#define CE_GL_LOAD(type, name)                                              \
    name = reinterpret_cast<type>(glfwGetProcAddress(#name));               \
    if (!name) { std::fprintf(stderr, "GL loader: missing %s\n", #name); ok = false; }
    CE_GL_FUNCS(CE_GL_LOAD)
#undef CE_GL_LOAD
    return ok;
}

} // namespace ce
