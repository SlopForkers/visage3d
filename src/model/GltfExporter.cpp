#include "model/GltfExporter.h"

#include "clothing/ClothingManager.h"
#include "model/Model.h"
#include "model/Skeleton.h"
#include "stb_image_write.h"
#include "tiny_gltf.h"

#include <cstring>

namespace ce {

namespace {

// Appends raw bytes to the single glTF buffer and returns the accessor index.
struct AccessorBuilder {
    tinygltf::Model& g;
    explicit AccessorBuilder(tinygltf::Model& model) : g(model) {}

    template <typename T>
    int add(const T* data, size_t count, int type, int compType, int target,
            const double* mn = nullptr, const double* mx = nullptr) {
        tinygltf::Buffer& buf = g.buffers[0];
        while (buf.data.size() % 4) buf.data.push_back(0); // 4-byte alignment
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = buf.data.size();
        bv.byteLength = count * sizeof(T);
        bv.target = target;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
        buf.data.insert(buf.data.end(), bytes, bytes + bv.byteLength);
        g.bufferViews.push_back(bv);

        tinygltf::Accessor acc;
        acc.bufferView = static_cast<int>(g.bufferViews.size()) - 1;
        acc.byteOffset = 0;
        acc.componentType = compType;
        acc.count = count;
        acc.type = type;
        if (mn && mx) {
            acc.minValues = {mn[0], mn[1], mn[2]};
            acc.maxValues = {mx[0], mx[1], mx[2]};
        }
        g.accessors.push_back(acc);
        return static_cast<int>(g.accessors.size()) - 1;
    }

    template <typename T>
    int addVec(const std::vector<T>& v, int type, int compType, int target) {
        return add(v.data(), v.size(), type, compType, target);
    }
};

void appendPngBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// u16 x4 element for JOINTS_0 (the builder needs the FULL element size)
struct U16Vec4 {
    uint16_t x, y, z, w;
};

// Adds a material (+its texture image) to the glTF model; returns the index.
int addMaterial(tinygltf::Model& g, const Material& m,
                const std::vector<Texture>& textures,
                const std::vector<int>& textureMap) {
    tinygltf::Material mat;
    mat.name = m.name;
    mat.pbrMetallicRoughness.baseColorFactor = {m.baseColor.x, m.baseColor.y, m.baseColor.z,
                                                m.baseColor.w};
    mat.pbrMetallicRoughness.metallicFactor = 0.0;
    mat.pbrMetallicRoughness.roughnessFactor = 0.9;
    if (m.texture >= 0 && m.texture < static_cast<int>(textureMap.size()) &&
        textureMap[m.texture] >= 0) {
        mat.pbrMetallicRoughness.baseColorTexture.index = textureMap[m.texture];
        mat.pbrMetallicRoughness.baseColorTexture.texCoord = 0;
    }
    switch (m.alpha) {
    case Material::Alpha::Mask:
        mat.alphaMode = "MASK";
        mat.alphaCutoff = m.alphaCutoff;
        break;
    case Material::Alpha::Blend: mat.alphaMode = "BLEND"; break;
    default: mat.alphaMode = "OPAQUE"; break;
    }
    mat.doubleSided = m.doubleSided;
    if (m.unlit) {
        tinygltf::Value::Object empty;
        mat.extensions["KHR_materials_unlit"] = tinygltf::Value(empty);
    }
    g.materials.push_back(std::move(mat));
    return static_cast<int>(g.materials.size()) - 1;
}

// Adds all textures of a model as PNG-embedded images; returns source->gltf map.
std::vector<int> addTextures(tinygltf::Model& g, const std::vector<Texture>& textures) {
    std::vector<int> map(textures.size(), -1);
    for (size_t i = 0; i < textures.size(); ++i) {
        const Texture& t = textures[i];
        if (t.width <= 0 || t.height <= 0 || t.rgba.empty()) continue;
        std::vector<unsigned char> png;
        if (!stbi_write_png_to_func(appendPngBytes, &png, t.width, t.height, 4,
                                    t.rgba.data(), t.width * 4) ||
            png.empty())
            continue;
        tinygltf::Buffer& buf = g.buffers[0];
        while (buf.data.size() % 4) buf.data.push_back(0);
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = buf.data.size();
        bv.byteLength = png.size();
        bv.target = 0;
        buf.data.insert(buf.data.end(), png.begin(), png.end());
        g.bufferViews.push_back(bv);

        tinygltf::Image img;
        img.name = "tex" + std::to_string(i);
        img.mimeType = "image/png";
        img.bufferView = static_cast<int>(g.bufferViews.size()) - 1;
        g.images.push_back(std::move(img));

        tinygltf::Texture tex;
        tex.source = static_cast<int>(g.images.size()) - 1;
        tex.sampler = 0;
        g.textures.push_back(std::move(tex));
        map[i] = static_cast<int>(g.textures.size()) - 1;
    }
    return map;
}

// Adds one primitive (positions/normals/uv/joints/weights/indices [+ morphs]).
void addPrimitive(tinygltf::Model& g, AccessorBuilder& ab, tinygltf::Mesh& out,
                  const Primitive& prim, int materialIndex, bool withMorphs) {
    const std::vector<Vec3>& pos = prim.blendedPos.size() == prim.pos.size()
                                       ? prim.blendedPos
                                       : prim.pos;
    const std::vector<Vec3>& nrm = prim.blendedNormal.size() == prim.normal.size()
                                       ? prim.blendedNormal
                                       : prim.normal;
    if (pos.empty() || prim.indices.empty()) return;

    tinygltf::Primitive gp;
    gp.mode = 4; // TRIANGLES
    if (materialIndex >= 0) gp.material = materialIndex;

    double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
    for (const Vec3& v : pos)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], static_cast<double>((&v.x)[k]));
            mx[k] = std::max(mx[k], static_cast<double>((&v.x)[k]));
        }
    gp.attributes["POSITION"] =
        ab.add(pos.data(), pos.size(), TINYGLTF_TYPE_VEC3,
               TINYGLTF_COMPONENT_TYPE_FLOAT, 34962, mn, mx);
    if (nrm.size() == pos.size())
        gp.attributes["NORMAL"] = ab.addVec(nrm, TINYGLTF_TYPE_VEC3,
                                            TINYGLTF_COMPONENT_TYPE_FLOAT, 34962);
    if (prim.uv.size() == pos.size())
        gp.attributes["TEXCOORD_0"] = ab.addVec(prim.uv, TINYGLTF_TYPE_VEC2,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT, 34962);
    if (prim.joints.size() == pos.size() * 4 && prim.weights.size() == pos.size()) {
        gp.attributes["JOINTS_0"] =
            ab.add(reinterpret_cast<const U16Vec4*>(prim.joints.data()), pos.size(),
                   TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, 34962);
        gp.attributes["WEIGHTS_0"] = ab.addVec(prim.weights, TINYGLTF_TYPE_VEC4,
                                               TINYGLTF_COMPONENT_TYPE_FLOAT, 34962);
    }
    gp.indices = ab.addVec(prim.indices, TINYGLTF_TYPE_SCALAR,
                           TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, 34963);

    // morph targets: deltas are absolute offsets — they stay valid on the
    // baked base; weights are zero in the export
    if (withMorphs) {
        for (const MorphTarget& mt : prim.morphs) {
            if (mt.dPos.size() != pos.size()) continue;
            std::map<std::string, int> target;
            target["POSITION"] = ab.addVec(mt.dPos, TINYGLTF_TYPE_VEC3,
                                           TINYGLTF_COMPONENT_TYPE_FLOAT, 34962);
            if (mt.dNormal.size() == pos.size())
                target["NORMAL"] = ab.addVec(mt.dNormal, TINYGLTF_TYPE_VEC3,
                                             TINYGLTF_COMPONENT_TYPE_FLOAT, 34962);
            gp.targets.push_back(std::move(target));
        }
    }
    out.primitives.push_back(std::move(gp));
}

} // namespace

