#include "clothing/ClothingManager.h"
#include "model/GltfLoader.h"
#include "json.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <thread>
#include <unordered_map>

namespace ce {

void ClothingManager::bind(Model& body, Skeleton& skeleton) {
    body_ = &body;
    skeleton_ = &skeleton;

    // The body skin drives the mesh that spans the largest VERTICAL extent
    // (legs..head). Picking the largest mesh by vertex count is wrong: VRM
    // models often merge the face (+hair) into a dense mesh with its own
    // skin, and clothing must follow the BODY skeleton, not face bones.
    bodySkinIndex_ = -1;
    float bestHeight = -1.f;
    for (const MeshInstance& inst : body.meshInstances) {
        if (inst.skin < 0) continue;
        bool anyNonHair = false;
        Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (const Primitive& p : body.meshes[inst.mesh].prims) {
            if (p.material >= 0 && p.material < static_cast<int>(body.materials.size())) {
                const std::string& mn_ = body.materials[p.material].name;
                bool hair = mn_.find("HAIR") != std::string::npos ||
                            mn_.find("Hair") != std::string::npos;
                if (!hair) anyNonHair = true;
            } else {
                anyNonHair = true;
            }
            for (const Vec3& v : p.pos) {
                mn.y = std::min(mn.y, v.y);
                mx.y = std::max(mx.y, v.y);
            }
        }
        if (!anyNonHair) continue; // pure-hair instance
        float h = mx.y - mn.y;
        if (h > bestHeight) { bestHeight = h; bodySkinIndex_ = inst.skin; }
    }
    cloudDirty_ = true;
}

// ---- helpers ----

void ClothingManager::bakeNodeTransforms(Model& model) {
    // compute world transform per node, bake into vertex positions/normals
    std::vector<Mat4> world(model.nodes.size());
    std::function<void(int, const Mat4&)> visit = [&](int i, const Mat4& parent) {
        const Node& n = model.nodes[i];
        world[i] = parent * Mat4::trs(n.t, n.r, n.s);
        for (int c : n.children) visit(c, world[i]);
    };
    for (size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].parent < 0) visit(static_cast<int>(i), Mat4::identity());

    for (const MeshInstance& inst : model.meshInstances) {
        const Mat4& w = world[inst.node];
        Mat3 nrm = w.upper3x3().inverted().transposed();
        // mirrored transform (negative scale): flip the triangle winding or
        // the mesh renders inside-out
        const bool mirrored = w.upper3x3().det() < 0.f;
        for (Primitive& prim : model.meshes[inst.mesh].prims) {
            for (Vec3& p : prim.pos) p = w.transformPoint(p);
            for (Vec3& n : prim.normal) {
                Vec3 t{nrm.m[0] * n.x + nrm.m[3] * n.y + nrm.m[6] * n.z,
                       nrm.m[1] * n.x + nrm.m[4] * n.y + nrm.m[7] * n.z,
                       nrm.m[2] * n.x + nrm.m[5] * n.y + nrm.m[8] * n.z};
                n = t.normalized();
            }
            if (mirrored)
                for (size_t t = 0; t + 2 < prim.indices.size(); t += 3)
                    std::swap(prim.indices[t + 1], prim.indices[t + 2]);
        }
    }
}

float ClothingManager::autoUnitScale(const Model& model) const {
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const Mesh& m : model.meshes)
        for (const Primitive& p : m.prims)
            for (const Vec3& v : p.pos) {
                mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
                mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
            }
    Vec3 ext = mx - mn;
    float diag = ext.length();
    if (diag <= 0.f) return 1.f;
    // try unit scales; pick the one giving a plausible human-garment bbox diagonal
    const float candidates[] = {100.f, 10.f, 1.f, 0.1f, 0.01f, 0.001f, 0.0001f};
    for (float s : candidates) {
        float d = diag * s;
        if (d >= 0.25f && d <= 2.3f) return s;
    }
    // fallback: scale so the diagonal becomes 1m
    return 1.f / diag;
}

