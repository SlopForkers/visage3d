#include "editor/ShapeController.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>

namespace ce {

namespace {

// Built-in detection rules (used when config/body_params.json is missing).
// axes = per-axis exponent of applied scale; compensate = undo scale on children.
const char* kBuiltinRules = R"json({
  "boneParams": [
    { "id": "breast_size", "name": "Breast size", "group": "Body",
      "bones": ["J_Sec_L_Bust1", "J_Sec_R_Bust1"],
      "patterns": ["(?i)(bust|breast|chest)_?[lr]?_?1$", "(?i)^mune_[lr]$"],
      "min": 0.5, "max": 1.8, "axes": [1.0, 1.0, 1.0], "compensate": false },

    { "id": "buttocks_size", "name": "Buttocks size", "group": "Body",
      "bones": ["J_Bip_C_Hips"],
      "patterns": ["(?i)^(hips|pelvis)$", "(?i)(hips|pelvis)$"],
      "min": 0.6, "max": 1.6, "axes": [0.6, 0.35, 1.0], "compensate": true },

    { "id": "hip_width", "name": "Hip width", "group": "Body",
      "bones": ["J_Bip_C_Hips"],
      "patterns": ["(?i)^(hips|pelvis)$", "(?i)(hips|pelvis)$"],
      "min": 0.8, "max": 1.4, "axes": [1.0, 0.1, 0.25], "compensate": true },

    { "id": "waist_size", "name": "Waist", "group": "Body",
      "bones": ["J_Bip_C_Spine"],
      "patterns": ["(?i)spine_?1?$", "(?i)^(spine|waist)$"],
      "min": 0.75, "max": 1.35, "axes": [1.0, 0.0, 1.0], "compensate": true }
  ]
})json";

} // namespace

void ShapeController::bind(Model& model, Skeleton& skeleton) {
    model_ = &model;
    skeleton_ = &skeleton;
}

void ShapeController::scan(const std::string& configPath) {
    params_.clear();
    addBoneParamsFromRules(configPath);
    addMorphParams();
    apply();
    morphsDirty = true;
}

ShapeParam* ShapeController::find(const std::string& id) {
    for (auto& p : params_)
        if (p.id == id) return &p;
    return nullptr;
}

void ShapeController::setValue(const std::string& id, float v) {
    if (ShapeParam* p = find(id)) {
        p->value = std::clamp(v, 0.f, 1.f);
        apply();
    }
}

void ShapeController::resetAll() {
    for (auto& p : params_) p.value = p.defValue;
    apply();
}

std::map<std::string, float> ShapeController::values() const {
    std::map<std::string, float> out;
    for (const auto& p : params_)
        if (p.value != p.defValue) out[p.id] = p.value;
    return out;
}

void ShapeController::setValues(const std::map<std::string, float>& v) {
    for (const auto& kv : v)
        if (ShapeParam* p = find(kv.first))
            p->value = std::clamp(kv.second, 0.f, 1.f);
    apply();
}

int ShapeController::findBone(const std::vector<std::string>& names,
                              const std::vector<std::string>& patterns) const {
    for (size_t i = 0; i < model_->nodes.size(); ++i)
        if (std::find(names.begin(), names.end(), model_->nodes[i].name) != names.end())
            return static_cast<int>(i);
    for (const std::string& pat : patterns) {
        std::regex re(pat);
        for (size_t i = 0; i < model_->nodes.size(); ++i)
            if (std::regex_search(model_->nodes[i].name, re))
                return static_cast<int>(i);
    }
    return -1;
}