bool exportGlb(const std::string& path, const Model& body, const Skeleton& skeleton,
               const std::vector<ClothingItem>& clothing, int bodySkinIndex,
               std::string& err) {
    if (body.nodes.empty() || body.meshes.empty()) {
        err = "empty model";
        return false;
    }
    if (bodySkinIndex < 0 || bodySkinIndex >= static_cast<int>(body.skins.size()))
        bodySkinIndex = body.meshInstances.empty() ? -1 : body.meshInstances[0].skin;
    if (bodySkinIndex < 0) {
        err = "no skin";
        return false;
    }

    tinygltf::Model g;
    g.asset.version = "2.0";
    g.asset.generator = "character_editor";
    g.buffers.emplace_back();
    AccessorBuilder ab(g);

    // one shared sampler (linear filtering, repeat)
    g.samplers.emplace_back();

    // ---- nodes: body hierarchy with baked shape offsets ----
    for (size_t i = 0; i < body.nodes.size(); ++i) {
        const Node& n = body.nodes[i];
        Vec3 t = n.t, s = n.s;
        if (i < skeleton.translateOffset.size()) t = t + skeleton.translateOffset[i];
        if (i < skeleton.scaleOffset.size()) s = s * skeleton.scaleOffset[i];
        tinygltf::Node gn;
        gn.name = n.name;
        gn.translation = {t.x, t.y, t.z};
        gn.rotation = {n.r.x, n.r.y, n.r.z, n.r.w};
        gn.scale = {s.x, s.y, s.z};
        gn.children = n.children;
        g.nodes.push_back(std::move(gn));
    }

    // ---- skins ----
    for (const Skin& skin : body.skins) {
        tinygltf::Skin gs;
        gs.joints = skin.joints;
        if (!skin.inverseBindMatrices.empty())
            gs.inverseBindMatrices =
                ab.add(skin.inverseBindMatrices.data(), skin.inverseBindMatrices.size(),
                       TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT, 0);
        g.skins.push_back(std::move(gs));
    }

    // ---- body materials + textures ----
    std::vector<int> bodyTexMap = addTextures(g, body.textures);
    std::vector<int> bodyMatMap(body.materials.size(), -1);
    for (size_t i = 0; i < body.materials.size(); ++i)
        bodyMatMap[i] = addMaterial(g, body.materials[i], body.textures, bodyTexMap);

    // ---- body meshes (morphs + effectors baked into positions) ----
    std::vector<std::vector<std::string>> targetNames(body.meshes.size());
    for (size_t mi = 0; mi < body.meshes.size(); ++mi) {
        const Mesh& mesh = body.meshes[mi];
        if (!mesh.prims.empty())
            for (const MorphTarget& mt : mesh.prims.front().morphs)
                targetNames[mi].push_back(mt.name);
    }
    for (size_t mi = 0; mi < body.meshes.size(); ++mi) {
        const Mesh& mesh = body.meshes[mi];
        tinygltf::Mesh gm;
        gm.name = mesh.name;
        for (const Primitive& prim : mesh.prims) {
            int mat = (prim.material >= 0 &&
                       prim.material < static_cast<int>(bodyMatMap.size()))
                          ? bodyMatMap[prim.material]
                          : -1;
            addPrimitive(g, ab, gm, prim, mat, true);
        }
        if (!targetNames[mi].empty()) {
            tinygltf::Value::Array names;
            for (const std::string& n : targetNames[mi]) names.push_back(tinygltf::Value(n));
            tinygltf::Value::Object extras;
            extras["targetNames"] = tinygltf::Value(names);
            gm.extras = tinygltf::Value(extras);
        }
        if (!gm.primitives.empty()) {
            if (!targetNames[mi].empty())
                gm.weights.assign(targetNames[mi].size(), 0.0);
            g.meshes.push_back(std::move(gm));
            // wire the mesh instance into its node
            for (const MeshInstance& inst : body.meshInstances)
                if (inst.mesh == static_cast<int>(mi)) {
                    g.nodes[inst.node].mesh = static_cast<int>(g.meshes.size()) - 1;
                    g.nodes[inst.node].skin = inst.skin;
                }
        }
    }

    // ---- clothing: extra meshes skinned to the body skeleton ----
    std::vector<int> clothNodes;
    for (const ClothingItem& item : clothing) {
        if (!item.visible) continue;
        std::vector<int> texMap = addTextures(g, item.model.textures);
        std::vector<int> matMap(item.model.materials.size(), -1);
        for (size_t i = 0; i < item.model.materials.size(); ++i)
            matMap[i] = addMaterial(g, item.model.materials[i], item.model.textures, texMap);
        for (size_t mi = 0; mi < item.model.meshes.size(); ++mi) {
            const Mesh& mesh = item.model.meshes[mi];
            tinygltf::Mesh gm;
            gm.name = item.name;
            for (const Primitive& prim : mesh.prims) {
                int mat = (prim.material >= 0 &&
                           prim.material < static_cast<int>(matMap.size()))
                              ? matMap[prim.material]
                              : -1;
                addPrimitive(g, ab, gm, prim, mat, false);
            }
            if (gm.primitives.empty()) continue;
            g.meshes.push_back(std::move(gm));

            // garment vertices are in the body's bind world space: identity
            // node transform, skinned by the body skin
            tinygltf::Node gn;
            gn.name = item.name;
            gn.mesh = static_cast<int>(g.meshes.size()) - 1;
            gn.skin = bodySkinIndex;
            gn.translation = {0, 0, 0};
            gn.rotation = {0, 0, 0, 1};
            gn.scale = {1, 1, 1};
            g.nodes.push_back(std::move(gn));
            clothNodes.push_back(static_cast<int>(g.nodes.size()) - 1);
        }
    }

    // ---- scene ----
    tinygltf::Scene scene;
    scene.name = "Scene";
    for (int root : body.sceneRoots) scene.nodes.push_back(root);
    for (int cn : clothNodes) scene.nodes.push_back(cn);
    g.scenes.push_back(std::move(scene));
    g.defaultScene = 0;

    // extensionsUsed
    for (const tinygltf::Material& m : g.materials)
        if (m.extensions.count("KHR_materials_unlit")) {
            g.extensionsUsed.push_back("KHR_materials_unlit");
            break;
        }

    tinygltf::TinyGLTF writer;
    if (!writer.WriteGltfSceneToFile(&g, path, true, true, false, true)) {
        err = "write failed: " + path;
        return false;
    }
    std::fprintf(stderr, "Exported %s (%zu nodes, %zu meshes, %zu materials)\n", path.c_str(),
                 g.nodes.size(), g.meshes.size(), g.materials.size());
    return true;
}

} // namespace ce
