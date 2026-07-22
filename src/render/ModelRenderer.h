#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include "render/Camera.h"
#include "render/Shader.h"
#include <string>
#include <vector>

namespace ce {

// GPU representation + drawing of models.
// Supports multiple slots: slot 0 = body (skinned by the skeleton), other
// slots = clothing items (their vertices carry transferred weights and are
// skinned by the same skeleton via forceSkin).
// - skinning runs on the GPU (uBones array), normals use inverse-transpose in-shader
// - morph targets are blended on the CPU and re-uploaded when dirty
class ModelRenderer {
public:
    static constexpr int kMaxBones = 80;

    // Hairstyle controls (applied to materials with "hair" in the name).
    bool hideHair = false;
    Vec4 hairTint{1, 1, 1, 1};

    bool init(std::string& error);

    // Registers a model, uploads its meshes/textures, returns slot id.
    int addModel(Model& model, int forceSkin = -1);
    void removeModel(int slot);
    void setVisible(int slot, bool visible);
    // Re-points a slot at its model (the owner reallocated/moved it).
    void setModel(int slot, Model* model);
    void releaseAll(); // frees GPU resources of all slots

    // Re-blends morphs (if any) and re-uploads vertex data of a slot.
    void syncVertices(int slot);
    // Re-uploads uv/joints/weights of a slot (after weight transfer).
    void syncStatic(int slot);

    void draw(const Skeleton& skeleton, const Model& bodyModel, const Camera& camera,
              float aspect, bool wireframe);
    void drawGrid(const Camera& camera, float aspect);

    bool hasModel() const;

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
    struct GpuModelData {
        std::vector<GpuMesh> meshes;
        std::vector<unsigned> textures;
        Model* model = nullptr;
        bool visible = true;
        int forceSkin = -1; // >=0: draw all skinned prims with this body skin
    };

    Shader modelShader_, gridShader_;
    std::vector<GpuModelData> slots_;
    unsigned whiteTex_ = 0;
    unsigned gridVao_ = 0, gridVbo_ = 0;
    int gridVertexCount_ = 0;
    std::vector<Mat4> boneMats_;
    std::vector<float> stage_; // reused interleave buffer for vertex uploads

    void uploadVertices(GpuPrim& gpuPrim, const Primitive& prim); // pos+normal -> dynamic VBO
    void uploadStatic(GpuPrim& gpuPrim, const Primitive& prim);   // uv/joints/weights -> static VBO
    unsigned uploadTexture(const Texture& tex);
    void releaseSlot(GpuModelData& slot);
    void drawSlot(GpuModelData& slot, const Skeleton& skeleton, const Model& bodyModel, int pass);
    void drawPrim(const GpuPrim& gp, const GpuModelData& slot);
    static bool isHairMaterial(const std::string& name);
};

} // namespace ce
