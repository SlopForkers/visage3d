#include "render/ModelRenderer.h"
#include "core/GL.h"
#include <algorithm>
#include <cstdio>

namespace ce {

namespace {

const char* kModelVS = R"glsl(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aJoints;
layout(location=4) in vec4 aWeights;

uniform mat4 uVP;
uniform mat4 uNodeMat;
uniform int uUseSkin;
uniform mat4 uBones[80];

out vec2 vUV;
out vec3 vNrm;
out vec3 vPos;

void main() {
    mat4 skin = uNodeMat;
    if (uUseSkin == 1) {
        skin = aWeights.x * uBones[int(aJoints.x)]
             + aWeights.y * uBones[int(aJoints.y)]
             + aWeights.z * uBones[int(aJoints.z)]
             + aWeights.w * uBones[int(aJoints.w)];
    }
    vec4 wp = skin * vec4(aPos, 1.0);
    mat3 nrmMat = transpose(inverse(mat3(skin)));
    vNrm = nrmMat * aNrm;
    vPos = wp.xyz;
    vUV = aUV;
    gl_Position = uVP * wp;
}
)glsl";

const char* kModelFS = R"glsl(#version 330 core
in vec2 vUV;
in vec3 vNrm;
in vec3 vPos;
out vec4 fragColor;

uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uBaseColor;
uniform int uAlphaMode; // 0 opaque, 1 mask, 2 blend
uniform float uCutoff;
uniform int uUnlit;
uniform int uToon;
uniform vec3 uLightDir;
uniform vec3 uCamPos;
uniform vec3 uAreolaPos[2];
uniform float uAreolaRadius;
uniform vec3 uAreolaColor;
uniform float uAreolaStrength;

void main() {
    vec4 c = uBaseColor;
    if (uHasTex == 1) c *= texture(uTex, vUV);
    if (uAlphaMode == 1 && c.a < uCutoff) discard;

    vec3 col = c.rgb;
    // areola tint: soft radial darkening around the anchor (body slot only)
    if (uAreolaStrength > 0.001 && uAreolaRadius > 0.001) {
        for (int i = 0; i < 2; ++i) {
            float d = distance(vPos, uAreolaPos[i]);
            float t = (1.0 - smoothstep(uAreolaRadius * 0.5, uAreolaRadius, d)) * uAreolaStrength;
            col = mix(col, col * uAreolaColor, t);
        }
    }
    if (uUnlit == 0) {
        vec3 N = normalize(vNrm);
        if (!gl_FrontFacing) N = -N;
        vec3 L = normalize(uLightDir);
        vec3 V = normalize(uCamPos - vPos);
        float ndl = dot(N, L) * 0.5 + 0.5;
        if (uToon == 1) {
            // cel shading: two soft-quantized bands; shadows go cool (anime
            // shade tint) instead of just darker
            float w = fwidth(ndl) * 1.5 + 1e-4;
            float s = smoothstep(0.42 - w, 0.42 + w, ndl) * 0.55
                    + smoothstep(0.72 - w, 0.72 + w, ndl) * 0.45;
            vec3 shadeCol = col * vec3(0.66, 0.64, 0.78);
            col = mix(shadeCol, col, clamp(s, 0.0, 1.0));
            // rim light on the lit side
            float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.5);
            rim *= smoothstep(0.30, 0.70, ndl);
            col += vec3(1.0, 0.98, 0.94) * rim * 0.20;
        } else {
            float wrap = ndl;
            vec3 H = normalize(L + V);
            float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.08;
            col = col * (0.35 + 0.65 * wrap) + vec3(spec);
        }
    }
    fragColor = vec4(pow(max(col, 0.0), vec3(1.0 / 2.2)), c.a);
}
)glsl";

// Inverted-hull outline: same skinning, then the vertex is pushed along its
// (projected) normal by a constant screen-space amount.
const char* kOutlineVS = R"glsl(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aJoints;
layout(location=4) in vec4 aWeights;

uniform mat4 uVP;
uniform mat4 uNodeMat;
uniform int uUseSkin;
uniform mat4 uBones[80];
uniform vec2 uWinSize;
uniform float uOutlineWidth; // pixels

out vec2 vUV;

