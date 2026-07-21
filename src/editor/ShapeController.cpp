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

std::vector<int> ShapeController::matchBones(const std::vector<std::string>& names,
                                             const std::vector<std::string>& patterns) const {
    std::vector<int> out;
    for (size_t i = 0; i < model_->nodes.size(); ++i) {
        const std::string& bn = model_->nodes[i].name;
        bool hit = std::find(names.begin(), names.end(), bn) != names.end();
        if (!hit)
            for (const std::string& pat : patterns) {
                try {
                    if (std::regex_search(bn, std::regex(pat))) { hit = true; break; }
                } catch (...) { /* invalid pattern: ignore */ }
            }
        if (hit && std::find(out.begin(), out.end(), static_cast<int>(i)) == out.end())
            out.push_back(static_cast<int>(i));
    }
    return out;
}

namespace {

void readNamesPatterns(const nlohmann::json& j, std::vector<std::string>& names,
                       std::vector<std::string>& patterns) {
    if (j.contains("bones"))
        for (const auto& b : j["bones"]) names.push_back(b.get<std::string>());
    if (j.contains("patterns"))
        for (const auto& b : j["patterns"]) patterns.push_back(b.get<std::string>());
}

Vec3 readVec3(const nlohmann::json& j, const char* key, const Vec3& def) {
    if (j.contains(key) && j[key].size() == 3)
        return {j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>()};
    return def;
}

} // namespace

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
        p.defValue = (p.maxS > p.minS) ? std::clamp((1.f - p.minS) / (p.maxS - p.minS), 0.f, 1.f) : 0.5f;
        p.value = p.defValue;

        bool hasContent = false;

        // Format A (flat, backward compatible): bones/patterns/axes/compensate at top level
        if (r.contains("bones") || r.contains("patterns")) {
            std::vector<std::string> names, patterns;
            readNamesPatterns(r, names, patterns);
            ShapeParam::BoneRule br;
            br.bones = matchBones(names, patterns);
            br.axes = readVec3(r, "axes", Vec3{1, 1, 1});
            br.compensate = r.value("compensate", false);
            if (!br.bones.empty()) { p.rules.push_back(std::move(br)); hasContent = true; }
        }

        // Format B (entries): per-rule axes + optional translate rules
        if (r.contains("entries")) {
            for (const auto& e : r["entries"]) {
                if (e.contains("translate")) {
                    const auto& t = e["translate"];
                    std::vector<std::string> names, patterns;
                    readNamesPatterns(t, names, patterns);
                    ShapeParam::TranslateRule tr;
                    tr.bones = matchBones(names, patterns);
                    tr.axis = readVec3(t, "axis", Vec3{0, 1, 0});
                    tr.factor = t.value("factor", 0.f);
                    tr.scaleMode = t.value("mode", std::string{"value"}) == "scale";
                    if (!tr.bones.empty() && tr.factor != 0.f) {
                        p.translateRules.push_back(std::move(tr));
                        hasContent = true;
                    }
                } else {
                    std::vector<std::string> names, patterns;
                    readNamesPatterns(e, names, patterns);
                    ShapeParam::BoneRule br;
                    br.bones = matchBones(names, patterns);
                    br.axes = readVec3(e, "axes", Vec3{1, 1, 1});
                    br.compensate = e.value("compensate", false);
                    if (!br.bones.empty()) { p.rules.push_back(std::move(br)); hasContent = true; }
                }
            }
        }

        if (hasContent && !p.id.empty() && !find(p.id))
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
    ++revision;

    // 1) reset skeleton offsets
    for (auto& o : skeleton_->scaleOffset) o = {1, 1, 1};
    for (auto& o : skeleton_->translateOffset) o = {0, 0, 0};

    // 2) bone-driven params
    for (const ShapeParam& p : params_) {
        if (p.type != ShapeParam::Type::BoneScale) continue;
        float s = p.minS + (p.maxS - p.minS) * p.value;

        for (const auto& rule : p.rules) {
            Vec3 sv{std::pow(s, rule.axes.x), std::pow(s, rule.axes.y), std::pow(s, rule.axes.z)};
            for (int b : rule.bones) {
                skeleton_->scaleOffset[b] = skeleton_->scaleOffset[b] * sv;
                if (rule.compensate) {
                    for (int c : m.nodes[b].children) {
                        // do not compensate children that are themselves targets
                        if (std::find(rule.bones.begin(), rule.bones.end(), c) != rule.bones.end())
                            continue;
                        Vec3& co = skeleton_->scaleOffset[c];
                        co.x /= sv.x; co.y /= sv.y; co.z /= sv.z;
                    }
                }
            }
        }

        for (const auto& tr : p.translateRules) {
            float k = tr.scaleMode ? tr.factor * (s - 1.f)
                                   : tr.factor * (p.value - p.defValue);
            Vec3 off = tr.axis * k;
            for (int b : tr.bones)
                skeleton_->translateOffset[b] = skeleton_->translateOffset[b] + off;
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
