#include "model/GltfLoader.h"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include <algorithm>
#include <cstring>

namespace ce {

namespace {

int numComponents(int type) {
    switch (type) {
        case TINYGLTF_TYPE_SCALAR: return 1;
        case TINYGLTF_TYPE_VEC2:   return 2;
        case TINYGLTF_TYPE_VEC3:   return 3;
        case TINYGLTF_TYPE_VEC4:   return 4;
        case TINYGLTF_TYPE_MAT4:   return 16;
        default: return 0;
    }
}

int componentSize(int componentType) {
    switch (componentType) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_INT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:          return 4;
        default: return 0;
    }
}

// Reads any numeric accessor as floats (FLOAT, or normalized integer formats).
std::vector<float> readFloats(const tinygltf::Model& m, int accIdx) {
    std::vector<float> out;
    if (accIdx < 0 || accIdx >= static_cast<int>(m.accessors.size())) return out;
    const tinygltf::Accessor& acc = m.accessors[accIdx];
    const tinygltf::BufferView& bv = m.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    int nc = numComponents(acc.type);
    int cs = componentSize(acc.componentType);
    size_t elemSize = static_cast<size_t>(nc) * cs;
    size_t stride = bv.byteStride ? bv.byteStride : elemSize;
    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(acc.count * nc);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = base + i * stride;
        for (int c = 0; c < nc; ++c) {
            float v = 0.f;
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    std::memcpy(&v, p + c * 4, 4);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    v = acc.normalized ? p[c] / 255.f : static_cast<float>(p[c]);
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    uint16_t t;
                    std::memcpy(&t, p + c * 2, 2);
                    v = acc.normalized ? t / 65535.f : static_cast<float>(t);
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_SHORT: {
                    int16_t t;
                    std::memcpy(&t, p + c * 2, 2);
                    v = acc.normalized ? std::max(t / 32767.f, -1.f) : static_cast<float>(t);
                    break;
                }
                default: break;
            }
            out[i * nc + c] = v;
        }
    }
    return out;
}

std::vector<uint32_t> readIndices(const tinygltf::Model& m, int accIdx) {
    std::vector<uint32_t> out;
    if (accIdx < 0) return out;
    const tinygltf::Accessor& acc = m.accessors[accIdx];
    const tinygltf::BufferView& bv = m.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    int cs = componentSize(acc.componentType);
    size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(cs);
    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = base + i * stride;
        uint32_t v = 0;
        switch (acc.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  v = p[0]; break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: { uint16_t t; std::memcpy(&t, p, 2); v = t; } break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   std::memcpy(&v, p, 4); break;
            default: break;
        }
        out[i] = v;
    }
    return out;
}

std::vector<uint16_t> readJoints(const tinygltf::Model& m, int accIdx) {
    std::vector<uint16_t> out;
    if (accIdx < 0) return out;
    const tinygltf::Accessor& acc = m.accessors[accIdx];
    const tinygltf::BufferView& bv = m.bufferViews[acc.bufferView];
    const tinygltf::Buffer& buf = m.buffers[bv.buffer];
    int nc = numComponents(acc.type); // 4
    int cs = componentSize(acc.componentType);
    size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(nc * cs);
    const uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(acc.count * nc);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = base + i * stride;
        for (int c = 0; c < nc; ++c) {
            uint16_t v = 0;
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    v = p[c];
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    std::memcpy(&v, p + c * 2, 2);
                    break;
                case TINYGLTF_COMPONENT_TYPE_SHORT: {
                    int16_t t;
                    std::memcpy(&t, p + c * 2, 2);
                    v = static_cast<uint16_t>(std::max<int>(t, 0));
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_FLOAT: { // VRoid body stores joints as float
                    float f;
                    std::memcpy(&f, p + c * 4, 4);
                    v = static_cast<uint16_t>(f + 0.5f);
                    break;
                }
                default: break;
            }
            out[i * nc + c] = v;
        }
    }
    return out;
}