void main() {
    mat4 skin = uNodeMat;
    if (uUseSkin == 1) {
        skin = aWeights.x * uBones[int(aJoints.x)]
             + aWeights.y * uBones[int(aJoints.y)]
             + aWeights.z * uBones[int(aJoints.z)]
             + aWeights.w * uBones[int(aJoints.w)];
    }
    vec4 wp = skin * vec4(aPos, 1.0);
    mat3 nrmMat = transpose(inverse(mat3(skin)));
    vec3 n = nrmMat * aNrm;
    vec4 cp = uVP * wp;
    vec3 cn = (uVP * vec4(n, 0.0)).xyz;
    vec2 dir = cn.xy;
    float l = length(dir);
    if (l > 1e-6) {
        // px -> NDC differs per axis (width vs height of the viewport)
        cp.xy += (dir / l) * uOutlineWidth * 2.0 / uWinSize * cp.w;
    }
    gl_Position = cp;
    vUV = aUV;
}
)glsl";

const char* kOutlineFS = R"glsl(#version 330 core
in vec2 vUV;
out vec4 fragColor;

uniform sampler2D uTex;
uniform int uHasTex;
uniform vec4 uBaseColor;
uniform int uAlphaMode; // only MASK matters here (blend prims are skipped)
uniform float uCutoff;
uniform vec3 uOutlineColor;

void main() {
    vec4 c = uBaseColor;
    if (uHasTex == 1) c *= texture(uTex, vUV);
    if (uAlphaMode == 1 && c.a < uCutoff) discard;
    // outline tinted by albedo (MToon-style), not pure black
    vec3 col = uOutlineColor * c.rgb;
    fragColor = vec4(pow(max(col, 0.0), vec3(1.0 / 2.2)), 1.0);
}
)glsl";

const char* kGridVS = R"glsl(#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uVP;
out float vFade;
void main() {
    vFade = 1.0 - clamp(length(aPos.xz) / 2.2, 0.0, 1.0);
    gl_Position = uVP * vec4(aPos, 1.0);
}
)glsl";

const char* kGridFS = R"glsl(#version 330 core
in float vFade;
uniform vec3 uColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(uColor * vFade, 1.0);
}
)glsl";

} // namespace

bool ModelRenderer::init(std::string& error) {
    if (!modelShader_.compile(kModelVS, kModelFS, error)) return false;
    if (!gridShader_.compile(kGridVS, kGridFS, error)) return false;
    if (!outlineShader_.compile(kOutlineVS, kOutlineFS, error)) return false;

    // 1x1 white fallback texture
    glGenTextures(1, &whiteTex_);
    glBindTexture(GL_TEXTURE_2D, whiteTex_);
    unsigned char white[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // grid: lines on XZ plane, every 0.1m within [-2, 2]
    std::vector<Vec3> lines;
    for (int i = -20; i <= 20; ++i) {
        float p = i * 0.1f;
        lines.push_back({p, 0, -2.f});
        lines.push_back({p, 0, 2.f});
        lines.push_back({-2.f, 0, p});
        lines.push_back({2.f, 0, p});
    }
    gridVertexCount_ = static_cast<int>(lines.size());
    glGenVertexArrays(1, &gridVao_);
    glBindVertexArray(gridVao_);
    glGenBuffers(1, &gridVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, lines.size() * sizeof(Vec3), lines.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), nullptr);
    glBindVertexArray(0);
    return true;
}

unsigned ModelRenderer::uploadTexture(const Texture& tex) {
    if (tex.width <= 0 || tex.rgba.empty()) return whiteTex_;
    unsigned id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, tex.width, tex.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, tex.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return id;
}

