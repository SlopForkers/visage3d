#pragma once
#include "core/Math3D.h"
#include <string>

namespace ce {

class Shader {
public:
    Shader() = default;
    ~Shader();
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    bool compile(const char* vsSrc, const char* fsSrc, std::string& error);
    void use() const;
    unsigned int id() const { return prog_; }

    void setMat4(const char* name, const Mat4& m) const;
    void setMat3(const char* name, const Mat3& m) const;
    void setVec3(const char* name, const Vec3& v) const;
    void setVec4(const char* name, const Vec4& v) const;
    void setFloat(const char* name, float v) const;
    void setInt(const char* name, int v) const;

private:
    unsigned int prog_ = 0;
};

} // namespace ce