void ShapeController::addBoneParamsFromRules(const std::string& configPath) {
    nlohmann::json rules;
    {
        std::ifstream f(configPath);
        if (f) {
            try { rules = nlohmann::json::parse(f); }
            catch (...) { rules = nlohmann::json::object(); }
        }
    }
    if (!rules.contains("boneParams"))
        rules = nlohmann::json::parse(kBuiltinRules);

    for (const auto& r : rules["boneParams"]) {
        ShapeParam p;
        p.type = ShapeParam::Type::BoneScale;
        p.id = r.value("id", std::string{});
        p.name = r.value("name", p.id);
        p.group = r.value("group", std::string{"Body"});
        p.minS = r.value("min", 0.5f);
        p.maxS = r.value("max", 1.5f);
        p.compensate = r.value("compensate", false);
        if (r.contains("axes") && r["axes"].size() == 3)
            p.axes = {r["axes"][0].get<float>(), r["axes"][1].get<float>(), r["axes"][2].get<float>()};
        // default: scale 1.0 mapped into normalized space
        p.defValue = (p.maxS > p.minS) ? std::clamp((1.f - p.minS) / (p.maxS - p.minS), 0.f, 1.f) : 0.5f;
        p.value = p.defValue;

        // Collect all matching bones (exact names first, then regex patterns).
        std::vector<std::string> names, patterns;
        if (r.contains("bones"))
            for (const auto& b : r["bones"]) names.push_back(b.get<std::string>());
        if (r.contains("patterns"))
            for (const auto& b : r["patterns"]) patterns.push_back(b.get<std::string>());

        for (size_t i = 0; i < model_->nodes.size(); ++i) {
            const std::string& bn = model_->nodes[i].name;
            bool hit = std::find(names.begin(), names.end(), bn) != names.end();
            if (!hit)
                for (const auto& pat : patterns) {
                    try {
                        if (std::regex_search(bn, std::regex(pat))) { hit = true; break; }
                    } catch (...) { /* invalid pattern: ignore */ }
                }
            if (hit && std::find(p.bones.begin(), p.bones.end(), static_cast<int>(i)) == p.bones.end())
                p.bones.push_back(static_cast<int>(i));
        }

        if (!p.bones.empty() && !p.id.empty() && !find(p.id))
            params_.push_back(std::move(p));
    }
}

void ShapeController::addMorphParams() {
    for (size_t mi = 0; mi < model_->meshes.size(); ++mi) {
        Mesh& mesh = model_->meshes[mi];
        if (mesh.prims.empty()) continue;
        const Primitive& ref = mesh.prims.front();
        for (size_t ti = 0; ti < ref.morphs.size(); ++ti) {
            ShapeParam p;
            p.type = ShapeParam::Type::Morph;
            p.id = "morph." + ref.morphs[ti].name;
            p.name = morphLabel(ref.morphs[ti].name);
            p.group = morphGroup(ref.morphs[ti].name);
            p.minS = 0.f;
            p.maxS = 1.f;
            p.defValue = 0.f;
            p.value = 0.f;
            p.mesh = static_cast<int>(mi);
            p.morphTarget = static_cast<int>(ti);
            params_.push_back(std::move(p));
        }
    }
}

std::string ShapeController::morphGroup(const std::string& n) {
    auto part = [&](const char* key) { return n.find(key) != std::string::npos; };
    if (part("_BRW_")) return "Лицо / Брови";
    if (part("_EYE_")) return "Лицо / Глаза";
    if (part("_MTH_")) return "Лицо / Рот";
    if (part("_HA_"))  return "Лицо / Блики";
    if (part("_ALL_")) return "Лицо / Эмоции";
    return "Морфы";
}

std::string ShapeController::morphLabel(const std::string& n) {
    // Fcl_BRW_Angry -> "Angry"; Fcl_EYE_Close_R -> "Close R"
    if (n.rfind("Fcl_", 0) == 0) {
        size_t first = n.find('_');
        size_t second = n.find('_', first + 1);
        if (second != std::string::npos) {
            std::string label = n.substr(second + 1);
            std::replace(label.begin(), label.end(), '_', ' ');
            return label;
        }
    }
    return n;
}

void ShapeController::apply() {
    Model& m = *model_;

    // 1) reset skeleton offsets
    for (auto& o : skeleton_->scaleOffset) o = {1, 1, 1};

    // 2) bone-scale params
    for (const ShapeParam& p : params_) {
        if (p.type != ShapeParam::Type::BoneScale) continue;
        float s = p.minS + (p.maxS - p.minS) * p.value;
        Vec3 sv{std::pow(s, p.axes.x), std::pow(s, p.axes.y), std::pow(s, p.axes.z)};
        for (int b : p.bones) {
            skeleton_->scaleOffset[b] = skeleton_->scaleOffset[b] * sv;
            if (p.compensate) {
                for (int c : m.nodes[b].children) {
                    // do not compensate children that are themselves targets
                    if (std::find(p.bones.begin(), p.bones.end(), c) != p.bones.end()) continue;
                    Vec3& co = skeleton_->scaleOffset[c];
                    co.x /= sv.x; co.y /= sv.y; co.z /= sv.z;
                }
            }
        }
    }

    // 3) morph weights (a target may be shared by several prims of one mesh)
    for (const ShapeParam& p : params_) {
        if (p.type != ShapeParam::Type::Morph) continue;
        float w = p.minS + (p.maxS - p.minS) * p.value;
        Mesh& mesh = m.meshes[p.mesh];
        for (Primitive& prim : mesh.prims) {
            if (p.morphTarget < static_cast<int>(prim.morphs.size())) {
                float& dst = prim.morphWeights[p.morphTarget];
                if (dst != w) { dst = w; morphsDirty = true; }
            }
        }
    }
}

} // namespace ce