std::vector<std::string> targetNames(const tinygltf::Mesh& mesh, size_t count) {
    std::vector<std::string> names;
    if (mesh.extras.Has("targetNames")) {
        const tinygltf::Value& arr = mesh.extras.Get("targetNames");
        if (arr.IsArray())
            for (int i = 0; i < arr.ArrayLen(); ++i)
                names.push_back(arr.Get(i).Get<std::string>());
    }
    while (names.size() < count)
        names.push_back("morph_" + std::to_string(names.size()));
    return names;
}

Quat mat3ToQuat(const Mat3& r) {
    // r should be a pure rotation matrix; column-major layout m[col*3 + row]
    float trace = r.m[0] + r.m[4] + r.m[8];
    Quat q;
    if (trace > 0.f) {
        float s = std::sqrt(trace + 1.f) * 2.f;
        q.w = 0.25f * s;
        q.x = (r.m[5] - r.m[7]) / s;
        q.y = (r.m[6] - r.m[2]) / s;
        q.z = (r.m[1] - r.m[3]) / s;
    } else if (r.m[0] > r.m[4] && r.m[0] > r.m[8]) {
        float s = std::sqrt(1.f + r.m[0] - r.m[4] - r.m[8]) * 2.f;
        q.w = (r.m[5] - r.m[7]) / s;
        q.x = 0.25f * s;
        q.y = (r.m[3] + r.m[1]) / s;
        q.z = (r.m[6] + r.m[2]) / s;
    } else if (r.m[4] > r.m[8]) {
        float s = std::sqrt(1.f + r.m[4] - r.m[0] - r.m[8]) * 2.f;
        q.w = (r.m[6] - r.m[2]) / s;
        q.x = (r.m[3] + r.m[1]) / s;
        q.y = 0.25f * s;
        q.z = (r.m[7] + r.m[5]) / s;
    } else {
        float s = std::sqrt(1.f + r.m[8] - r.m[0] - r.m[4]) * 2.f;
        q.w = (r.m[1] - r.m[3]) / s;
        q.x = (r.m[6] + r.m[2]) / s;
        q.y = (r.m[7] + r.m[5]) / s;
        q.z = 0.25f * s;
    }
    return q.normalized();
}

void decomposeMatrix(const Mat4& m, Vec3& t, Quat& r, Vec3& s) {
    t = {m.m[12], m.m[13], m.m[14]};
    Mat3 r3 = m.upper3x3();
    Vec3 col0{r3.m[0], r3.m[1], r3.m[2]};
    Vec3 col1{r3.m[3], r3.m[4], r3.m[5]};
    Vec3 col2{r3.m[6], r3.m[7], r3.m[8]};
    s = {col0.length(), col1.length(), col2.length()};
    if (s.x > 1e-8f) { r3.m[0] /= s.x; r3.m[1] /= s.x; r3.m[2] /= s.x; }
    if (s.y > 1e-8f) { r3.m[3] /= s.y; r3.m[4] /= s.y; r3.m[5] /= s.y; }
    if (s.z > 1e-8f) { r3.m[6] /= s.z; r3.m[7] /= s.z; r3.m[8] /= s.z; }
    r = mat3ToQuat(r3);
}

} // namespace