void ModelRenderer::uploadVertices(GpuPrim& gp, const Primitive& prim) {
    // reuse the staging buffer: per-frame uploads (gizmo drags, morphs) must
    // not allocate several MB every call
    if (stage_.size() < gp.vertexCount * 6) stage_.resize(gp.vertexCount * 6);
    float* buf = stage_.data();
    const std::vector<Vec3>& pos = prim.blendedPos.empty() ? prim.pos : prim.blendedPos;
    const std::vector<Vec3>& nrm = prim.blendedNormal.empty() ? prim.normal : prim.blendedNormal;
    const bool hasNrm = nrm.size() == pos.size();
    for (size_t v = 0; v < gp.vertexCount; ++v) {
        buf[v * 6 + 0] = pos[v].x;
        buf[v * 6 + 1] = pos[v].y;
        buf[v * 6 + 2] = pos[v].z;
        if (hasNrm) {
            buf[v * 6 + 3] = nrm[v].x;
            buf[v * 6 + 4] = nrm[v].y;
            buf[v * 6 + 5] = nrm[v].z;
        } else {
            buf[v * 6 + 3] = 0.f;
            buf[v * 6 + 4] = 1.f;
            buf[v * 6 + 5] = 0.f;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, gp.vboDyn);
    glBufferSubData(GL_ARRAY_BUFFER, 0, gp.vertexCount * 6 * sizeof(float), buf);
}

namespace {
struct StaticVert {
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};
} // namespace

void ModelRenderer::uploadStatic(GpuPrim& gp, const Primitive& prim) {
    std::vector<StaticVert> sv(gp.vertexCount);
    bool skinned = !prim.joints.empty() && !prim.weights.empty();
    for (size_t i = 0; i < gp.vertexCount; ++i) {
        if (i < prim.uv.size()) { sv[i].uv[0] = prim.uv[i].x; sv[i].uv[1] = prim.uv[i].y; }
        if (skinned) {
            for (int k = 0; k < 4; ++k) {
                sv[i].joints[k] = prim.joints[i * 4 + k];
                const float* w = &prim.weights[i].x;
                sv[i].weights[k] = w[k];
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, gp.vboStatic);
    glBufferData(GL_ARRAY_BUFFER, sv.size() * sizeof(StaticVert), sv.data(), GL_STATIC_DRAW);
}

void ModelRenderer::syncStatic(int slotId) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    GpuModelData& slot = slots_[slotId];
    if (!slot.model) return;
    for (size_t mi = 0; mi < slot.model->meshes.size() && mi < slot.meshes.size(); ++mi)
        for (size_t pi = 0; pi < slot.model->meshes[mi].prims.size(); ++pi) {
            Primitive& prim = slot.model->meshes[mi].prims[pi];
            slot.meshes[mi].prims[pi].skinned = !prim.joints.empty() && !prim.weights.empty();
            uploadStatic(slot.meshes[mi].prims[pi], prim);
        }
}

int ModelRenderer::addModel(Model& model, int forceSkin) {
    GpuModelData slot;
    slot.model = &model;
    slot.forceSkin = forceSkin;

    slot.textures.reserve(model.textures.size());
    for (const Texture& t : model.textures) slot.textures.push_back(uploadTexture(t));

    slot.meshes.resize(model.meshes.size());
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const Mesh& mesh = model.meshes[mi];
        GpuMesh& gm = slot.meshes[mi];
        gm.hasMorphs = !mesh.prims.empty() && !mesh.prims.front().morphs.empty();

        for (const Primitive& prim : mesh.prims) {
            GpuPrim gp;
            gp.material = prim.material;
            gp.indexCount = static_cast<int>(prim.indices.size());
            gp.vertexCount = prim.pos.size();
            gp.skinned = !prim.joints.empty() && !prim.weights.empty();

            glGenVertexArrays(1, &gp.vao);
            glBindVertexArray(gp.vao);

            // dynamic buffer: pos(3f) + normal(3f) interleaved
            glGenBuffers(1, &gp.vboDyn);
            glBindBuffer(GL_ARRAY_BUFFER, gp.vboDyn);
            glBufferData(GL_ARRAY_BUFFER, gp.vertexCount * 6 * sizeof(float), nullptr,
                         GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                                  reinterpret_cast<void*>(3 * sizeof(float)));

            // static buffer: uv(2f) | joints(4 u16) | weights(4f)
            glGenBuffers(1, &gp.vboStatic);
            glBindBuffer(GL_ARRAY_BUFFER, gp.vboStatic);
            uploadStatic(gp, prim);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(StaticVert), nullptr);
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(3, 4, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(StaticVert),
                                  reinterpret_cast<void*>(offsetof(StaticVert, joints)));
            glEnableVertexAttribArray(4);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(StaticVert),
                                  reinterpret_cast<void*>(offsetof(StaticVert, weights)));

            glGenBuffers(1, &gp.ebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gp.ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, prim.indices.size() * sizeof(uint32_t),
                         prim.indices.data(), GL_STATIC_DRAW);

            glBindVertexArray(0);
            gm.prims.push_back(gp);
        }

        // fill dynamic vertex buffers for every prim (morph or not)
        for (size_t pi = 0; pi < mesh.prims.size(); ++pi)
            uploadVertices(gm.prims[pi], mesh.prims[pi]);
    }

    // reuse an empty slot if any
    for (size_t i = 0; i < slots_.size(); ++i)
        if (!slots_[i].model) {
            slots_[i] = std::move(slot);
            return static_cast<int>(i);
        }
    slots_.push_back(std::move(slot));
    return static_cast<int>(slots_.size()) - 1;
}

