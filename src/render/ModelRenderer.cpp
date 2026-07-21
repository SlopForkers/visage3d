#include "render/ModelRenderer.h"
#include "core/GL.h"
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
uniform vec3 uLightDir;
uniform vec3 uCamPos;

void main() {
    vec4 c = uBaseColor;
    if (uHasTex == 1) c *= texture(uTex, vUV);
    if (uAlphaMode == 1 && c.a < uCutoff) discard;

    vec3 col = c.rgb;
    if (uUnlit == 0) {
        vec3 N = normalize(vNrm);
        if (!gl_FrontFacing) N = -N;
        vec3 L = normalize(uLightDir);
        float wrap = dot(N, L) * 0.5 + 0.5;
        vec3 V = normalize(uCamPos - vPos);
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.08;
        col = col * (0.35 + 0.65 * wrap) + vec3(spec);
    }
    fragColor = vec4(pow(max(col, 0.0), vec3(1.0 / 2.2)), c.a);
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

void ModelRenderer::upload(Model& model) {
    release();

    textures_.reserve(model.textures.size());
    for (const Texture& t : model.textures) textures_.push_back(uploadTexture(t));

    meshes_.resize(model.meshes.size());
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const Mesh& mesh = model.meshes[mi];
        GpuMesh& gm = meshes_[mi];
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
            if (!prim.uv.empty() || gp.skinned) {
                struct StaticVert {
                    float uv[2];
                    uint16_t joints[4];
                    float weights[4];
                };
                std::vector<StaticVert> sv(gp.vertexCount);
                for (size_t i = 0; i < gp.vertexCount; ++i) {
                    if (i < prim.uv.size()) { sv[i].uv[0] = prim.uv[i].x; sv[i].uv[1] = prim.uv[i].y; }
                    if (gp.skinned) {
                        for (int k = 0; k < 4; ++k) {
                            sv[i].joints[k] = prim.joints[i * 4 + k];
                            const float* w = &prim.weights[i].x;
                            sv[i].weights[k] = w[k];
                        }
                    }
                }
                glGenBuffers(1, &gp.vboStatic);
                glBindBuffer(GL_ARRAY_BUFFER, gp.vboStatic);
                glBufferData(GL_ARRAY_BUFFER, sv.size() * sizeof(StaticVert), sv.data(),
                             GL_STATIC_DRAW);
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(StaticVert), nullptr);
                glEnableVertexAttribArray(3);
                glVertexAttribPointer(3, 4, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(StaticVert),
                                      reinterpret_cast<void*>(offsetof(StaticVert, joints)));
                glEnableVertexAttribArray(4);
                glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(StaticVert),
                                      reinterpret_cast<void*>(offsetof(StaticVert, weights)));
            }

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
}

void ModelRenderer::uploadVertices(GpuPrim& gp, const Primitive& prim) {
    std::vector<float> buf(gp.vertexCount * 6);
    const std::vector<Vec3>& pos = prim.blendedPos.empty() ? prim.pos : prim.blendedPos;
    const std::vector<Vec3>& nrm = prim.blendedNormal.empty() ? prim.normal : prim.blendedNormal;
    for (size_t v = 0; v < gp.vertexCount; ++v) {
        buf[v * 6 + 0] = pos[v].x;
        buf[v * 6 + 1] = pos[v].y;
        buf[v * 6 + 2] = pos[v].z;
        if (v < nrm.size()) {
            buf[v * 6 + 3] = nrm[v].x;
            buf[v * 6 + 4] = nrm[v].y;
            buf[v * 6 + 5] = nrm[v].z;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, gp.vboDyn);
    glBufferSubData(GL_ARRAY_BUFFER, 0, buf.size() * sizeof(float), buf.data());
}

void ModelRenderer::syncMorphs(Model& model) {
    for (size_t mi = 0; mi < model.meshes.size() && mi < meshes_.size(); ++mi) {
        GpuMesh& gm = meshes_[mi];
        if (!gm.hasMorphs) continue;
        Mesh& mesh = model.meshes[mi];
        for (size_t pi = 0; pi < mesh.prims.size(); ++pi) {
            Primitive& prim = mesh.prims[pi];
            // CPU blend: base + sum(weight * delta)
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
            uploadVertices(gm.prims[pi], prim);
        }
    }
}

void ModelRenderer::drawPrim(const GpuPrim& gp, const Model& model) {
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
        if (mat->texture >= 0 && mat->texture < static_cast<int>(textures_.size()))
            tex = textures_[mat->texture];
        doubleSided = mat->doubleSided;
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

void ModelRenderer::draw(const Model& model, const Skeleton& skeleton, const Camera& camera,
                         float aspect, bool wireframe) {
    Mat4 vp = camera.projection(aspect) * camera.view();
    modelShader_.use();
    modelShader_.setMat4("uVP", vp);
    modelShader_.setVec3("uLightDir", Vec3{0.5f, 0.9f, 0.7f}.normalized());
    modelShader_.setVec3("uCamPos", camera.eye());

    if (wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    glEnable(GL_DEPTH_TEST);

    // two passes: opaque+mask first, then blended with depth writes off
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (const MeshInstance& inst : model.meshInstances) {
            if (inst.mesh < 0 || inst.mesh >= static_cast<int>(meshes_.size())) continue;
            const GpuMesh& gm = meshes_[inst.mesh];

            modelShader_.setInt("uUseSkin", 0);
            modelShader_.setMat4("uNodeMat", skeleton.world()[inst.node]);
            if (inst.skin >= 0 && inst.skin < static_cast<int>(model.skins.size())) {
                skeleton.skinMatrices(inst.skin, boneMats_, boneNrms_);
                int nb = static_cast<int>(boneMats_.size());
                if (nb > kMaxBones) {
                    nb = kMaxBones; // safety: never overflow the uniform array
                }
                modelShader_.setInt("uUseSkin", 1);
                glUniformMatrix4fv(
                    glGetUniformLocation(modelShader_.id(), "uBones"), nb, GL_FALSE,
                    boneMats_[0].m);
            }

            for (const GpuPrim& gp : gm.prims) {
                const Material* mat = nullptr;
                if (gp.material >= 0 && gp.material < static_cast<int>(model.materials.size()))
                    mat = &model.materials[gp.material];
                bool blended = mat && mat->alpha == Material::Alpha::Blend;
                if ((pass == 1) != blended) continue;
                drawPrim(gp, model);
            }
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

void ModelRenderer::release() {
    for (GpuMesh& gm : meshes_)
        for (GpuPrim& gp : gm.prims) {
            if (gp.vao) glDeleteVertexArrays(1, &gp.vao);
            if (gp.vboStatic) glDeleteBuffers(1, &gp.vboStatic);
            if (gp.vboDyn) glDeleteBuffers(1, &gp.vboDyn);
            if (gp.ebo) glDeleteBuffers(1, &gp.ebo);
        }
    meshes_.clear();
    for (unsigned t : textures_)
        if (t && t != whiteTex_) glDeleteTextures(1, &t);
    textures_.clear();
}

} // namespace ce
