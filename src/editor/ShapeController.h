#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ce {

// A single editable shape parameter, either morph-driven or bone-driven.
struct ShapeParam {
    enum class Type { Morph, BoneScale };
    Type type;
    std::string id;    // stable id used in presets, e.g. "breast_size" / "morph.Fcl_EYE_Joy"
    std::string name;  // UI label
    std::string group; // UI group (collapsing header)
    float minS = 0.f;  // BoneScale: scale at value 0   | Morph: weight at value 0 (0)
    float maxS = 1.f;  // BoneScale: scale at value 1   | Morph: weight at value 1 (1)
    float defValue = 0.5f; // normalized default
    float value = 0.5f;    // normalized 0..1

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

    std::vector<ShapeParam>& params() { return params_; }
    ShapeParam* find(const std::string& id);
    void setValue(const std::string& id, float v);
    void resetAll();

    // Preset values as id -> normalized value.
    std::map<std::string, float> values() const;
    void setValues(const std::map<std::string, float>& v);

    bool morphsDirty = true; // renderer must re-upload morphed vertex data
    uint64_t revision = 0;   // bumped on every apply() — for debounced clothing refit

private:
    Model* model_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    std::vector<ShapeParam> params_;

    void addBoneParamsFromRules(const std::string& configPath);
    void addMorphParams();
    std::vector<int> matchBones(const std::vector<std::string>& names,
                                const std::vector<std::string>& patterns) const;
    static std::string morphGroup(const std::string& morphName);
    static std::string morphLabel(const std::string& morphName);
};

} // namespace ce
