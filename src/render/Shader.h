#pragma once
#include "core/GL.h"
#include "core/Math3D.h"
#include <string>
#include <unordered_map>

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
    int location(const char* name) const { return loc(name); } // cached

    void setMat4(const char* name, const Mat4& m) const;
    void setMat3(const char* name, const Mat3& m) const;
    void setVec2(const char* name, const Vec2& v) const;
    void setVec3(const char* name, const Vec3& v) const;
    void setVec4(const char* name, const Vec4& v) const;
    void setFloat(const char* name, float v) const;
    void setInt(const char* name, int v) const;

private:
    // uniform locations are cached per program (glGetUniformLocation per call
    // shows up in the hot draw path otherwise)
    int loc(const char* name) const {
        auto it = locCache_.find(name);
        if (it != locCache_.end()) return it->second;
        int l = glGetUniformLocation(prog_, name);
        locCache_.emplace(name, l);
        return l;
    }

    unsigned int prog_ = 0;
    mutable std::unordered_map<std::string, int> locCache_;
};

} // namespace ce
