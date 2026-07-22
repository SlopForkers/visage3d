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
#include <regex>
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
    buildArmJointFlags();
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

Vec3 mul(const Mat3& m, const Vec3& v) {
    return {m.m[0] * v.x + m.m[3] * v.y + m.m[6] * v.z,
            m.m[1] * v.x + m.m[4] * v.y + m.m[7] * v.z,
            m.m[2] * v.x + m.m[5] * v.y + m.m[8] * v.z};
}

// value at the given fraction of a sorted-by-nth_element copy (percentile)
float percentile(std::vector<float>& v, float frac) {
    if (v.empty()) return 0.f;
    size_t k = static_cast<size_t>(frac * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// Single place where a fitted vertex is composed. The reference surface is
// lerped between the real body surface (looseness=0, tight/anatomical) and
// the drape envelope (looseness=1, concavities bridged like fabric). Then:
// hard floor at ref+padding and optional shrink-wrap ("magnet") towards it.
// At looseness=0 this is exactly the historical clamp+shrink behavior.
Vec3 composeFitVert(const Vec3& base, const Vec3& target, const Vec3& dir,
                    const Vec3& drapeTarget, const Vec3& drapeDir, float looseness,
                    float padding, float shrink) {
    Vec3 ref = target;
    Vec3 rn = dir;
    if (looseness > 0.f) {
        ref = target + (drapeTarget - target) * looseness;
        rn = (dir + (drapeDir - dir) * looseness).normalized();
    }
    Vec3 surf = ref + rn * padding;
    float sd = (base - ref).dot(rn);
    Vec3 c = sd < padding ? surf : base;
    return c * (1.f - shrink) + surf * shrink;
}

// adjacency (CSR) from triangle indices
void buildAdjacency(const std::vector<uint32_t>& indices, size_t nv,
                    std::vector<uint32_t>& off, std::vector<uint32_t>& idx) {
    std::vector<std::vector<uint32_t>> tmp(nv);
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        uint32_t a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        tmp[a].push_back(b); tmp[a].push_back(c);
        tmp[b].push_back(a); tmp[b].push_back(c);
        tmp[c].push_back(a); tmp[c].push_back(b);
    }
    off.assign(nv + 1, 0);
    for (size_t v = 0; v < nv; ++v) {
        std::sort(tmp[v].begin(), tmp[v].end());
        tmp[v].erase(std::unique(tmp[v].begin(), tmp[v].end()), tmp[v].end());
        off[v + 1] = off[v] + static_cast<uint32_t>(tmp[v].size());
    }
    idx.resize(off[nv]);
    for (size_t v = 0; v < nv; ++v)
        std::copy(tmp[v].begin(), tmp[v].end(), idx.begin() + off[v]);
}

} // namespace

// Drape envelope: Laplacian smoothing of the body surface fills concavities
// (cleavage, underbust fold). Convex regions would shrink — each point is
// then pushed back along its original normal, but never BELOW the original
// surface (concave points keep the fill). The result is a fabric-like
// envelope that still contains the whole body (no skin poke-through).
void ClothingManager::smoothCloud(std::vector<BodyPoint>& cloud,
                                  const std::vector<uint32_t>& off,
                                  const std::vector<uint32_t>& idx, int iters, float lambda) {
    size_t nv = cloud.size();
    if (nv == 0 || off.size() != nv + 1) return;
    std::vector<Vec3> orig(nv), nrm(nv), cur(nv), next(nv);
    for (size_t v = 0; v < nv; ++v) {
        orig[v] = cloud[v].pos;
        nrm[v] = cloud[v].nrm;
        cur[v] = cloud[v].pos;
    }
    for (int it = 0; it < iters; ++it) {
        for (size_t v = 0; v < nv; ++v) {
            uint32_t b = off[v], e = off[v + 1];
            if (b == e) { next[v] = cur[v]; continue; }
            Vec3 avg{0, 0, 0};
            for (uint32_t i = b; i < e; ++i) avg += cur[idx[i]];
            avg = avg * (1.f / static_cast<float>(e - b));
            next[v] = cur[v] + (avg - cur[v]) * lambda;
        }
        std::swap(cur, next);
    }
    for (size_t v = 0; v < nv; ++v) {
        float back = (orig[v] - cur[v]).dot(nrm[v]);
        if (back > 0.f) cur[v] = cur[v] + nrm[v] * back; // restore convex parts
        cloud[v].pos = cur[v];
    }
}

