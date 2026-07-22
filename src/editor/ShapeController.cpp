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
  ],

  "effectorAnchors": {
    "bones": ["J_Sec_L_Bust1", "J_Sec_R_Bust1"],
    "patterns": ["(?i)(bust|breast)_?[lr]?_?1$", "(?i)^mune_[lr]$"]
  },
  "effectorParams": [
    { "id": "nipple_length", "name": "Соски: выпирание", "group": "Соски",
      "kind": "protrude", "min": 0.0, "max": 0.03 },
    { "id": "nipple_width", "name": "Соски: ширина", "group": "Соски",
      "kind": "tipRadius", "min": 0.004, "max": 0.02, "def": 0.5 },
    { "id": "areola_size", "name": "Ореола: размер", "group": "Соски",
      "kind": "areolaRadius", "min": 0.01, "max": 0.05, "def": 0.4 },
    { "id": "areola_bulge", "name": "Ореола: выпуклость", "group": "Соски",
      "kind": "bulge", "min": 0.0, "max": 0.012 },
    { "id": "areola_tint", "name": "Ореола: затемнение", "group": "Соски",
      "kind": "tint", "min": 0.0, "max": 1.0 }
  ],

  "deformAnchors": {
    "eye_l": { "bones": ["J_Adj_L_FaceEye"],
               "patterns": ["(?i)l_?face_?eye$", "(?i)eye_?l$", "(?i)left_?eye$"] },
    "eye_r": { "bones": ["J_Adj_R_FaceEye"],
               "patterns": ["(?i)r_?face_?eye$", "(?i)eye_?r$", "(?i)right_?eye$"] },
    "mouth": { "material": ["(?i)facemouth", "(?i)mouth"] },
    "nose":  { "material": ["(?i)face_?00_?skin", "(?i)face.*skin"], "extreme": [0.0, 0.0, 1.0] }
  },
  "deformParams": [
    { "id": "eye_size", "name": "Размер глаз", "group": "Глаза",
      "anchors": ["eye_l", "eye_r"], "kind": "scale", "axis": [1.0, 1.0, 1.0],
      "radius": 0.04, "min": 0.8, "max": 1.25 },
    { "id": "eye_spacing", "name": "Расстояние глаз", "group": "Глаза",
      "anchors": ["eye_l", "eye_r"], "kind": "translate",
      "axis": [[1.0, 0.0, 0.0], [-1.0, 0.0, 0.0]],
      "radius": 0.04, "min": -0.006, "max": 0.006 },
    { "id": "eye_height", "name": "Высота глаз", "group": "Глаза",
      "anchors": ["eye_l", "eye_r"], "kind": "translate", "axis": [0.0, 1.0, 0.0],
      "radius": 0.04, "min": -0.006, "max": 0.006 },
    { "id": "nose_size", "name": "Размер носа", "group": "Нос",
      "anchors": ["nose"], "kind": "scale", "axis": [1.0, 1.0, 1.0],
      "radius": 0.03, "min": 0.75, "max": 1.35 },
    { "id": "nose_length", "name": "Длина носа", "group": "Нос",
      "anchors": ["nose"], "kind": "translate", "axis": [0.0, 0.0, 1.0],
      "radius": 0.028, "min": -0.004, "max": 0.01 },
    { "id": "nose_height", "name": "Высота носа", "group": "Нос",
      "anchors": ["nose"], "kind": "translate", "axis": [0.0, 1.0, 0.0],
      "radius": 0.028, "min": -0.005, "max": 0.005 },
    { "id": "nose_width", "name": "Ширина носа", "group": "Нос",
      "anchors": ["nose"], "kind": "scale", "axis": [1.0, 0.0, 0.0],
      "radius": 0.03, "min": 0.7, "max": 1.5 },
    { "id": "mouth_width", "name": "Ширина рта", "group": "Рот",
      "anchors": ["mouth"], "kind": "scale", "axis": [1.0, 0.0, 0.0],
      "radius": 0.032, "min": 0.7, "max": 1.4 },
    { "id": "mouth_height", "name": "Высота рта", "group": "Рот",
      "anchors": ["mouth"], "kind": "translate", "axis": [0.0, 1.0, 0.0],
      "radius": 0.03, "min": -0.005, "max": 0.005 },
    { "id": "mouth_size", "name": "Размер рта", "group": "Рот",
      "anchors": ["mouth"], "kind": "scale", "axis": [1.0, 1.0, 1.0],
      "radius": 0.032, "min": 0.8, "max": 1.3 }
  ]
})json";

} // namespace