namespace {
struct GridKey {
    int x, y, z;
    bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridKeyHash {
    size_t operator()(const GridKey& k) const {
        return (static_cast<size_t>(k.x) * 73856093u) ^ (static_cast<size_t>(k.y) * 19349663u) ^
               (static_cast<size_t>(k.z) * 83492791u);
    }
};
} // namespace

struct ClothingManager::CloudGrid {
    float cell = 0.05f;
    std::unordered_multimap<GridKey, int, GridKeyHash> map;
};

ClothingManager::ClothingManager() = default;  // CloudGrid complete here
ClothingManager::~ClothingManager() = default;

// ---- body point cloud (cached, shared by all clothing items) ----

const std::vector<ClothingManager::BodyPoint>& ClothingManager::bodyCloud() {
    if (!cloudDirty_) return cloud_;
    cloud_.clear();
    grid_.reset();
    cloudDirty_ = false;
    if (bodySkinIndex_ < 0 || !body_) return cloud_;

    // world matrices may be stale (parameters changed outside the frame loop)
    skeleton_->update();
    const Skin& skin = body_->skins[bodySkinIndex_];
    const size_t nj = skin.joints.size();

    // skin matrices in the CURRENT pose (so binding matches the current shape)
    std::vector<Mat4> jm(nj);
    for (size_t i = 0; i < nj; ++i)
        jm[i] = skeleton_->world()[skin.joints[i]] * skin.inverseBindMatrices[i];

    size_t reserve = 0;
    for (const MeshInstance& inst : body_->meshInstances)
        if (inst.skin == bodySkinIndex_)
            for (const Primitive& prim : body_->meshes[inst.mesh].prims)
                reserve += prim.pos.size();
    cloud_.reserve(reserve);

    for (const MeshInstance& inst : body_->meshInstances) {
        if (inst.skin != bodySkinIndex_) continue;
        for (const Primitive& prim : body_->meshes[inst.mesh].prims) {
            // skip hair: clothing should not take weights from hair strands
            const Material* mat = nullptr;
            if (prim.material >= 0 && prim.material < static_cast<int>(body_->materials.size()))
                mat = &body_->materials[prim.material];
            if (mat && (mat->name.find("HAIR") != std::string::npos ||
                        mat->name.find("Hair") != std::string::npos))
                continue;

            const std::vector<Vec3>& pos = prim.blendedPos.empty() ? prim.pos : prim.blendedPos;
            for (size_t v = 0; v < pos.size(); ++v) {
                BodyPoint bp;
                Mat4 skinM;
                for (float& f : skinM.m) f = 0.f;
                const uint16_t* j4 = &prim.joints[v * 4];
                const float* w4 = &prim.weights[v].x;
                for (int k = 0; k < 4; ++k) {
                    bp.joints[k] = j4[k];
                    bp.weights[k] = w4[k];
                    if (j4[k] < nj && w4[k] > 0.f)
                        for (int e = 0; e < 16; ++e) skinM.m[e] += jm[j4[k]].m[e] * w4[k];
                }
                bp.pos = skinM.transformPoint(pos[v]);
                cloud_.push_back(bp);
            }
        }
    }

    // spatial hash over the cloud
    auto g = std::make_unique<CloudGrid>();
    const float cell = g->cell;
    g->map.reserve(cloud_.size() * 2);
    for (size_t i = 0; i < cloud_.size(); ++i) {
        const Vec3& p = cloud_[i].pos;
        g->map.insert({{static_cast<int>(std::floor(p.x / cell)),
                        static_cast<int>(std::floor(p.y / cell)),
                        static_cast<int>(std::floor(p.z / cell))},
                       static_cast<int>(i)});
    }
    grid_ = std::move(g);
    return cloud_;
}

int ClothingManager::knnCloud(const Vec3& p, int k, int* outIdx, float* outDistSq) const {
    const CloudGrid& g = *grid_;
    const float cell = g.cell;
    const int cx = static_cast<int>(std::floor(p.x / cell));
    const int cy = static_cast<int>(std::floor(p.y / cell));
    const int cz = static_cast<int>(std::floor(p.z / cell));

    int found = 0;
    float worst = FLT_MAX; // largest distance^2 among the current top-k

    auto consider = [&](int i) {
        Vec3 d = cloud_[i].pos - p;
        float d2 = d.dot(d);
        if (found < k) {
            outIdx[found] = i;
            outDistSq[found] = d2;
            ++found;
            if (found == k) {
                worst = 0.f;
                for (int q = 0; q < k; ++q) worst = std::max(worst, outDistSq[q]);
            }
        } else if (d2 < worst) {
            int farthest = 0;
            for (int q = 1; q < k; ++q)
                if (outDistSq[q] > outDistSq[farthest]) farthest = q;
            outIdx[farthest] = i;
            outDistSq[farthest] = d2;
            worst = 0.f;
            for (int q = 0; q < k; ++q) worst = std::max(worst, outDistSq[q]);
        }
    };

    // Expanding Chebyshev shells. After shell r every unvisited cell is
    // strictly farther than r*cell, so worst <= r*cell proves exactness.
    constexpr int kMaxRing = 12; // 60 cm at 5 cm cells; brute force beyond that
    for (int r = 0; r <= kMaxRing; ++r) {
        for (int dx = -r; dx <= r; ++dx)
            for (int dy = -r; dy <= r; ++dy)
                for (int dz = -r; dz <= r; ++dz) {
                    if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r)
                        continue; // only the shell
                    auto range = g.map.equal_range({cx + dx, cy + dy, cz + dz});
                    for (auto it = range.first; it != range.second; ++it) consider(it->second);
                }
        if (found == k) {
            float limit = r * cell;
            if (worst <= limit * limit) return found;
        }
    }