struct ClothingManager::CloudGrid {
    float cell = 0.05f;
    std::unordered_multimap<GridKey, int, GridKeyHash> map;
};

ClothingManager::ClothingManager() = default;  // CloudGrid complete here
ClothingManager::~ClothingManager() = default;

// ---- body point cloud (cached, shared by all clothing items) ----

void ClothingManager::buildArmJointFlags() {
    armJoints_.clear();
    if (!body_ || bodySkinIndex_ < 0 || bodySkinIndex_ >= static_cast<int>(body_->skins.size()))
        return;
    // mark node subtrees rooted at arm-related bones
    std::vector<char> nodeArm(body_->nodes.size(), 0);
    std::regex pat("(arm|shoulder|hand|wrist|elbow|finger|thumb|clavicle)",
                   std::regex_constants::icase);
    std::function<void(int, bool)> mark = [&](int i, bool under) {
        bool a = under;
        try {
            a = a || std::regex_search(body_->nodes[i].name, pat);
        } catch (...) { /* keep parent flag */ }
        nodeArm[i] = a ? 1 : 0;
        for (int c : body_->nodes[i].children) mark(c, a);
    };
    for (size_t i = 0; i < body_->nodes.size(); ++i)
        if (body_->nodes[i].parent < 0) mark(static_cast<int>(i), false);

    const Skin& skin = body_->skins[bodySkinIndex_];
    armJoints_.resize(skin.joints.size());
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        int n = skin.joints[j];
        armJoints_[j] = (n >= 0 && n < static_cast<int>(nodeArm.size())) ? nodeArm[n] != 0
                                                                         : false;
    }
}

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

    // skin matrices in the CURRENT pose (so clothing fits the current shape).
    // Per-joint normal matrices are precomputed once (linear blend of them is
    // the standard approximation) instead of a per-vertex 3x3 inverse.
    std::vector<Mat4> jm(nj);
    std::vector<Mat3> jn(nj);
    for (size_t i = 0; i < nj; ++i) {
        jm[i] = skeleton_->world()[skin.joints[i]] * skin.inverseBindMatrices[i];
        jn[i] = jm[i].upper3x3().inverted().transposed();
    }

    size_t reserve = 0;
    for (const MeshInstance& inst : body_->meshInstances)
        if (inst.skin == bodySkinIndex_)
            for (const Primitive& prim : body_->meshes[inst.mesh].prims)
                reserve += prim.pos.size();
    cloud_.reserve(reserve);
    bodyAdjOff_.assign(1, 0);
    bodyAdjIdx_.clear();
    bodyAdjIdx_.reserve(reserve * 6);

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
            const std::vector<Vec3>& nrm =
                prim.blendedNormal.empty() ? prim.normal : prim.blendedNormal;
            const bool hasNrm = nrm.size() == pos.size();
            const size_t cloudBase = cloud_.size();
            for (size_t v = 0; v < pos.size(); ++v) {
                BodyPoint bp;
                Mat4 skinM;
                for (float& f : skinM.m) f = 0.f;
                const uint16_t* j4 = &prim.joints[v * 4];
                const float* w4 = &prim.weights[v].x;
                int dominant = 0;
                float domW = -1.f;
                for (int k = 0; k < 4; ++k) {
                    bp.joints[k] = j4[k];
                    bp.weights[k] = w4[k];
                    if (j4[k] < nj && w4[k] > 0.f) {
                        for (int e = 0; e < 16; ++e) skinM.m[e] += jm[j4[k]].m[e] * w4[k];
                        if (w4[k] > domW) { domW = w4[k]; dominant = j4[k]; }
                    }
                }
                bp.pos = skinM.transformPoint(pos[v]);
                if (hasNrm) {
                    Vec3 na{0, 0, 0};
                    for (int k = 0; k < 4; ++k)
                        if (j4[k] < nj && w4[k] > 0.f) na += mul(jn[j4[k]], nrm[v]) * w4[k];
                    bp.nrm = na.length() > 1e-7f ? na.normalized() : Vec3{0, 1, 0};
                } else {
                    bp.nrm = {0, 1, 0};
                }
                bp.arm = dominant < static_cast<int>(armJoints_.size()) && armJoints_[dominant];
                cloud_.push_back(bp);
            }
            // per-prim adjacency, remapped into the global cloud indexing
            std::vector<uint32_t> pOff, pIdx;
            buildAdjacency(prim.indices, pos.size(), pOff, pIdx);
            for (size_t v = 0; v < pos.size(); ++v)
                bodyAdjOff_.push_back(bodyAdjOff_.back() + (pOff[v + 1] - pOff[v]));
            for (uint32_t j : pIdx) bodyAdjIdx_.push_back(static_cast<uint32_t>(cloudBase) + j);
        }
    }

    auto buildGrid = [](const std::vector<BodyPoint>& cloud) {
        auto g = std::make_unique<CloudGrid>();
        const float cell = g->cell;
        g->map.reserve(cloud.size() * 2);
        for (size_t i = 0; i < cloud.size(); ++i) {
            const Vec3& p = cloud[i].pos;
            g->map.insert({{static_cast<int>(std::floor(p.x / cell)),
                            static_cast<int>(std::floor(p.y / cell)),
                            static_cast<int>(std::floor(p.z / cell))},
                           static_cast<int>(i)});
        }
        return g;
    };

    // spatial hash over the cloud
    grid_ = buildGrid(cloud_);

    // drape envelope (concavities filled, convex parts restored) + its grid
    drapeCloud_ = cloud_;
    smoothCloud(drapeCloud_, bodyAdjOff_, bodyAdjIdx_, 30, 0.6f);
    drapeGrid_ = buildGrid(drapeCloud_);
    return cloud_;
}

