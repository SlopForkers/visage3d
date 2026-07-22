#include "render/Shader.h"
#include "core/GL.h"

namespace ce {

Shader::~Shader() {
    if (prog_) glDeleteProgram(prog_);
}

bool Shader::compile(const char* vsSrc, const char* fsSrc, std::string& error) {
    auto compileStage = [&](unsigned int type, const char* src, std::string& err) -> unsigned int {
        unsigned int sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        int ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            err = log;
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };

    unsigned int vs = compileStage(GL_VERTEX_SHADER, vsSrc, error);
    if (!vs) return false;
    unsigned int fs = compileStage(GL_FRAGMENT_SHADER, fsSrc, error);
    if (!fs) { glDeleteShader(vs); return false; }

    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    int ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog_, sizeof(log), nullptr, log);
        error = log;
        glDeleteProgram(prog_);
        prog_ = 0;
        return false;
    }
    return true;
}

void Shader::use() const { glUseProgram(prog_); }

void Shader::setMat4(const char* name, const Mat4& m) const {
    glUniformMatrix4fv(loc(name), 1, GL_FALSE, m.m);
}
void Shader::setMat3(const char* name, const Mat3& m) const {
    glUniformMatrix3fv(loc(name), 1, GL_FALSE, m.m);
}
void Shader::setVec2(const char* name, const Vec2& v) const {
    glUniform2f(loc(name), v.x, v.y);
}
void Shader::setVec3(const char* name, const Vec3& v) const {
    glUniform3f(loc(name), v.x, v.y, v.z);
}
void Shader::setVec4(const char* name, const Vec4& v) const {
    glUniform4f(loc(name), v.x, v.y, v.z, v.w);
}
void Shader::setFloat(const char* name, float v) const {
    glUniform1f(loc(name), v);
}
void Shader::setInt(const char* name, int v) const {
    glUniform1i(loc(name), v);
}

} // namespace ce