    // last resort: full scan (garment placed very far from the body)
    for (size_t i = 0; i < cloud_.size(); ++i) consider(static_cast<int>(i));
    return found;
}

void ClothingManager::bodyAxisCenter(float& cx, float& cz) const {
    cx = cz = 0.f;
    if (cloud_.empty()) return;
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const BodyPoint& bp : cloud_) {
        mn.x = std::min(mn.x, bp.pos.x); mx.x = std::max(mx.x, bp.pos.x);
        mn.z = std::min(mn.z, bp.pos.z); mx.z = std::max(mx.z, bp.pos.z);
    }
    cx = (mn.x + mx.x) * 0.5f;
    cz = (mn.z + mx.z) * 0.5f;
}

// Weight transfer: every garment vertex (at its current fit transform) takes
// an inverse-square blend of the K nearest body points' skin weights. The
// mesh shape itself is never modified — the garment follows the body purely
// through the shared skeleton (GPU skinning).
bool ClothingManager::transferWeights(ClothingItem& item) {
    const std::vector<BodyPoint>& cloud = bodyCloud();
    if (cloud.empty()) return false;

    const Skin& skin = body_->skins[bodySkinIndex_];
    const int nj = static_cast<int>(skin.joints.size());
    const Mat4 fitM = item.fitMatrix();
    const Quat fitR = item.fitRot;

    for (Mesh& mesh : item.model.meshes) {
        for (Primitive& prim : mesh.prims) {
            size_t nv = prim.pos.size();
            prim.joints.assign(nv * 4, 0);
            prim.weights.assign(nv, Vec4{0, 0, 0, 0});
            prim.blendedPos.resize(nv);
            if (prim.blendedNormal.size() != nv) prim.blendedNormal = prim.normal;

            // per-vertex worker (thread-safe: writes only own slot)
            auto process = [&](size_t v) {
                Vec3 cp = fitM.transformPoint(prim.pos[v]);

                float acc[128] = {}; // nj <= 80 in practice
                constexpr int K = 4;
                int idx[K];
                float dsq[K];
                int found = knnCloud(cp, K, idx, dsq);
                for (int q = 0; q < found; ++q) {
                    float w = 1.f / (dsq[q] + 1e-8f);
                    const BodyPoint& bp = cloud[idx[q]];
                    for (int k = 0; k < 4; ++k)
                        if (bp.joints[k] < nj && bp.joints[k] < 128)
                            acc[bp.joints[k]] += bp.weights[k] * w;
                }

                // pick top-4 joints, normalize
                int top[4] = {-1, -1, -1, -1};
                for (int k = 0; k < 4; ++k) {
                    float best = 0.f;
                    for (int j = 0; j < nj && j < 128; ++j) {
                        bool used = false;
                        for (int q = 0; q < k; ++q) used = used || top[q] == j;
                        if (!used && acc[j] > best) { best = acc[j]; top[k] = j; }
                    }
                    if (best <= 0.f && k > 0) { top[k] = top[0]; }
                }
                float tot = 0.f;
                for (int k = 0; k < 4; ++k) tot += acc[top[k] < 0 ? 0 : top[k]];
                if (tot <= 0.f) tot = 1.f;
                Vec4 wout{0, 0, 0, 0};
                float* wp = &wout.x;
                for (int k = 0; k < 4; ++k) {
                    prim.joints[v * 4 + k] = static_cast<uint16_t>(top[k] < 0 ? 0 : top[k]);
                    wp[k] = acc[top[k] < 0 ? 0 : top[k]] / tot;
                }
                prim.weights[v] = wout;

                prim.blendedPos[v] = cp;
                // normals follow the fit rotation (uniform scale keeps direction)
                if (v < prim.normal.size())
                    prim.blendedNormal[v] = fitR.rotate(prim.normal[v]);
            };

            // parallel chunks for large prims
            unsigned nt = nv > 4000 ? std::min(8u, std::thread::hardware_concurrency()) : 1;
            if (nt <= 1) {
                for (size_t v = 0; v < nv; ++v) process(v);
            } else {
                std::vector<std::thread> pool;
                size_t chunk = (nv + nt - 1) / nt;
                for (unsigned t = 0; t < nt; ++t) {
                    size_t begin = t * chunk;
                    size_t end = std::min(nv, begin + chunk);
                    if (begin >= end) break;
                    pool.emplace_back([&, begin, end] {
                        for (size_t v = begin; v < end; ++v) process(v);
                    });
                }
                for (auto& th : pool) th.join();
            }
        }
    }
    return true;
}

