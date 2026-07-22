#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include "json.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ce {

// A single editable shape parameter, either morph-driven or bone-driven.
struct ShapeParam {
    enum class Type { Morph, BoneScale, VertexEffect, VertexDeform };
    Type type;
    std::string id;    // stable id used in presets, e.g. "breast_size" / "morph.Fcl_EYE_Joy"
    std::string name;  // UI label
    std::string group; // UI group (collapsing header)
    float minS = 0.f;  // BoneScale: scale at value 0   | Morph: weight at value 0 (0)
    float maxS = 1.f;  // BoneScale: scale at value 1   | Morph: weight at value 1 (1)
                       // VertexEffect: meters (tint: 0..1) at value 0 / 1
    float defValue = 0.5f; // normalized default
    float value = 0.5f;    // normalized 0..1

    // VertexEffect kind (Type::VertexEffect only)
    enum class Effect { Protrude = 0, TipRadius = 1, AreolaRadius = 2, Bulge = 3, Tint = 4 };
    Effect effect = Effect::Protrude;

    // VertexDeform (Type::VertexDeform only): region transform around named
    // anchors (bind space, gaussian falloff). magnitude = lerp(minS, maxS).
    enum class DeformKind { Translate = 0, Scale = 1, Protrude = 2 };
    DeformKind deformKind = DeformKind::Translate;
    std::vector<int> deformAnchors;   // indices into the controller's deformAnchors_
    std::vector<Vec3> deformAxes;     // per-anchor axis (size 1 = shared by all)
    float deformRadius = 0.03f;       // gaussian falloff radius (m)

    // Bone-scale rules: each entry scales a set of bones around given axes
    // (axes = per-axis exponent), optionally compensating direct children.
    struct BoneRule {
        std::vector<int> bones;
        Vec3 axes{1, 1, 1};
        bool compensate = false;
    };
    // Bone-translate rules: offset bones along an axis.
    // scaleMode=false: offset = axis * factor * (value - defValue)
    // scaleMode=true:  offset = axis * factor * (currentScale - 1)
    struct TranslateRule {
        std::vector<int> bones;
        Vec3 axis{0, 1, 0};
        float factor = 0.f;
        bool scaleMode = false;
    };
    std::vector<BoneRule> rules;
    std::vector<TranslateRule> translateRules;

    // Morph specifics
    int mesh = -1;
    int morphTarget = -1;
};

// Owns the parameter set of a loaded model, auto-detects body parameters
// (breast / buttocks / hips / waist) from bone names, exposes every morph
// target as a slider, and applies values to the model + skeleton.
class ShapeController {
public:
    void bind(Model& model, Skeleton& skeleton);

    // (Re)builds the parameter list. Tries config/body_params.json first,
    // falls back to built-in rules. Resets values to defaults.
    void scan(const std::string& configPath = "config/body_params.json");

    void apply(); // push values into model morph weights + skeleton scale offsets

    // ---- vertex effectors (nipple / areola editor) ----
    // Gaussian domes around the breast apex vertices (anchored to the bust
    // bones, so they follow breast_size). Idempotent: displaced vertices are
    // rewritten from stored bind bases. Call after the morph re-blend.
    void applyEffectorsToMesh();
    bool hasEffectors() const { return !effAnchors_.empty() || !deformAnchors_.empty(); }
    // current effector state (for the areola shader tint)
    Vec3 effectorAnchorWorld(int index) const; // index < effectorAnchorCount()
    int effectorAnchorCount() const { return static_cast<int>(effAnchors_.size()); }
    float effectorValue(ShapeParam::Effect kind) const; // meters (tint: 0..1)

    std::vector<ShapeParam>& params() { return params_; }
    ShapeParam* find(const std::string& id);
    void setValue(const std::string& id, float v);
    void resetAll();

    // Preset values as id -> normalized value.
    std::map<std::string, float> values() const;
    void setValues(const std::map<std::string, float>& v);

    bool morphsDirty = true; // renderer must re-upload morphed vertex data
    uint64_t revision = 0;   // bumped on every apply() — invalidates the clothing point cloud

private:
    Model* model_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    std::vector<ShapeParam> params_;

    void addBoneParamsFromRules(const std::string& configPath);
    void addEffectorParams(const std::string& configPath);
    void buildEffectorAnchors(const std::vector<std::string>& names,
                              const std::vector<std::string>& patterns);
    void addMorphParams();
    std::vector<int> matchBones(const std::vector<std::string>& names,
                                const std::vector<std::string>& patterns) const;
    static std::string morphGroup(const std::string& morphName);
    static std::string morphLabel(const std::string& morphName);

    // ---- vertex effector state ----
    struct EffectorVert {
        int mesh, prim, vert;
        float d;        // |bindPos - apex|
        Vec3 toVert;    // bindPos - apex (gradient direction)
        Vec3 basePos;   // bind base (idempotent rewrite)
        Vec3 baseNrm;
    };
    struct EffectorAnchor {
        int node = -1; // bust bone node
        int skin = -1; // skin driving the body mesh that contains the node
        int joint = -1; // joint index of the node within that skin
        Vec3 apexBind{0, 0, 0}; // nipple apex (bind): farthest bust-weighted vertex
        Vec3 nrmBind{0, 1, 0};
        std::vector<EffectorVert> verts; // vertices inside the max falloff radius
    };
    std::vector<EffectorAnchor> effAnchors_;
    int effParam_[5] = {-1, -1, -1, -1, -1}; // param index per Effect kind
    float effLast_[5] = {-1.f, -1.f, -1.f, -1.f, -1.f}; // change detection

    // ---- vertex deform state (face editor: eyes / nose / mouth) ----
    // Region transform around a named anchor, applied additively on top of
    // the morph blend (idempotent: the blend resets blendedPos first).
    struct DeformAnchor {
        std::string id;
        Vec3 pos{0, 0, 0};   // bind-space anchor position
        Vec3 nrm{0, 0, 1};   // surface normal at the anchor (for Protrude)
        struct V {
            int mesh, prim, vert;
            float d;     // |bindPos - pos|
            Vec3 toVert; // bindPos - pos
        };
        std::vector<V> verts; // vertices inside the hard radius cap
    };
    std::vector<DeformAnchor> deformAnchors_;
    std::vector<float> deformLast_; // per-param change detection

    void addDeformParams(const std::string& configPath);
    void buildDeformAnchors(const nlohmann::json& anchorsJson);
    void addDeformParamsFromJson(const nlohmann::json& rules);
};

} // namespace ce
