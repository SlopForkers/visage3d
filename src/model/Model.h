#pragma once
// Internal, renderer-agnostic representation of a glTF/VRM character.
#include "core/Math3D.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ce {

struct MorphTarget {
    std::string name;
    std::vector<Vec3> dPos;    // position deltas (one per vertex)
    std::vector<Vec3> dNormal; // normal deltas (may be empty)
};

struct Primitive {
    // bind-pose data (immutable after load)
    std::vector<Vec3> pos;
    std::vector<Vec3> normal;
    std::vector<Vec2> uv;
    std::vector<uint16_t> joints; // 4 per vertex
    std::vector<Vec4> weights;    // 1 per vertex
    std::vector<uint32_t> indices;
    int material = -1;
    std::vector<MorphTarget> morphs;

    // CPU morph blend results (same size as pos/normal), uploaded to GPU when dirty
    std::vector<Vec3> blendedPos;
    std::vector<Vec3> blendedNormal;
    std::vector<float> morphWeights; // current weight per morph target
};

struct Mesh {
    std::string name;
    std::vector<Primitive> prims;
};

struct Material {
    std::string name;
    int texture = -1; // index into Model::textures
    Vec4 baseColor{1, 1, 1, 1};
    enum class Alpha { Opaque, Mask, Blend };
    Alpha alpha = Alpha::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
};

struct Texture {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // 4 bytes per pixel
};

struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    Vec3 t{0, 0, 0};
    Quat r{};
    Vec3 s{1, 1, 1};
    int mesh = -1;
    int skin = -1;
};

struct Skin {
    std::vector<int> joints;
    std::vector<Mat4> inverseBindMatrices;
};

struct MeshInstance {
    int node;  // node carrying the mesh (for naming/transforms)
    int mesh;
    int skin; // -1 => rigid (rare for characters)
};

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Node> nodes;
    std::vector<Skin> skins;
    std::vector<int> sceneRoots;
    std::vector<MeshInstance> meshInstances;
    std::string filePath;
    std::string fileName;

    int findNode(const std::string& nodeName) const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].name == nodeName) return static_cast<int>(i);
        return -1;
    }
};

} // namespace ce