void ClothingManager::rebind(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.weightsReady = transferWeights(item);
}

// Cheap transform update for interactive dragging (gizmo / sliders):
// re-transforms vertices with the CURRENT fit matrix. Weights stay as-is;
// call rebind() on release to re-resolve them at the new position.
void ClothingManager::applyFit(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    Mat4 m = item.fitMatrix();
    const Quat r = item.fitRot;
    for (Mesh& mesh : item.model.meshes)
        for (Primitive& prim : mesh.prims) {
            size_t nv = prim.pos.size();
            prim.blendedPos.resize(nv);
            if (prim.blendedNormal.size() != nv) prim.blendedNormal = prim.normal;
            for (size_t v = 0; v < nv; ++v) {
                prim.blendedPos[v] = m.transformPoint(prim.pos[v]);
                if (v < prim.normal.size())
                    prim.blendedNormal[v] = r.rotate(prim.normal[v]);
            }
        }
}

int ClothingManager::add(const std::string& path, std::string& err, bool deferBind) {
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto ms = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    };
    auto t0 = now();
    Model model;
    if (!GltfLoader::load(path, model, err)) return -1;

    bakeNodeTransforms(model);

    ClothingItem item;
    item.name = model.fileName;
    item.path = path;
    item.model = std::move(model);
    item.fitScale = autoUnitScale(item.model);
    item.unitScale = item.fitScale;

    // raw bbox (gizmo pivot + anchor reference)
    {
        Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (const Mesh& m : item.model.meshes)
            for (const Primitive& p : m.prims)
                for (const Vec3& v : p.pos) {
                    mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y);
                    mn.z = std::min(mn.z, v.z);
                    mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y);
                    mx.z = std::max(mx.z, v.z);
                }
        item.rawMin = mn;
        item.rawMax = mx;
        item.rawCenter = (mn + mx) * 0.5f;
    }

    // type from the file path (Sketchfab exports are all "scene.gltf" —
    // keywords live in the dir name) and the equipment slot it implies
    item.type = detectType(item.path);
    item.slot = slotForType(item.type);

    items_.push_back(std::move(item));
    int idx = static_cast<int>(items_.size()) - 1;
    if (onItemsChanged) onItemsChanged(); // vector may have reallocated
    auto t1 = now();
    if (!deferBind) {
        applyType(idx, items_[idx].type); // type anchor (position only)
        rebind(idx);
    }
    auto t2 = now();
    std::fprintf(stderr, "  clothing timings: load %lldms, place+transfer %lldms\n",
                 ms(t0, t1), ms(t1, t2));
    const ClothingItem& it = items_[idx];
    std::fprintf(stderr,
                 "  fit: unitScale=%.4g scale=%.4g offset=(%.3f %.3f %.3f) type=%s slot=%s "
                 "weights=%d\n",
                 it.unitScale, it.fitScale, it.fitOffset.x, it.fitOffset.y, it.fitOffset.z,
                 it.type.c_str(), it.slot.c_str(), it.weightsReady ? 1 : 0);
    return idx;
}

