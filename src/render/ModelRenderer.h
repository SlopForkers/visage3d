#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include "render/Camera.h"
#include "render/Shader.h"
#include <string>
#include <vector>

namespace ce {

// GPU representation + drawing of a Model.
// - skinning runs on the GPU (uBones array), normals use inverse-transpose in-shader
// - morph targets are blended on the CPU and re-uploaded when dirty
class ModelRenderer {
public:
    static constexpr int kMaxBones = 80;

    bool init(std::string& error);
    void upload(Model& model);         // (re)creates all GPU buffers
    void release();

    // Re-blends morph targets on CPU and re-uploads changed vertex data.
    void syncMorphs(Model& model);

    void draw(const Model& model, const Skeleton& skeleton, const Camera& camera,
              float aspect, bool wireframe);
    void drawGrid(const Camera& camera, float aspect);

    bool hasModel() const { return !meshes_.empty(); }

private:
    struct GpuPrim {
        unsigned vao = 0, vboStatic = 0, vboDyn = 0, ebo = 0;
        int indexCount = 0;
        int material = -1;
        size_t vertexCount = 0;
        bool skinned = false;
    };
    struct GpuMesh {
        std::vector<GpuPrim> prims;
        bool hasMorphs = false;
    };

    Shader modelShader_, gridShader_;
    std::vector<GpuMesh> meshes_;
    std::vector<unsigned> textures_;
    unsigned whiteTex_ = 0;
    unsigned gridVao_ = 0, gridVbo_ = 0;
    int gridVertexCount_ = 0;
    // flat index: for each mesh instance, list of (meshIdx) is in Model itself
    std::vector<Mat4> boneMats_;
    std::vector<Mat3> boneNrms_;

    void drawPrim(const GpuPrim& prim, const Model& model);
    void uploadVertices(GpuPrim& gpuPrim, const Primitive& prim); // pos+normal -> dynamic VBO
    unsigned uploadTexture(const Texture& tex);
};

} // namespace ce