void ModelRenderer::syncVertices(int slotId) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    GpuModelData& slot = slots_[slotId];
    if (!slot.model) return;
    Model& model = *slot.model;

    for (size_t mi = 0; mi < model.meshes.size() && mi < slot.meshes.size(); ++mi) {
        GpuMesh& gm = slot.meshes[mi];
        Mesh& mesh = model.meshes[mi];
        if (gm.hasMorphs) {
            // CPU blend: base + sum(weight * delta)
            for (Primitive& prim : mesh.prims) {
                prim.blendedPos = prim.pos;
                prim.blendedNormal = prim.normal;
                for (size_t ti = 0; ti < prim.morphs.size(); ++ti) {
                    float w = (ti < prim.morphWeights.size()) ? prim.morphWeights[ti] : 0.f;
                    if (w == 0.f) continue;
                    const MorphTarget& mt = prim.morphs[ti];
                    for (size_t v = 0; v < prim.blendedPos.size() && v < mt.dPos.size(); ++v)
                        prim.blendedPos[v] += mt.dPos[v] * w;
                    if (!mt.dNormal.empty())
                        for (size_t v = 0; v < prim.blendedNormal.size() && v < mt.dNormal.size(); ++v)
                            prim.blendedNormal[v] += mt.dNormal[v] * w;
                }
            }
        }
        for (size_t pi = 0; pi < mesh.prims.size(); ++pi)
            uploadVertices(gm.prims[pi], mesh.prims[pi]);
    }
}

void ModelRenderer::reupload(int slotId) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    GpuModelData& slot = slots_[slotId];
    if (!slot.model) return;
    Model& model = *slot.model;
    for (size_t mi = 0; mi < model.meshes.size() && mi < slot.meshes.size(); ++mi)
        for (size_t pi = 0; pi < model.meshes[mi].prims.size(); ++pi)
            uploadVertices(slot.meshes[mi].prims[pi], model.meshes[mi].prims[pi]);
}

void ModelRenderer::releaseSlot(GpuModelData& slot) {
    for (GpuMesh& gm : slot.meshes)
        for (GpuPrim& gp : gm.prims) {
            if (gp.vao) glDeleteVertexArrays(1, &gp.vao);
            if (gp.vboStatic) glDeleteBuffers(1, &gp.vboStatic);
            if (gp.vboDyn) glDeleteBuffers(1, &gp.vboDyn);
            if (gp.ebo) glDeleteBuffers(1, &gp.ebo);
        }
    slot.meshes.clear();
    for (unsigned t : slot.textures)
        if (t && t != whiteTex_) glDeleteTextures(1, &t);
    slot.textures.clear();
    slot.model = nullptr;
}

void ModelRenderer::removeModel(int slotId) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    releaseSlot(slots_[slotId]);
}

void ModelRenderer::releaseAll() {
    for (GpuModelData& slot : slots_) releaseSlot(slot);
    slots_.clear();
}

void ModelRenderer::setVisible(int slotId, bool visible) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    slots_[slotId].visible = visible;
}

void ModelRenderer::setModel(int slotId, Model* model) {
    if (slotId < 0 || slotId >= static_cast<int>(slots_.size())) return;
    slots_[slotId].model = model;
}

bool ModelRenderer::hasModel() const {
    for (const auto& s : slots_)
        if (s.model && s.visible) return true;
    return false;
}

bool ModelRenderer::isHairMaterial(const std::string& name) {
    return name.find("HAIR") != std::string::npos || name.find("Hair") != std::string::npos ||
           name.find("hair") != std::string::npos;
}