void ClothingManager::remove(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_.erase(items_.begin() + index);
    if (onItemsChanged) onItemsChanged(); // elements shifted
}

void ClothingManager::clear() {
    items_.clear();
    if (onItemsChanged) onItemsChanged();
}

// ---- clothing types with anchor offsets ----

namespace {
const char* kBuiltinTypes = R"json({
  "types": [
    { "id": "auto",    "name": "Авто (как сшито)", "yOffset": 0.0,  "bottomAlign": false },
    { "id": "panties", "name": "Трусы",            "yOffset": 0.92, "bottomAlign": false },
    { "id": "shorts",  "name": "Шорты",            "yOffset": 0.8,  "bottomAlign": false },
    { "id": "pants",   "name": "Штаны / брюки",    "yOffset": 0.52, "bottomAlign": false },
    { "id": "skirt",   "name": "Юбка",             "yOffset": 0.8,  "bottomAlign": false },
    { "id": "top",     "name": "Топ / футболка",   "yOffset": 1.22, "bottomAlign": false },
    { "id": "bra",     "name": "Лиф / бюстгальтер","yOffset": 1.26, "bottomAlign": false },
    { "id": "dress",   "name": "Платье",           "yOffset": 0.95, "bottomAlign": false },
    { "id": "shoes",   "name": "Обувь",            "yOffset": 0.0,  "bottomAlign": true },
    { "id": "head",    "name": "Голова / волосы",  "yOffset": 1.45, "bottomAlign": false },
    { "id": "accessory", "name": "Аксессуар",      "yOffset": 0.0,  "bottomAlign": false }
  ]
})json";
} // namespace

void ClothingManager::loadTypes(const std::string& configPath) {
    types_.clear();
    nlohmann::json j;
    {
        std::ifstream f(configPath);
        if (f) {
            try { j = nlohmann::json::parse(f); }
            catch (...) { j = nlohmann::json::object(); }
        }
    }
    if (!j.contains("types")) j = nlohmann::json::parse(kBuiltinTypes);
    for (const auto& t : j["types"]) {
        ClothTypePreset p;
        p.id = t.value("id", std::string{});
        p.name = t.value("name", p.id);
        p.yOffset = t.value("yOffset", 0.9f);
        p.bottomAlign = t.value("bottomAlign", false);
        if (!p.id.empty()) types_.push_back(std::move(p));
    }

    slots_ = {{"top", "Верх"},       {"bottom", "Низ"},      {"dress", "Платье"},
              {"shoes", "Обувь"},    {"hair", "Причёска"},   {"accessory", "Аксессуар"}};
}

bool ClothingManager::saveTypes(const std::string& configPath) const {
    try {
        nlohmann::json j;
        j["types"] = nlohmann::json::array();
        for (const ClothTypePreset& p : types_) {
            nlohmann::json t;
            t["id"] = p.id;
            t["name"] = p.name;
            t["yOffset"] = p.yOffset;
            t["bottomAlign"] = p.bottomAlign;
            j["types"].push_back(std::move(t));
        }
        std::ofstream f(configPath);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

const ClothTypePreset* ClothingManager::typePreset(const std::string& typeId) const {
    for (const ClothTypePreset& p : types_)
        if (p.id == typeId) return &p;
    return nullptr;
}

std::string ClothingManager::slotForType(const std::string& typeId) {
    if (typeId == "top" || typeId == "bra") return "top";
    if (typeId == "panties" || typeId == "shorts" || typeId == "pants" || typeId == "skirt")
        return "bottom";
    if (typeId == "dress") return "dress";
    if (typeId == "shoes") return "shoes";
    if (typeId == "head") return "hair";
    if (typeId == "accessory") return "accessory";
    return {};
}

int ClothingManager::itemInSlot(const std::string& slotId) const {
    if (slotId.empty()) return -1;
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].slot == slotId) return static_cast<int>(i);
    return -1;
}