int ClothingManager::knnCloud(const Vec3& p, int k, int* outIdx, float* outDistSq,
                              const std::vector<BodyPoint>& cloud,
                              const CloudGrid& grid) const {
    const CloudGrid& g = grid;
    const float cell = g.cell;
    const int cx = static_cast<int>(std::floor(p.x / cell));
    const int cy = static_cast<int>(std::floor(p.y / cell));
    const int cz = static_cast<int>(std::floor(p.z / cell));

    int found = 0;
    float worst = FLT_MAX; // largest distance^2 among the current top-k

    auto consider = [&](int i) {
        Vec3 d = cloud[i].pos - p;
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
    for (size_t i = 0; i < cloud.size(); ++i) consider(static_cast<int>(i));
    return found;
}

float ClothingManager::bodyWidthInBand(float y0, float y1) const {
    float cx, cz;
    bodyAxisCenter(cx, cz);
    std::vector<float> radii;
    for (const BodyPoint& bp : cloud_) {
        if (bp.arm) continue; // T-pose arms would dominate the chest band
        if (bp.pos.y < y0 || bp.pos.y > y1) continue;
        float dx = bp.pos.x - cx, dz = bp.pos.z - cz;
        radii.push_back(dx * dx + dz * dz);
    }
    if (radii.size() < 16) return 0.f;
    float r75 = std::sqrt(percentile(radii, 0.75f));
    return 2.f * r75;
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

bool ClothingManager::transferWeights(ClothingItem& item) {
    const std::vector<BodyPoint>& cloud = bodyCloud();
    if (cloud.empty()) return false;

    const Skin& skin = body_->skins[bodySkinIndex_];
    const int nj = static_cast<int>(skin.joints.size());

    item.fits.assign(item.model.meshes.size(), {});

    for (size_t mi = 0; mi < item.model.meshes.size(); ++mi) {
        Mesh& mesh = item.model.meshes[mi];
        item.fits[mi].resize(mesh.prims.size());
        for (size_t pi = 0; pi < mesh.prims.size(); ++pi) {
            Primitive& prim = mesh.prims[pi];
            ClothingItem::PrimFit& fit = item.fits[mi][pi];
            size_t nv = prim.pos.size();
            prim.joints.assign(nv * 4, 0);
            prim.weights.assign(nv, Vec4{0, 0, 0, 0});
            prim.blendedPos.resize(nv);
            if (prim.blendedNormal.size() != nv) prim.blendedNormal = prim.normal;
            fit.basePos.resize(nv);
            fit.pushDir.resize(nv);
            fit.targetPos.resize(nv);
            fit.drapePos.resize(nv);
            fit.drapeDir.resize(nv);

            const Mat4 fitM = item.fitMatrix();
            const Quat fitR = item.fitRot;
            // per-vertex worker (thread-safe: writes only own slot)
            auto process = [&](size_t v) {
                Vec3 cp = fitM.transformPoint(prim.pos[v]);

                constexpr int K = 4;
                int idx[K];
                float dsq[K];
                int found = knnBody(cp, K, idx, dsq);

                // accumulate per-joint weights with inverse-square falloff
                float acc[128] = {}; // nj <= 80 in practice
                Vec3 nrmSum{0, 0, 0};
                for (int q = 0; q < found; ++q) {
                    float w = 1.f / (dsq[q] + 1e-8f);
                    const BodyPoint& bp = cloud[idx[q]];
                    for (int k = 0; k < 4; ++k)
                        if (bp.joints[k] < nj && bp.joints[k] < 128)
                            acc[bp.joints[k]] += bp.weights[k] * w;
                    nrmSum += bp.nrm * w;
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

                // nearest single body point (shrink target)
                int nearest = 0;
                for (int q = 1; q < found; ++q)
                    if (dsq[q] < dsq[nearest]) nearest = q;
                Vec3 target = cloud[idx[nearest]].pos;

                // padding direction: smoothed body surface normal (true outward,
                // works even when the clothing vertex sits inside the body)
                Vec3 dir{0, 1, 0};
                if (nrmSum.length() > 1e-6f)
                    dir = nrmSum.normalized();
                else if (v < prim.normal.size())
                    dir = fitR.rotate(prim.normal[v]);

                // nearest point on the drape envelope (+ smoothed normal)
                {
                    int di[4];
                    float dd[4];
                    int dfound = knnCloud(cp, 4, di, dd, drapeCloud_, *drapeGrid_);
                    int dn = 0;
                    for (int q = 1; q < dfound; ++q)
                        if (dd[q] < dd[dn]) dn = q;
                    fit.drapePos[v] = drapeCloud_[di[dn]].pos;
                    Vec3 dns{0, 0, 0};
                    for (int q = 0; q < dfound; ++q)
                        dns += drapeCloud_[di[q]].nrm * (1.f / (dd[q] + 1e-8f));
                    fit.drapeDir[v] = dns.length() > 1e-6f ? dns.normalized() : dir;
                }

                fit.basePos[v] = cp;
                fit.pushDir[v] = dir;
                fit.targetPos[v] = target;
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

            for (size_t v = 0; v < nv; ++v)
                prim.blendedPos[v] =
                    composeFitVert(fit.basePos[v], fit.targetPos[v], fit.pushDir[v],
                                   fit.drapePos[v], fit.drapeDir[v], item.looseness,
                                   item.padding, item.shrink);
        }
    }
    return true;
}

void ClothingManager::applyPadding(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    if (item.fits.size() != item.model.meshes.size()) return; // no transfer yet
    for (size_t mi = 0; mi < item.model.meshes.size(); ++mi)
        for (size_t pi = 0; pi < item.model.meshes[mi].prims.size(); ++pi) {
            if (pi >= item.fits[mi].size()) break;
            Primitive& prim = item.model.meshes[mi].prims[pi];
            const ClothingItem::PrimFit& fit = item.fits[mi][pi];
            if (fit.targetPos.size() != prim.blendedPos.size()) continue;
            const bool hasDrape = fit.drapePos.size() == prim.blendedPos.size() &&
                                  fit.drapeDir.size() == prim.blendedPos.size();
            for (size_t v = 0; v < prim.blendedPos.size(); ++v)
                prim.blendedPos[v] = composeFitVert(
                    fit.basePos[v], fit.targetPos[v], fit.pushDir[v],
                    hasDrape ? fit.drapePos[v] : fit.targetPos[v],
                    hasDrape ? fit.drapeDir[v] : fit.pushDir[v],
                    hasDrape ? item.looseness : 0.f, item.padding, item.shrink);
        }
}

// Cheap fit update for interactive dragging (gizmo / fit sliders):
// transforms raw vertices with the CURRENT fit matrix and clamps against the
// stored (possibly stale) body-surface + envelope targets. Call refit() on
// release to re-resolve them.
void ClothingManager::applyFit(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    Mat4 m = item.fitMatrix();
    const Quat r = item.fitRot;
    const bool haveFits = item.fits.size() == item.model.meshes.size();
    for (size_t mi = 0; mi < item.model.meshes.size(); ++mi)
        for (size_t pi = 0; pi < item.model.meshes[mi].prims.size(); ++pi) {
            Primitive& prim = item.model.meshes[mi].prims[pi];
            const bool clamp = haveFits && pi < item.fits[mi].size() &&
                               item.fits[mi][pi].targetPos.size() == prim.pos.size() &&
                               item.fits[mi][pi].drapePos.size() == prim.pos.size();
            const ClothingItem::PrimFit* fit = clamp ? &item.fits[mi][pi] : nullptr;
            if (prim.blendedNormal.size() != prim.pos.size()) prim.blendedNormal = prim.normal;
            for (size_t v = 0; v < prim.pos.size(); ++v) {
                Vec3 cp = m.transformPoint(prim.pos[v]);
                Vec3 out = cp;
                if (fit)
                    out = composeFitVert(cp, fit->targetPos[v], fit->pushDir[v],
                                         fit->drapePos[v], fit->drapeDir[v], item.looseness,
                                         item.padding, item.shrink);
                prim.blendedPos[v] = out;
                if (v < prim.normal.size())
                    prim.blendedNormal[v] = r.rotate(prim.normal[v]);
            }
        }
}

void ClothingManager::refit(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.weightsReady = transferWeights(item);
}

int ClothingManager::add(const std::string& path, std::string& err, bool deferFit) {
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

    // raw bbox (gizmo pivot + size-to-body reference)
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
    if (!deferFit) {
        applyType(idx, items_[idx].type, true); // anchor + size-to-body
        refit(idx);
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
// cheap and tight enough for anchoring / width matching).
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

void ClothingManager::applyType(int index, const std::string& typeId, bool scaleToBody) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.type = typeId;
    item.slot = slotForType(typeId);
    const ClothTypePreset* preset = typePreset(typeId);
    if (!preset || typeId == "auto" || typeId == "accessory")
        return; // authored placement kept as-is

    const std::vector<BodyPoint>& cloud = bodyCloud();
    float cx, cz;
    bodyAxisCenter(cx, cz);

    auto anchor = [&](float scale, Vec3& outOff, Vec3& outMn, Vec3& outMx) {
        bboxUnderRS(item, scale, outMn, outMx);
        outOff.x = cx - (outMn.x + outMx.x) * 0.5f;
        outOff.z = cz - (outMn.z + outMx.z) * 0.5f;
        if (preset->bottomAlign)
            outOff.y = preset->yOffset - outMn.y;
        else
            outOff.y = preset->yOffset - (outMn.y + outMx.y) * 0.5f;
    };

    float scale = item.fitScale;
    if (scaleToBody && !cloud.empty()) {
        // Provisional anchor at the authored unit scale to find the world
        // height band; then match the garment width against the body width
        // measured in that band (top 30% of the garment — waistband / chest —
        // or the bottom part for shoes).
        Vec3 off, mn, mx;
        anchor(item.unitScale, off, mn, mx);
        float h = mx.y - mn.y;
        float y0, y1;
        if (preset->bottomAlign) {
            y0 = preset->yOffset;
            y1 = preset->yOffset + std::max(0.12f, 0.5f * h);
        } else if (typeId == "top" || typeId == "bra" || typeId == "dress") {
            // chest band (cups), not the straps at the top of the bbox
            float c = off.y + (mn.y + mx.y) * 0.5f;
            y0 = c - 0.15f * h;
            y1 = c + 0.15f * h;
        } else {
            // waistband area (top of the garment)
            y1 = off.y + mx.y;
            y0 = y1 - 0.3f * h;
        }
        float bodyW = bodyWidthInBand(y0, y1);
        // garment width: radial 65th percentile of its vertices inside the band
        float garmentW = 0.f;
        {
            Mat4 prov = Mat4::translation(off) * Mat4::rotation(item.fitRot) *
                        Mat4::scaling({item.unitScale, item.unitScale, item.unitScale});
            std::vector<float> radii;
            for (const Mesh& m : item.model.meshes)
                for (const Primitive& p : m.prims)
                    for (const Vec3& v : p.pos) {
                        Vec3 w = prov.transformPoint(v);
                        if (w.y < y0 || w.y > y1) continue;
                        float dx = w.x - cx, dz = w.z - cz;
                        radii.push_back(dx * dx + dz * dz);
                    }
            if (radii.size() >= 16)
                garmentW = 2.f * std::sqrt(percentile(radii, 0.65f));
        }
        if (bodyW > 0.01f && garmentW > 0.01f) {
            float k = std::clamp(1.05f * bodyW / garmentW, 0.55f, 2.4f);
            scale = item.unitScale * k;
            item.fitScale = scale;
        }
    }

    Vec3 mn, mx;
    anchor(scale, item.fitOffset, mn, mx);
}

} // namespace ce