bool GltfLoader::load(const std::string& path, Model& out, std::string& error) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model g;
    std::string err, warn;

    // Binary detection by magic number, not by extension.
    bool binary = true;
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { error = "Cannot open file: " + path; return false; }
        char magic[4] = {0, 0, 0, 0};
        size_t n = std::fread(magic, 1, 4, f);
        std::fclose(f);
        binary = (n == 4 && std::memcmp(magic, "glTF", 4) == 0);
    }

    bool ok = binary ? loader.LoadBinaryFromFile(&g, &err, &warn, path)
                     : loader.LoadASCIIFromFile(&g, &err, &warn, path);
    if (!ok) { error = "glTF load failed: " + err; return false; }

    out = Model{};
    out.filePath = path;
    size_t slash = path.find_last_of("/\\");
    out.fileName = (slash == std::string::npos) ? path : path.substr(slash + 1);

    // ---- textures ----
    for (const tinygltf::Image& img : g.images) {
        Texture tex;
        tex.width = img.width;
        tex.height = img.height;
        if (img.component == 4) {
            tex.rgba = img.image;
        } else if (img.component == 3) {
            tex.rgba.resize(static_cast<size_t>(img.width) * img.height * 4);
            for (size_t i = 0; i < static_cast<size_t>(img.width) * img.height; ++i) {
                tex.rgba[i * 4 + 0] = img.image[i * 3 + 0];
                tex.rgba[i * 4 + 1] = img.image[i * 3 + 1];
                tex.rgba[i * 4 + 2] = img.image[i * 3 + 2];
                tex.rgba[i * 4 + 3] = 255;
            }
        } else {
            tex.width = tex.height = 0; // unsupported; white texture will be used
        }
        out.textures.push_back(std::move(tex));
    }

    // ---- materials ----
    for (const tinygltf::Material& gm : g.materials) {
        Material mat;
        mat.name = gm.name;
        const auto& pbr = gm.pbrMetallicRoughness;
        if (pbr.baseColorTexture.index >= 0) {
            int src = g.textures[pbr.baseColorTexture.index].source;
            if (src >= 0) mat.texture = src;
        }
        if (pbr.baseColorFactor.size() == 4)
            mat.baseColor = Vec4{static_cast<float>(pbr.baseColorFactor[0]),
                                 static_cast<float>(pbr.baseColorFactor[1]),
                                 static_cast<float>(pbr.baseColorFactor[2]),
                                 static_cast<float>(pbr.baseColorFactor[3])};
        if (gm.alphaMode == "MASK") mat.alpha = Material::Alpha::Mask;
        else if (gm.alphaMode == "BLEND") mat.alpha = Material::Alpha::Blend;
        mat.alphaCutoff = static_cast<float>(gm.alphaCutoff);
        mat.doubleSided = gm.doubleSided;
        mat.unlit = gm.extensions.count("KHR_materials_unlit") > 0;
        out.materials.push_back(std::move(mat));
    }

    // ---- meshes ----
    for (const tinygltf::Mesh& gm : g.meshes) {
        Mesh mesh;
        mesh.name = gm.name;
        for (const tinygltf::Primitive& gp : gm.primitives) {
            Primitive prim;
            prim.material = gp.material;

            auto attr3 = [&](const char* name) {
                std::vector<Vec3> r;
                auto it = gp.attributes.find(name);
                if (it == gp.attributes.end()) return r;
                std::vector<float> f = readFloats(g, it->second);
                r.resize(f.size() / 3);
                for (size_t i = 0; i < r.size(); ++i) r[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
                return r;
            };
            prim.pos = attr3("POSITION");
            prim.normal = attr3("NORMAL");
            {
                auto it = gp.attributes.find("TEXCOORD_0");
                if (it != gp.attributes.end()) {
                    std::vector<float> f = readFloats(g, it->second);
                    prim.uv.resize(f.size() / 2);
                    for (size_t i = 0; i < prim.uv.size(); ++i) prim.uv[i] = {f[i * 2], f[i * 2 + 1]};
                }
            }
            {
                auto it = gp.attributes.find("JOINTS_0");
                if (it != gp.attributes.end()) prim.joints = readJoints(g, it->second);
            }
            {
                auto it = gp.attributes.find("WEIGHTS_0");
                if (it != gp.attributes.end()) {
                    std::vector<float> f = readFloats(g, it->second);
                    prim.weights.resize(f.size() / 4);
                    for (size_t i = 0; i < prim.weights.size(); ++i)
                        prim.weights[i] = {f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]};
                }
            }
            prim.indices = readIndices(g, gp.indices);
            if (prim.indices.empty()) { // non-indexed: generate sequential
                prim.indices.resize(prim.pos.size());
                for (size_t i = 0; i < prim.indices.size(); ++i) prim.indices[i] = static_cast<uint32_t>(i);
            }

            // morph targets
            std::vector<std::string> names = targetNames(gm, gp.targets.size());
            for (size_t ti = 0; ti < gp.targets.size(); ++ti) {
                MorphTarget mt;
                mt.name = names[ti];
                for (const auto& kv : gp.targets[ti]) {
                    if (kv.first == "POSITION") {
                        std::vector<float> f = readFloats(g, kv.second);
                        mt.dPos.resize(f.size() / 3);
                        for (size_t i = 0; i < mt.dPos.size(); ++i)
                            mt.dPos[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
                    } else if (kv.first == "NORMAL") {
                        std::vector<float> f = readFloats(g, kv.second);
                        mt.dNormal.resize(f.size() / 3);
                        for (size_t i = 0; i < mt.dNormal.size(); ++i)
                            mt.dNormal[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
                    }
                }
                prim.morphs.push_back(std::move(mt));
            }
            prim.blendedPos = prim.pos;
            prim.blendedNormal = prim.normal;
            prim.morphWeights.assign(prim.morphs.size(), 0.f);
            mesh.prims.push_back(std::move(prim));
        }
        out.meshes.push_back(std::move(mesh));
    }

    // ---- nodes ----
    out.nodes.resize(g.nodes.size());
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const tinygltf::Node& gn = g.nodes[i];
        Node& n = out.nodes[i];
        n.name = gn.name;
        n.mesh = gn.mesh;
        n.skin = gn.skin;
        for (int c : gn.children) {
            n.children.push_back(c);
            out.nodes[c].parent = static_cast<int>(i);
        }
        if (gn.matrix.size() == 16) {
            Mat4 m;
            for (int k = 0; k < 16; ++k) m.m[k] = static_cast<float>(gn.matrix[k]);
            decomposeMatrix(m, n.t, n.r, n.s);
        } else {
            if (gn.translation.size() == 3)
                n.t = {static_cast<float>(gn.translation[0]), static_cast<float>(gn.translation[1]),
                       static_cast<float>(gn.translation[2])};
            if (gn.rotation.size() == 4)
                n.r = Quat{static_cast<float>(gn.rotation[0]), static_cast<float>(gn.rotation[1]),
                           static_cast<float>(gn.rotation[2]), static_cast<float>(gn.rotation[3])};
            if (gn.scale.size() == 3)
                n.s = {static_cast<float>(gn.scale[0]), static_cast<float>(gn.scale[1]),
                       static_cast<float>(gn.scale[2])};
        }
    }

    // ---- skins ----
    for (const tinygltf::Skin& gs : g.skins) {
        Skin skin;
        skin.joints.assign(gs.joints.begin(), gs.joints.end());
        std::vector<float> f = readFloats(g, gs.inverseBindMatrices);
        size_t nj = skin.joints.size();
        skin.inverseBindMatrices.resize(nj);
        for (size_t i = 0; i < nj && i * 16 + 16 <= f.size(); ++i)
            for (int k = 0; k < 16; ++k)
                skin.inverseBindMatrices[i].m[k] = f[i * 16 + k];
        out.skins.push_back(std::move(skin));
    }

    // ---- scene roots & mesh instances ----
    int sceneIdx = g.defaultScene >= 0 ? g.defaultScene : 0;
    if (!g.scenes.empty())
        out.sceneRoots.assign(g.scenes[sceneIdx].nodes.begin(), g.scenes[sceneIdx].nodes.end());
    for (size_t i = 0; i < out.nodes.size(); ++i)
        if (out.nodes[i].mesh >= 0)
            out.meshInstances.push_back({static_cast<int>(i), out.nodes[i].mesh, out.nodes[i].skin});

    return true;
}

} // namespace ce