void ModelRenderer::drawPrim(const GpuPrim& gp, const GpuModelData& slot) {
    const Model& model = *slot.model;
    const Material* mat = nullptr;
    if (gp.material >= 0 && gp.material < static_cast<int>(model.materials.size()))
        mat = &model.materials[gp.material];

    int alphaMode = 0;
    float cutoff = 0.5f;
    Vec4 base{1, 1, 1, 1};
    int unlit = 0;
    unsigned tex = whiteTex_;
    bool doubleSided = false;
    if (mat) {
        alphaMode = mat->alpha == Material::Alpha::Mask ? 1 : (mat->alpha == Material::Alpha::Blend ? 2 : 0);
        cutoff = mat->alphaCutoff;
        base = mat->baseColor;
        unlit = mat->unlit ? 1 : 0;
        if (mat->texture >= 0 && mat->texture < static_cast<int>(slot.textures.size()))
            tex = slot.textures[mat->texture];
        doubleSided = mat->doubleSided;
        if (isHairMaterial(mat->name)) base = base * hairTint;
    }

    if (doubleSided) glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE);

    modelShader_.setInt("uAlphaMode", alphaMode);
    modelShader_.setFloat("uCutoff", cutoff);
    modelShader_.setVec4("uBaseColor", base);
    modelShader_.setInt("uUnlit", unlit);
    modelShader_.setInt("uHasTex", 1);
    modelShader_.setInt("uTex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(gp.vao);
    glDrawElements(GL_TRIANGLES, gp.indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void ModelRenderer::setSkinUniforms(Shader& sh, const Skeleton& skeleton,
                                    const Model& slotModel, const Model& skinModel,
                                    int forceSkin, const MeshInstance& inst, bool anySkinned) {
    (void)slotModel;
    int skinToUse = (forceSkin >= 0) ? forceSkin : inst.skin;
    sh.setInt("uUseSkin", 0);
    sh.setMat4("uNodeMat", forceSkin >= 0 ? Mat4::identity() : skeleton.world()[inst.node]);
    if (anySkinned && skinToUse >= 0 && skinToUse < static_cast<int>(skinModel.skins.size())) {
        const Skin& skin = skinModel.skins[skinToUse];
        int nb = std::min(static_cast<int>(skin.joints.size()), kMaxBones);
        if (static_cast<int>(boneMats_.size()) < nb) boneMats_.resize(nb);
        for (int i = 0; i < nb; ++i)
            boneMats_[i] = skeleton.world()[skin.joints[i]] * skin.inverseBindMatrices[i];
        sh.setInt("uUseSkin", 1);
        glUniformMatrix4fv(sh.location("uBones"), nb, GL_FALSE, boneMats_[0].m);
    }
}

void ModelRenderer::drawSlot(GpuModelData& slot, const Skeleton& skeleton,
                              const Model& bodyModel, int pass) {
    const Model& model = *slot.model;
    // Clothing slots: vertices are in body bind space and index the body skin.
    const Model& skinModel = (slot.forceSkin >= 0) ? bodyModel : model;

    // areola tint applies to bare body only, never to clothing
    modelShader_.setFloat("uAreolaStrength", slot.forceSkin >= 0 ? 0.f : areolaStrength);

    for (const MeshInstance& inst : model.meshInstances) {
        if (inst.mesh < 0 || inst.mesh >= static_cast<int>(slot.meshes.size())) continue;
        const GpuMesh& gm = slot.meshes[inst.mesh];

        bool anySkinned = false;
        for (const GpuPrim& gp : gm.prims)
            if (gp.skinned) { anySkinned = true; break; }
        setSkinUniforms(modelShader_, skeleton, model, skinModel, slot.forceSkin, inst,
                        anySkinned);

        for (const GpuPrim& gp : gm.prims) {
            const Material* mat = nullptr;
            if (gp.material >= 0 && gp.material < static_cast<int>(model.materials.size()))
                mat = &model.materials[gp.material];
            bool blended = mat && mat->alpha == Material::Alpha::Blend;
            if ((pass == 1) != blended) continue;
            if (hideHair && mat && isHairMaterial(mat->name)) continue;
            drawPrim(gp, slot);
        }
    }
}

void ModelRenderer::drawPrimOutline(const GpuPrim& gp, const GpuModelData& slot) {
    const Model& model = *slot.model;
    const Material* mat = nullptr;
    if (gp.material >= 0 && gp.material < static_cast<int>(model.materials.size()))
        mat = &model.materials[gp.material];

    int alphaMode = 0;
    float cutoff = 0.5f;
    Vec4 base{1, 1, 1, 1};
    unsigned tex = whiteTex_;
    if (mat) {
        alphaMode = mat->alpha == Material::Alpha::Mask ? 1 : 0;
        cutoff = mat->alphaCutoff;
        base = mat->baseColor;
        if (mat->texture >= 0 && mat->texture < static_cast<int>(slot.textures.size()))
            tex = slot.textures[mat->texture];
    }
    outlineShader_.setInt("uAlphaMode", alphaMode);
    outlineShader_.setFloat("uCutoff", cutoff);
    outlineShader_.setVec4("uBaseColor", base);
    outlineShader_.setInt("uHasTex", 1);
    outlineShader_.setInt("uTex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(gp.vao);
    glDrawElements(GL_TRIANGLES, gp.indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void ModelRenderer::drawSlotOutline(GpuModelData& slot, const Skeleton& skeleton,
                                    const Model& bodyModel) {
    const Model& model = *slot.model;
    const Model& skinModel = (slot.forceSkin >= 0) ? bodyModel : model;

    for (const MeshInstance& inst : model.meshInstances) {
        if (inst.mesh < 0 || inst.mesh >= static_cast<int>(slot.meshes.size())) continue;
        const GpuMesh& gm = slot.meshes[inst.mesh];

        bool anySkinned = false;
        for (const GpuPrim& gp : gm.prims)
            if (gp.skinned) { anySkinned = true; break; }
        setSkinUniforms(outlineShader_, skeleton, model, skinModel, slot.forceSkin, inst,
                        anySkinned);

        for (const GpuPrim& gp : gm.prims) {
            const Material* mat = nullptr;
            if (gp.material >= 0 && gp.material < static_cast<int>(model.materials.size()))
                mat = &model.materials[gp.material];
            if (mat && mat->alpha == Material::Alpha::Blend) continue; // no outline on glass
            if (hideHair && mat && isHairMaterial(mat->name)) continue;
            drawPrimOutline(gp, slot);
        }
    }
}

void ModelRenderer::draw(const Skeleton& skeleton, const Model& bodyModel, const Camera& camera,
                          float aspect, bool wireframe, int winW, int winH) {
    Mat4 vp = camera.projection(aspect) * camera.view();
    glEnable(GL_DEPTH_TEST);

    // ---- inverted-hull outline (back faces pushed along the normal) ----
    if (outlineEnabled && !wireframe) {
        outlineShader_.use();
        outlineShader_.setMat4("uVP", vp);
        outlineShader_.setVec2("uWinSize", Vec2{static_cast<float>(winW),
                                                static_cast<float>(winH)});
        outlineShader_.setFloat("uOutlineWidth", outlineWidth);
        outlineShader_.setVec3("uOutlineColor", outlineColor);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        for (GpuModelData& slot : slots_) {
            if (!slot.model || !slot.visible) continue;
            drawSlotOutline(slot, skeleton, bodyModel);
        }
        glCullFace(GL_BACK);
    }

    modelShader_.use();
    modelShader_.setMat4("uVP", vp);
    modelShader_.setVec3("uLightDir", Vec3{0.5f, 0.9f, 0.7f}.normalized());
    modelShader_.setVec3("uCamPos", camera.eye());
    modelShader_.setInt("uToon", toonShading ? 1 : 0);
    modelShader_.setVec3("uAreolaPos[0]", areolaAnchors[0]);
    modelShader_.setVec3("uAreolaPos[1]", areolaAnchors[1]);
    modelShader_.setFloat("uAreolaRadius", areolaRadius);
    modelShader_.setVec3("uAreolaColor", areolaColor);

    if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    // two passes across all slots: opaque+mask first, then blended w/o depth writes
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (GpuModelData& slot : slots_) {
            if (!slot.model || !slot.visible) continue;
            drawSlot(slot, skeleton, bodyModel, pass);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

void ModelRenderer::drawGrid(const Camera& camera, float aspect) {
    Mat4 vp = camera.projection(aspect) * camera.view();
    gridShader_.use();
    gridShader_.setMat4("uVP", vp);
    gridShader_.setVec3("uColor", Vec3{0.28f, 0.30f, 0.33f});
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(gridVao_);
    glDrawArrays(GL_LINES, 0, gridVertexCount_);
    glBindVertexArray(0);
}

} // namespace ce