void ShapeController::bind(Model& model, Skeleton& skeleton) {
    model_ = &model;
    skeleton_ = &skeleton;
}

void ShapeController::scan(const std::string& configPath) {
    params_.clear();
    effAnchors_.clear();
    for (int& i : effParam_) i = -1;
    for (float& f : effLast_) f = -1.f;
    addBoneParamsFromRules(configPath);
    addEffectorParams(configPath);
    addDeformParams(configPath);
    addMorphParams();
    deformLast_.assign(params_.size(), -1.f);
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

// ---- vertex effectors (nipple / areola) ----

void ShapeController::addEffectorParams(const std::string& configPath) {
    nlohmann::json rules;
    {
        std::ifstream f(configPath);
        if (f) {
            try { rules = nlohmann::json::parse(f); }
            catch (...) { rules = nlohmann::json::object(); }
        }
    }
    nlohmann::json builtin = nlohmann::json::parse(kBuiltinRules);
    if (!rules.contains("effectorParams")) rules["effectorParams"] = builtin["effectorParams"];
    if (!rules.contains("effectorAnchors"))
        rules["effectorAnchors"] = builtin["effectorAnchors"];

    auto kindOf = [](const std::string& k) -> ShapeParam::Effect {
        if (k == "tipRadius") return ShapeParam::Effect::TipRadius;
        if (k == "areolaRadius") return ShapeParam::Effect::AreolaRadius;
        if (k == "bulge") return ShapeParam::Effect::Bulge;
        if (k == "tint") return ShapeParam::Effect::Tint;
        return ShapeParam::Effect::Protrude;
    };

    bool any = false;
    for (const auto& r : rules["effectorParams"]) {
        ShapeParam p;
        p.type = ShapeParam::Type::VertexEffect;
        p.id = r.value("id", std::string{});
        p.name = r.value("name", p.id);
        p.group = r.value("group", std::string{"Соски"});
        p.effect = kindOf(r.value("kind", std::string{"protrude"}));
        p.minS = r.value("min", 0.f);
        p.maxS = r.value("max", 1.f);
        p.defValue = r.value("def", 0.f);
        p.value = p.defValue;
        if (p.id.empty() || find(p.id)) continue;
        effParam_[static_cast<int>(p.effect)] = static_cast<int>(params_.size());
        params_.push_back(std::move(p));
        any = true;
    }

    if (any) {
        std::vector<std::string> names, patterns;
        readNamesPatterns(rules["effectorAnchors"], names, patterns);
        buildEffectorAnchors(names, patterns);
    }
}

void ShapeController::buildEffectorAnchors(const std::vector<std::string>& names,
                                           const std::vector<std::string>& patterns) {
    if (!model_ || !skeleton_) return;
    skeleton_->update(); // world = bind pose right after bind()
    constexpr float kMaxRadius = 0.06f;  // hard cap of the affected region (m)
    constexpr float kMinBoneWeight = 0.25f;

    for (int node : matchBones(names, patterns)) {
        // Candidate (skin, joint) pairs containing this bone. A bone may be
        // listed in several skins (face + body): the right one actually OWNS
        // vertices weighted to it.
        struct Cand {
            int skin, joint;
        };
        std::vector<Cand> cands;
        for (size_t si = 0; si < model_->skins.size(); ++si)
            for (size_t j = 0; j < model_->skins[si].joints.size(); ++j)
                if (model_->skins[si].joints[j] == node)
                    cands.push_back({static_cast<int>(si), static_cast<int>(j)});

        // The bone chain tip points at the nipple (VRoid: Bust1 -> Bust2).
        // Use the first child that is also a skin joint; fall back to the bone
        // itself when there is no suitable child.
        int tipNode = node;
        for (int child : model_->nodes[node].children) {
            bool inSkin = false;
            for (const Cand& c : cands)
                for (int jn : model_->skins[c.skin].joints)
                    if (jn == child) { inSkin = true; break; }
            if (inSkin) { tipNode = child; break; }
        }
        const Mat4& wm = skeleton_->world()[tipNode];
        Vec3 bonePos{wm.m[12], wm.m[13], wm.m[14]};

        EffectorAnchor a;
        a.node = node;
        float bestD2 = 1e30f;
        for (const Cand& c : cands) {
            if (bestD2 < 1e29f) break; // a previous skin already owns the verts
            a.skin = c.skin;
            a.joint = c.joint;
            // nipple apex: the bust-weighted body vertex nearest to the chain tip
            for (const MeshInstance& inst : model_->meshInstances) {
                if (inst.skin != c.skin) continue;
                for (size_t pi = 0; pi < model_->meshes[inst.mesh].prims.size(); ++pi) {
                    const Primitive& prim = model_->meshes[inst.mesh].prims[pi];
                    // skip hair materials (hair can share the body skin)
                    if (prim.material >= 0 &&
                        prim.material < static_cast<int>(model_->materials.size())) {
                        const std::string& mn = model_->materials[prim.material].name;
                        if (mn.find("HAIR") != std::string::npos ||
                            mn.find("Hair") != std::string::npos)
                            continue;
                    }
                    for (size_t v = 0; v < prim.pos.size(); ++v) {
                        if (prim.joints.empty() || prim.weights.empty()) continue;
                        float w = 0.f;
                        for (int k = 0; k < 4; ++k)
                            if (prim.joints[v * 4 + k] == c.joint) w += (&prim.weights[v].x)[k];
                        if (w < kMinBoneWeight) continue;
                        Vec3 dv = prim.pos[v] - bonePos;
                        float d2 = dv.dot(dv);
                        if (d2 < bestD2) {
                            bestD2 = d2;
                            a.apexBind = prim.pos[v];
                            a.nrmBind = v < prim.normal.size() ? prim.normal[v] : Vec3{0, 1, 0};
                        }
                    }
                }
            }
        }
        if (bestD2 > 1e29f) continue; // bone drives no vertices

        for (const MeshInstance& inst : model_->meshInstances) {
            if (inst.skin != a.skin) continue;
            for (size_t pi = 0; pi < model_->meshes[inst.mesh].prims.size(); ++pi) {
                Primitive& prim = model_->meshes[inst.mesh].prims[pi];
                if (prim.material >= 0 &&
                    prim.material < static_cast<int>(model_->materials.size())) {
                    const std::string& mn = model_->materials[prim.material].name;
                    if (mn.find("HAIR") != std::string::npos || mn.find("Hair") != std::string::npos)
                        continue;
                }
                for (size_t v = 0; v < prim.pos.size(); ++v) {
                    Vec3 dv = prim.pos[v] - a.apexBind;
                    float d2 = dv.dot(dv);
                    if (d2 > kMaxRadius * kMaxRadius) continue;
                    EffectorVert ev;
                    ev.mesh = inst.mesh;
                    ev.prim = static_cast<int>(pi);
                    ev.vert = static_cast<int>(v);
                    ev.d = std::sqrt(d2);
                    ev.toVert = dv;
                    ev.basePos = prim.pos[v];
                    ev.baseNrm = v < prim.normal.size() ? prim.normal[v] : Vec3{0, 1, 0};
                    a.verts.push_back(ev);
                }
            }
        }
        effAnchors_.push_back(std::move(a));
    }
}

// ---- vertex deforms (face editor: eyes / nose / mouth) ----

void ShapeController::addDeformParams(const std::string& configPath) {
    nlohmann::json rules;
    {
        std::ifstream f(configPath);
        if (f) {
            try { rules = nlohmann::json::parse(f); }
            catch (...) { rules = nlohmann::json::object(); }
        }
    }
    nlohmann::json builtin = nlohmann::json::parse(kBuiltinRules);
    if (!rules.contains("deformAnchors") && builtin.contains("deformAnchors"))
        rules["deformAnchors"] = builtin["deformAnchors"];
    if (!rules.contains("deformParams") && builtin.contains("deformParams"))
        rules["deformParams"] = builtin["deformParams"];
    if (!rules.contains("deformAnchors") || !rules.contains("deformParams")) return;

    buildDeformAnchors(rules["deformAnchors"]);
    addDeformParamsFromJson(rules["deformParams"]);
}

void ShapeController::buildDeformAnchors(const nlohmann::json& anchorsJson) {
    if (!model_ || !skeleton_ || !anchorsJson.is_object()) return;
    skeleton_->update(); // world = bind pose right after bind()
    constexpr float kMaxRadius = 0.09f; // hard cap of the affected region (m)

    auto materialMatches = [](const std::string& name, const std::vector<std::string>& pats) {
        for (const std::string& p : pats) {
            try {
                if (std::regex_search(name, std::regex(p))) return true;
            } catch (...) { /* bad regex: no match */ }
        }
        return false;
    };

    for (auto it = anchorsJson.begin(); it != anchorsJson.end(); ++it) {
        const nlohmann::json& spec = it.value();
        DeformAnchor a;
        a.id = it.key();
        bool havePos = false;

        if (spec.contains("bones") || spec.contains("patterns")) {
            // anchor = bind position of the named bone (e.g. eye bones)
            std::vector<std::string> names, patterns;
            readNamesPatterns(spec, names, patterns);
            std::vector<int> bones = matchBones(names, patterns);
            if (!bones.empty()) {
                const Mat4& wm = skeleton_->world()[bones.front()];
                a.pos = {wm.m[12], wm.m[13], wm.m[14]};
                havePos = true;
            }
        } else if (spec.contains("material")) {
            // anchor = centroid (or extreme vertex along "extreme" dir) of the
            // verts of prims whose material matches (e.g. mouth / nose tip)
            std::vector<std::string> pats;
            const nlohmann::json& m = spec["material"];
            if (m.is_string())
                pats.push_back(m.get<std::string>());
            else
                for (const auto& s : m) pats.push_back(s.get<std::string>());
            Vec3 dir = readVec3(spec, "extreme", Vec3{0, 0, 0});
            const bool useExtreme = dir.length() > 0.5f;
            Vec3 sum{0, 0, 0}, nsum{0, 0, 0};
            size_t cnt = 0;
            float bestD = -1e30f;
            for (const Mesh& mesh : model_->meshes)
                for (const Primitive& prim : mesh.prims) {
                    if (prim.material < 0 ||
                        prim.material >= static_cast<int>(model_->materials.size()))
                        continue;
                    if (!materialMatches(model_->materials[prim.material].name, pats))
                        continue;
                    for (size_t v = 0; v < prim.pos.size(); ++v) {
                        if (useExtreme) {
                            float d = prim.pos[v].dot(dir);
                            if (d > bestD) {
                                bestD = d;
                                a.pos = prim.pos[v];
                                a.nrm = v < prim.normal.size() ? prim.normal[v]
                                                               : Vec3{0, 0, 1};
                            }
                        } else {
                            sum += prim.pos[v];
                            nsum += v < prim.normal.size() ? prim.normal[v] : Vec3{0, 0, 1};
                            ++cnt;
                        }
                    }
                }
            if (useExtreme)
                havePos = bestD > -1e29f;
            else if (cnt > 0) {
                a.pos = sum * (1.f / static_cast<float>(cnt));
                a.nrm = nsum.length() > 1e-7f ? nsum.normalized() : Vec3{0, 0, 1};
                havePos = true;
            }
        }
        if (!havePos) continue;

        // affected verts: everything within the hard radius (all meshes —
        // iris/white/highlight/eyeline/skin move together, nothing detaches),
        // except hair materials
        for (size_t mi = 0; mi < model_->meshes.size(); ++mi)
            for (size_t pi = 0; pi < model_->meshes[mi].prims.size(); ++pi) {
                const Primitive& prim = model_->meshes[mi].prims[pi];
                if (prim.material >= 0 &&
                    prim.material < static_cast<int>(model_->materials.size())) {
                    const std::string& mn = model_->materials[prim.material].name;
                    if (mn.find("HAIR") != std::string::npos ||
                        mn.find("Hair") != std::string::npos)
                        continue;
                }
                for (size_t v = 0; v < prim.pos.size(); ++v) {
                    Vec3 dv = prim.pos[v] - a.pos;
                    float d2 = dv.dot(dv);
                    if (d2 > kMaxRadius * kMaxRadius) continue;
                    DeformAnchor::V vv;
                    vv.mesh = static_cast<int>(mi);
                    vv.prim = static_cast<int>(pi);
                    vv.vert = static_cast<int>(v);
                    vv.d = std::sqrt(d2);
                    vv.toVert = dv;
                    a.verts.push_back(vv);
                }
            }
        if (!a.verts.empty()) deformAnchors_.push_back(std::move(a));
    }
}

void ShapeController::addDeformParamsFromJson(const nlohmann::json& arr) {
    auto anchorIdx = [&](const std::string& id) -> int {
        for (size_t i = 0; i < deformAnchors_.size(); ++i)
            if (deformAnchors_[i].id == id) return static_cast<int>(i);
        return -1;
    };
    for (const auto& r : arr) {
        ShapeParam p;
        p.type = ShapeParam::Type::VertexDeform;
        p.id = r.value("id", std::string{});
        p.name = r.value("name", p.id);
        p.group = r.value("group", std::string{"Лицо"});
        const std::string kind = r.value("kind", std::string{"translate"});
        p.deformKind = kind == "scale" ? ShapeParam::DeformKind::Scale
                     : kind == "protrude" ? ShapeParam::DeformKind::Protrude
                                          : ShapeParam::DeformKind::Translate;
        p.minS = r.value("min", 0.f);
        p.maxS = r.value("max", 1.f);
        p.deformRadius = r.value("radius", 0.03f);

        if (r.contains("anchors"))
            for (const auto& an : r["anchors"]) {
                int ai = anchorIdx(an.get<std::string>());
                if (ai >= 0) p.deformAnchors.push_back(ai);
            }
        if (r.contains("axis")) {
            const nlohmann::json& ax = r["axis"];
            const bool nested = ax.is_array() && !ax.empty() && ax[0].is_array();
            if (nested) {
                for (const auto& one : ax)
                    if (one.size() == 3)
                        p.deformAxes.push_back({one[0].get<float>(), one[1].get<float>(),
                                                one[2].get<float>()});
            } else if (ax.size() == 3) {
                p.deformAxes.push_back(
                    {ax[0].get<float>(), ax[1].get<float>(), ax[2].get<float>()});
            }
        }
        if (p.deformAxes.empty())
            p.deformAxes.push_back(p.deformKind == ShapeParam::DeformKind::Scale
                                       ? Vec3{1, 1, 1}
                                       : Vec3{0, 1, 0});
        if (p.deformAnchors.empty() || p.id.empty() || find(p.id)) continue;

        // neutral default: Scale -> magnitude 1; Translate/Protrude -> 0
        const float neutral = p.deformKind == ShapeParam::DeformKind::Scale ? 1.f : 0.f;
        float def = (p.maxS > p.minS)
                        ? std::clamp((neutral - p.minS) / (p.maxS - p.minS), 0.f, 1.f)
                        : 0.5f;
        p.defValue = r.value("def", def);
        p.value = p.defValue;
        params_.push_back(std::move(p));
    }
}

float ShapeController::effectorValue(ShapeParam::Effect kind) const {
    int idx = effParam_[static_cast<int>(kind)];
    if (idx < 0) {
        // sensible defaults when the param is absent (other models)
        return kind == ShapeParam::Effect::TipRadius ? 0.012f
             : kind == ShapeParam::Effect::AreolaRadius ? 0.026f
                                                        : 0.f;
    }
    const ShapeParam& p = params_[idx];
    return p.minS + (p.maxS - p.minS) * p.value;
}

void ShapeController::applyEffectorsToMesh() {
    if (!model_ || (effAnchors_.empty() && deformAnchors_.empty())) return;
    const float tipLen = effectorValue(ShapeParam::Effect::Protrude);
    const float tipR = std::max(effectorValue(ShapeParam::Effect::TipRadius), 1e-3f);
    const float aR = std::max(effectorValue(ShapeParam::Effect::AreolaRadius), 1e-3f);
    const float bulge = effectorValue(ShapeParam::Effect::Bulge);

    auto gauss = [](float d, float r) {
        float x = d / r;
        return std::exp(-x * x);
    };
    for (const EffectorAnchor& a : effAnchors_) {
        for (const EffectorVert& ev : a.verts) {
            Primitive& prim = model_->meshes[ev.mesh].prims[ev.prim];
            if (prim.blendedPos.size() != prim.pos.size() ||
                prim.blendedNormal.size() != prim.pos.size())
                continue;
            float h = 0.f, g = 0.f;
            if (tipLen > 0.f && ev.d < 2.5f * tipR) {
                float e = gauss(ev.d, tipR);
                h += tipLen * e;
                g += 2.f * tipLen * e / (tipR * tipR);
            }
            if (bulge > 0.f && ev.d < 2.5f * aR) {
                float e = gauss(ev.d, aR);
                h += bulge * e;
                g += 2.f * bulge * e / (aR * aR);
            }
            // idempotent: rewrite from the stored bind base
            prim.blendedPos[ev.vert] = ev.basePos + ev.baseNrm * h;
            // gradient-corrected normal so the dome actually shades
            prim.blendedNormal[ev.vert] = (ev.baseNrm + ev.toVert * g).normalized();
        }
    }

    // ---- region deforms (face editor: eyes / nose / mouth) ----
    // All prims inside the region (iris, white, highlight, eyeline, skin) get
    // the SAME transform — nothing detaches. Deltas are accumulated per
    // vertex and applied ONCE: morph prims get += on top of the fresh blend
    // (the blend resets blendedPos every pass), non-morph prims (body!) are
    // rewritten from pos — their blendedPos is NOT reset by the blend, so a
    // plain += would accumulate drift on every slider move (head peeling).
    std::map<std::pair<int, int>, std::vector<std::pair<int, Vec3>>> deltas;
    for (const ShapeParam& p : params_) {
        if (p.type != ShapeParam::Type::VertexDeform) continue;
        const float m = p.minS + (p.maxS - p.minS) * p.value;
        const bool neutral = (p.deformKind == ShapeParam::DeformKind::Scale) ? (m == 1.f)
                                                                             : (m == 0.f);
        if (neutral) continue;
        const float r = std::max(p.deformRadius, 1e-3f);
        for (size_t ai = 0; ai < p.deformAnchors.size(); ++ai) {
            const DeformAnchor& a = deformAnchors_[p.deformAnchors[ai]];
            const Vec3 axis = p.deformAxes.size() == 1 ? p.deformAxes[0] : p.deformAxes[ai];
            for (const DeformAnchor::V& v : a.verts) {
                float e = gauss(v.d, r);
                if (e < 1e-3f) continue;
                Vec3 delta{0, 0, 0};
                if (p.deformKind == ShapeParam::DeformKind::Translate) {
                    delta = axis * (m * e);
                } else if (p.deformKind == ShapeParam::DeformKind::Scale) {
                    delta = {v.toVert.x * axis.x * (m - 1.f) * e,
                             v.toVert.y * axis.y * (m - 1.f) * e,
                             v.toVert.z * axis.z * (m - 1.f) * e};
                } else { // Protrude
                    delta = a.nrm * (m * e);
                }
                deltas[{v.mesh, v.prim}].emplace_back(v.vert, delta);
            }
        }
    }
    for (auto& kv : deltas) {
        Primitive& prim = model_->meshes[kv.first.first].prims[kv.first.second];
        if (prim.blendedPos.size() != prim.pos.size()) continue;
        if (!prim.morphs.empty()) {
            for (const auto& vd : kv.second)
                prim.blendedPos[vd.first] = prim.blendedPos[vd.first] + vd.second;
        } else {
            std::map<int, Vec3> sum; // one rewrite per vert, all deltas merged
            for (const auto& vd : kv.second) sum[vd.first] = sum[vd.first] + vd.second;
            for (const auto& vd : sum)
                prim.blendedPos[vd.first] = prim.pos[vd.first] + vd.second;
        }
    }
}

Vec3 ShapeController::effectorAnchorWorld(int index) const {
    if (index < 0 || index >= static_cast<int>(effAnchors_.size()) || !model_ || !skeleton_)
        return {0, -1000.f, 0}; // far away: no tint
    const EffectorAnchor& a = effAnchors_[index];
    const Skin& skin = model_->skins[a.skin];
    Mat4 jm = skeleton_->world()[skin.joints[a.joint]] * skin.inverseBindMatrices[a.joint];
    return jm.transformPoint(a.apexBind);
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

    // 4) vertex effectors/deforms: any change needs a mesh re-stamp + re-upload
    for (int k = 0; k < 5; ++k) {
        float v = effectorValue(static_cast<ShapeParam::Effect>(k));
        if (v != effLast_[k]) {
            effLast_[k] = v;
            if (!effAnchors_.empty()) morphsDirty = true;
        }
    }
    if (deformLast_.size() != params_.size()) deformLast_.assign(params_.size(), -1.f);
    for (size_t i = 0; i < params_.size(); ++i) {
        if (params_[i].type != ShapeParam::Type::VertexDeform) continue;
        if (params_[i].value != deformLast_[i]) {
            deformLast_[i] = params_[i].value;
            if (!deformAnchors_.empty()) morphsDirty = true;
        }
    }
}

} // namespace ce