std::string ClothingManager::detectType(const std::string& fileName) {
    std::string n = fileName;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    auto hasAny = [&](std::initializer_list<const char*> ks) {
        for (const char* k : ks)
            if (n.find(k) != std::string::npos) return true;
        return false;
    };
    bool bottom = hasAny({"panties", "panty", "briefs", "underwear", "shorts", "pants",
                          "trousers", "jeans", "leggings", "skirt"});
    bool top = hasAny({"bra", "bikini", "lingerie", "hoodie", "sweater", "shirt", "top",
                       "blouse", "jacket", "coat", "tshirt", "t-shirt", "t_shirt",
                       "footbolka", "futbolka", "polo"});
    if (bottom && top) return "auto"; // multi-garment set: keep authored placement
    if (hasAny({"panties", "panty", "briefs", "underwear"})) return "panties";
    if (hasAny({"shorts"})) return "shorts";
    if (hasAny({"pants", "trousers", "jeans", "leggings"})) return "pants";
    if (hasAny({"dress"})) return "dress";
    if (hasAny({"skirt"})) return "skirt";
    if (hasAny({"bra", "bikini", "lingerie"})) return "bra";
    if (hasAny({"hoodie", "sweater", "shirt", "top", "blouse", "jacket", "coat",
                "tshirt", "t-shirt", "t_shirt", "footbolka", "futbolka", "polo"}))
        return "top";
    if (hasAny({"shoes", "boots", "sneakers", "heels"})) return "shoes";
    if (hasAny({"hair", "hat", "cap"})) return "head";
    if (hasAny({"necklace", "earring", "glasses", "sunglass", "watch", "bracelet", "jewel",
                "crown", "tiara", "ribbon", "backpack", "bag", "belt", "choker", "piercing",
                "accessory"}))
        return "accessory";
    return "auto";
}

namespace {
// Axis-aligned bbox of the raw mesh bbox under rot*scale (8 corner transform —
// cheap and tight enough for anchoring).
void bboxUnderRS(const ClothingItem& item, float scale, Vec3& outMn, Vec3& outMx) {
    Mat4 rs = Mat4::rotation(item.fitRot) * Mat4::scaling({scale, scale, scale});
    outMn = {1e30f, 1e30f, 1e30f};
    outMx = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        Vec3 corner{(c & 1) ? item.rawMax.x : item.rawMin.x,
                    (c & 2) ? item.rawMax.y : item.rawMin.y,
                    (c & 4) ? item.rawMax.z : item.rawMin.z};
        Vec3 w = rs.transformPoint(corner);
        outMn.x = std::min(outMn.x, w.x); outMx.x = std::max(outMx.x, w.x);
        outMn.y = std::min(outMn.y, w.y); outMx.y = std::max(outMx.y, w.y);
        outMn.z = std::min(outMn.z, w.z); outMx.z = std::max(outMx.z, w.z);
    }
}
} // namespace

void ClothingManager::applyType(int index, const std::string& typeId) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.type = typeId;
    item.slot = slotForType(typeId);
    const ClothTypePreset* preset = typePreset(typeId);
    if (!preset || typeId == "auto" || typeId == "accessory")
        return; // authored placement kept as-is

    float cx, cz;
    bodyAxisCenter(cx, cz);

    // anchor at the CURRENT scale — the user's size fit is never overridden
    Vec3 mn, mx;
    bboxUnderRS(item, item.fitScale, mn, mx);
    item.fitOffset.x = cx - (mn.x + mx.x) * 0.5f;
    item.fitOffset.z = cz - (mn.z + mx.z) * 0.5f;
    if (preset->bottomAlign)
        item.fitOffset.y = preset->yOffset - mn.y;
    else
        item.fitOffset.y = preset->yOffset - (mn.y + mx.y) * 0.5f;
}

} // namespace ce
