#include "clothing/ClothingManager.h"
#include "model/GltfLoader.h"
#include "json.hpp"

#include <algorithm>
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

    // find the skin of the largest skinned mesh instance (the body mesh)
    bodySkinIndex_ = -1;
    size_t best = 0;
    for (const MeshInstance& inst : body.meshInstances) {
        if (inst.skin < 0) continue;
        size_t verts = 0;
        for (const Primitive& p : body.meshes[inst.mesh].prims) verts += p.pos.size();
        if (verts > best) { best = verts; bodySkinIndex_ = inst.skin; }
    }
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
        for (Primitive& prim : model.meshes[inst.mesh].prims) {
            for (Vec3& p : prim.pos) p = w.transformPoint(p);
            for (Vec3& n : prim.normal) {
                Vec3 t{nrm.m[0] * n.x + nrm.m[3] * n.y + nrm.m[6] * n.z,
                       nrm.m[1] * n.x + nrm.m[4] * n.y + nrm.m[7] * n.z,
                       nrm.m[2] * n.x + nrm.m[5] * n.y + nrm.m[8] * n.z};
                n = t.normalized();
            }
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

std::vector<ClothingManager::BodyPoint> ClothingManager::buildBodyPointCloud() {
    std::vector<BodyPoint> cloud;
    if (bodySkinIndex_ < 0 || !body_) return cloud;
    // world matrices may be stale (parameters changed outside the frame loop)
    skeleton_->update();
    const Skin& skin = body_->skins[bodySkinIndex_];

    // skin matrices in the CURRENT pose (so clothing fits the current shape)
    std::vector<Mat4> jm(skin.joints.size());
    for (size_t i = 0; i < skin.joints.size(); ++i)
        jm[i] = skeleton_->world()[skin.joints[i]] * skin.inverseBindMatrices[i];

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
            for (size_t v = 0; v < pos.size(); ++v) {
                BodyPoint bp;
                Mat4 skinM;
                for (float& f : skinM.m) f = 0.f;
                const uint16_t* j4 = &prim.joints[v * 4];
                const float* w4 = &prim.weights[v].x;
                for (int k = 0; k < 4; ++k) {
                    bp.joints[k] = j4[k];
                    bp.weights[k] = w4[k];
                    if (j4[k] < jm.size() && w4[k] > 0.f)
                        for (int e = 0; e < 16; ++e) skinM.m[e] += jm[j4[k]].m[e] * w4[k];
                }
                bp.pos = skinM.transformPoint(pos[v]);
                if (v < nrm.size()) {
                    Mat3 r3 = skinM.upper3x3().inverted().transposed();
                    Vec3 n = nrm[v];
                    bp.nrm = Vec3{r3.m[0] * n.x + r3.m[3] * n.y + r3.m[6] * n.z,
                                  r3.m[1] * n.x + r3.m[4] * n.y + r3.m[7] * n.z,
                                  r3.m[2] * n.x + r3.m[5] * n.y + r3.m[8] * n.z}
                                 .normalized();
                } else {
                    bp.nrm = {0, 1, 0};
                }
                cloud.push_back(bp);
            }
        }
    }
    return cloud;
}

bool ClothingManager::transferWeights(ClothingItem& item) {
    std::vector<BodyPoint> cloud = buildBodyPointCloud();
    if (cloud.empty()) return false;

    // spatial hash over body points
    const float cell = 0.05f;
    std::unordered_multimap<GridKey, int, GridKeyHash> grid;
    for (size_t i = 0; i < cloud.size(); ++i) {
        const Vec3& p = cloud[i].pos;
        grid.insert({{static_cast<int>(std::floor(p.x / cell)),
                      static_cast<int>(std::floor(p.y / cell)),
                      static_cast<int>(std::floor(p.z / cell))},
                     static_cast<int>(i)});
    }

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

            const Mat4 fitM = item.fitMatrix();
            // per-vertex worker (thread-safe: writes only own slot)
            auto process = [&](size_t v) {
                Vec3 cp = fitM.transformPoint(prim.pos[v]);

                // k nearest body points: rings 1-2 of the hash grid, then brute
                // force over the whole cloud (cheaper than scanning far rings)
                constexpr int K = 4;
                int idx[K];
                float dist[K];
                int found = 0;
                int cx = static_cast<int>(std::floor(cp.x / cell));
                int cy = static_cast<int>(std::floor(cp.y / cell));
                int cz = static_cast<int>(std::floor(cp.z / cell));
                for (int ring = 1; ring <= 2; ++ring) {
                    for (int dx = -ring; dx <= ring; ++dx)
                        for (int dy = -ring; dy <= ring; ++dy)
                            for (int dz = -ring; dz <= ring; ++dz) {
                                if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring)
                                    continue; // only the shell
                                auto range = grid.equal_range({cx + dx, cy + dy, cz + dz});
                                for (auto it = range.first; it != range.second; ++it) {
                                    float d = (cloud[it->second].pos - cp).length();
                                    if (found < K) {
                                        idx[found] = it->second;
                                        dist[found] = d;
                                        ++found;
                                    } else {
                                        int farthest = 0;
                                        for (int q = 1; q < K; ++q)
                                            if (dist[q] > dist[farthest]) farthest = q;
                                        if (d < dist[farthest]) {
                                            idx[farthest] = it->second;
                                            dist[farthest] = d;
                                        }
                                    }
                                }
                            }
                }
                {
                    // verify with brute force: guarantees exact K nearest
                    bool needBrute = found < K;
                    if (!needBrute) {
                        float kth = 0.f;
                        for (int q = 0; q < K; ++q) kth = std::max(kth, dist[q]);
                        needBrute = kth > 2.f * cell; // ring 2 covers only 2 cells
                    }
                    if (needBrute) {
                        for (size_t q = 0; q < cloud.size(); ++q) {
                            float d = (cloud[q].pos - cp).length();
                            if (found < K) {
                                idx[found] = static_cast<int>(q);
                                dist[found] = d;
                                ++found;
                            } else {
                                int farthest = 0;
                                for (int t = 1; t < K; ++t)
                                    if (dist[t] > dist[farthest]) farthest = t;
                                if (d < dist[farthest]) {
                                    idx[farthest] = static_cast<int>(q);
                                    dist[farthest] = d;
                                }
                            }
                        }
                    }
                }

                // accumulate per-joint weights with inverse-square falloff
                float acc[128] = {}; // nj <= 80 in practice
                Vec3 nrmSum{0, 0, 0};
                for (int q = 0; q < found; ++q) {
                    float w = 1.f / (dist[q] * dist[q] + 1e-8f);
                    const BodyPoint& bp = cloud[idx[q]];
                    for (int k = 0; k < 4; ++k)
                        if (bp.joints[k] < nj) acc[bp.joints[k]] += bp.weights[k] * w;
                    nrmSum += bp.nrm * w;
                }
                // pick top-4 joints, normalize
                int top[4] = {-1, -1, -1, -1};
                for (int k = 0; k < 4; ++k) {
                    float best = 0.f;
                    for (int j = 0; j < nj; ++j) {
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
                    if (dist[q] < dist[nearest]) nearest = q;
                Vec3 target = cloud[idx[nearest]].pos;

                // padding direction: smoothed body surface normal (true outward,
                // works even when the clothing vertex sits inside the body)
                Vec3 dir{0, 1, 0};
                if (nrmSum.length() > 1e-6f)
                    dir = nrmSum.normalized();
                else if (v < prim.normal.size())
                    dir = prim.normal[v];

                fit.basePos[v] = cp;
                fit.pushDir[v] = dir;
                fit.targetPos[v] = target;
                // never leave the vertex inside the body: clamp to surface + padding
                // (signed distance along the body normal catches deep-inside verts too)
                Vec3 surfacePos = target + dir * item.padding;
                float sd = (cp - target).dot(dir);
                Vec3 base = sd < item.padding ? surfacePos : cp;
                prim.blendedPos[v] = base * (1.f - item.shrink) + surfacePos * item.shrink;
            };

            // parallel chunks for large prims
            unsigned nt = nv > 20000 ? std::min(8u, std::thread::hardware_concurrency()) : 1;
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

void ClothingManager::applyPadding(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    for (size_t mi = 0; mi < item.model.meshes.size(); ++mi)
        for (size_t pi = 0; pi < item.model.meshes[mi].prims.size(); ++pi) {
            Primitive& prim = item.model.meshes[mi].prims[pi];
            const ClothingItem::PrimFit& fit = item.fits[mi][pi];
            for (size_t v = 0; v < prim.blendedPos.size(); ++v) {
                Vec3 surfacePos = fit.targetPos[v] + fit.pushDir[v] * item.padding;
                float sd = (fit.basePos[v] - fit.targetPos[v]).dot(fit.pushDir[v]);
                Vec3 base = sd < item.padding ? surfacePos : fit.basePos[v];
                prim.blendedPos[v] = base * (1.f - item.shrink) + surfacePos * item.shrink;
            }
        }
}

// Cheap fit update for interactive dragging (gizmo / fit sliders):
// transforms raw vertices with the CURRENT fit matrix and clamps against the
// stored (possibly stale) body-surface targets. Call refit() on release.
void ClothingManager::applyFit(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    Mat4 m = item.fitMatrix();
    for (size_t mi = 0; mi < item.model.meshes.size(); ++mi)
        for (size_t pi = 0; pi < item.model.meshes[mi].prims.size(); ++pi) {
            Primitive& prim = item.model.meshes[mi].prims[pi];
            const ClothingItem::PrimFit& fit = item.fits[mi][pi];
            for (size_t v = 0; v < prim.blendedPos.size(); ++v) {
                Vec3 cp = m.transformPoint(prim.pos[v]);
                Vec3 surfacePos = fit.targetPos[v] + fit.pushDir[v] * item.padding;
                float sd = (cp - fit.targetPos[v]).dot(fit.pushDir[v]);
                Vec3 base = sd < item.padding ? surfacePos : cp;
                prim.blendedPos[v] = base * (1.f - item.shrink) + surfacePos * item.shrink;
            }
        }
}

void ClothingManager::refit(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.weightsReady = transferWeights(item);
}

// ---- auto fit (coordinate descent over scale + offset) ----

struct ClothingManager::CloudGrid {
    float cell;
    const std::vector<BodyPoint>* cloud;
    std::unordered_multimap<GridKey, int, GridKeyHash> map;
};

std::unique_ptr<ClothingManager::CloudGrid>
ClothingManager::makeGrid(const std::vector<BodyPoint>& cloud, float cell) {
    auto g = std::make_unique<CloudGrid>();
    g->cell = cell;
    g->cloud = &cloud;
    for (size_t i = 0; i < cloud.size(); ++i) {
        const Vec3& p = cloud[i].pos;
        g->map.insert({{static_cast<int>(std::floor(p.x / cell)),
                        static_cast<int>(std::floor(p.y / cell)),
                        static_cast<int>(std::floor(p.z / cell))},
                       static_cast<int>(i)});
    }
    return g;
}

float ClothingManager::distToCloud(const Vec3& p, const CloudGrid& grid) const {
    int cx = static_cast<int>(std::floor(p.x / grid.cell));
    int cy = static_cast<int>(std::floor(p.y / grid.cell));
    int cz = static_cast<int>(std::floor(p.z / grid.cell));
    for (int ring = 1; ring <= 4; ++ring) {
        float best = 1e30f;
        for (int dx = -ring; dx <= ring; ++dx)
            for (int dy = -ring; dy <= ring; ++dy)
                for (int dz = -ring; dz <= ring; ++dz) {
                    if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != ring) continue;
                    auto range = grid.map.equal_range({cx + dx, cy + dy, cz + dz});
                    for (auto it = range.first; it != range.second; ++it)
                        best = std::min(best, ((*grid.cloud)[it->second].pos - p).length());
                }
        if (best < 1e29f) return best;
    }
    return 0.5f; // capped penalty for far-away points
}

void ClothingManager::autoFitToBody(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || !body_) return;
    ClothingItem& item = items_[index];

    std::vector<BodyPoint> cloud = buildBodyPointCloud();
    if (cloud.empty()) return;
    auto grid = makeGrid(cloud, 0.04f);

    // sample clothing vertices (raw positions)
    std::vector<Vec3> samples;
    size_t total = 0;
    for (const Mesh& m : item.model.meshes)
        for (const Primitive& p : m.prims) total += p.pos.size();
    size_t stride = std::max<size_t>(1, total / 1200);
    for (const Mesh& m : item.model.meshes)
        for (const Primitive& p : m.prims)
            for (size_t v = 0; v < p.pos.size(); v += stride) samples.push_back(p.pos[v]);
    if (samples.empty()) return;

    // scaling pivot: bbox center in X/Z, bbox bottom in Y — resizing keeps the
    // garment's position and hem instead of shifting it around the origin
    const Mat4 fitM = item.fitMatrix();
    Vec3 pivot;
    {
        Vec3 pmn{1e30f, 1e30f, 1e30f}, pmx{-1e30f, -1e30f, -1e30f};
        for (const Vec3& s : samples) {
            Vec3 w = fitM.transformPoint(s);
            pmn.x = std::min(pmn.x, w.x); pmx.x = std::max(pmx.x, w.x);
            pmn.y = std::min(pmn.y, w.y); pmx.y = std::max(pmx.y, w.y);
            pmn.z = std::min(pmn.z, w.z); pmx.z = std::max(pmx.z, w.z);
        }
        pivot = {(pmn.x + pmx.x) * 0.5f, pmn.y, (pmn.z + pmx.z) * 0.5f};
    }

    // metric: mean distance with distances capped at 25cm — robust to baggy
    // garment regions (they contribute a constant, not a runaway gradient).
    // An inertia penalty keeps the garment near its authored placement unless
    // a clearly better fit exists (small garments otherwise "migrate" to the
    // wrong body region — panties fit hips, chest and armpit equally well).
    auto metric = [&](float scale, const Vec3& off, double& outMean) {
        double sum = 0.0;
        for (const Vec3& s : samples) {
            Vec3 w = fitM.transformPoint(s);
            Vec3 ws = pivot + (w - pivot) * scale + off;
            float d = distToCloud(ws, *grid);
            sum += std::min(d, 0.25f);
        }
        outMean = sum / samples.size();
        double penalty = 2.0 * off.length() + 0.5 * std::fabs(1.0 - scale);
        return -(outMean + penalty); // "higher is better" convention
    };

    // init: center X/Z only — garments are authored ground-aligned, keep authored Y
    Vec3 cmn{1e30f, 1e30f, 1e30f}, cmx{-1e30f, -1e30f, -1e30f};
    for (const Vec3& s : samples) {
        Vec3 w = fitM.transformPoint(s);
        cmn.x = std::min(cmn.x, w.x); cmx.x = std::max(cmx.x, w.x);
        cmn.z = std::min(cmn.z, w.z); cmx.z = std::max(cmx.z, w.z);
    }
    Vec3 bmn{1e30f, 1e30f, 1e30f}, bmx{-1e30f, -1e30f, -1e30f};
    for (const BodyPoint& bp : cloud) {
        bmn.x = std::min(bmn.x, bp.pos.x); bmx.x = std::max(bmx.x, bp.pos.x);
        bmn.z = std::min(bmn.z, bp.pos.z); bmx.z = std::max(bmx.z, bp.pos.z);
    }
    item.fitOffset.x += (bmn.x + bmx.x - cmn.x - cmx.x) * 0.5f;
    item.fitOffset.z += (bmn.z + bmx.z - cmn.z - cmx.z) * 0.5f;

    // greedy coordinate descent; bounded to avoid degenerate/runaway fits
    float scale = 1.f; // multiplicative delta around the pivot
    Vec3 off{0, 0, 0};
    double bestMean = 1e9;
    double best = metric(1.f, off, bestMean);
    const float offs[] = {-0.03f, -0.015f, 0.015f, 0.03f};
    const float scl[] = {-0.05f, -0.02f, 0.02f, 0.05f};
    for (int round = 0; round < 4; ++round) {
        bool improved = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (axis == 1) continue; // keep authored ground alignment in Y
            for (float d : offs) {
                Vec3 tryOff = off;
                (&tryOff.x)[axis] += d;
                if (tryOff.length() > 0.25f) continue; // stay near the init position
                double mean;
                double m = metric(scale, tryOff, mean);
                if (m > best) { best = m; bestMean = mean; off = tryOff; improved = true; }
            }
        }
        for (float d : scl) {
            float tryScale = std::clamp(scale * (1.f + d), 0.85f, 1.2f);
            double mean;
            double m = metric(tryScale, off, mean);
            if (m > best) { best = m; bestMean = mean; scale = tryScale; improved = true; }
        }
        if (!improved) break;
    }
    // w' = pivot + (w - pivot)*scale + off  <=>  uniform scale commutes, so:
    //   newFitScale = fitScale * scale
    //   newOffset   = pivot + (fitOffset - pivot) * scale + off
    //   rotation unchanged
    item.fitOffset = pivot + (item.fitOffset - pivot) * scale + off;
    item.fitScale *= scale;
}

int ClothingManager::add(const std::string& path, std::string& err) {
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

    // raw bbox center (gizmo pivot)
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
        item.rawCenter = (mn + mx) * 0.5f;
    }

    items_.push_back(std::move(item));
    int idx = static_cast<int>(items_.size()) - 1;
    auto t1 = now();
    autoFitToBody(idx); // coarse alignment before weight transfer
    // type anchor: if the file path reveals the garment type, snap it into place
    // (Sketchfab exports are all "scene.gltf" — keywords live in the dir name)
    {
        std::string detected = detectType(items_[idx].path);
        if (detected != "auto") applyType(idx, detected);
    }
    auto t2 = now();
    refit(idx);
    auto t3 = now();
    std::fprintf(stderr, "  clothing timings: load %lldms, autofit %lldms, transfer %lldms\n",
                 ms(t0, t1), ms(t1, t2), ms(t2, t3));
    const ClothingItem& it = items_[idx];
    std::fprintf(stderr,
                 "  fit: unitScale=%.4g scale=%.4g offset=(%.3f %.3f %.3f) weights=%d\n",
                 it.unitScale, it.fitScale, it.fitOffset.x, it.fitOffset.y, it.fitOffset.z,
                 it.weightsReady ? 1 : 0);
    return idx;
}

void ClothingManager::remove(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    items_.erase(items_.begin() + index);
}

void ClothingManager::clear() {
    items_.clear();
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
    { "id": "head",    "name": "Голова / волосы",  "yOffset": 1.45, "bottomAlign": false }
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

std::string ClothingManager::detectType(const std::string& fileName) const {
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
                       "blouse", "jacket", "coat"});
    if (bottom && top) return "auto"; // multi-garment set: keep authored placement
    if (hasAny({"panties", "panty", "briefs", "underwear"})) return "panties";
    if (hasAny({"shorts"})) return "shorts";
    if (hasAny({"pants", "trousers", "jeans", "leggings"})) return "pants";
    if (hasAny({"dress"})) return "dress";
    if (hasAny({"skirt"})) return "skirt";
    if (hasAny({"bra", "bikini", "lingerie"})) return "bra";
    if (hasAny({"hoodie", "sweater", "shirt", "top", "blouse", "jacket", "coat"})) return "top";
    if (hasAny({"shoes", "boots", "sneakers", "heels"})) return "shoes";
    if (hasAny({"hair", "hat", "cap"})) return "head";
    return "auto";
}

void ClothingManager::applyType(int index, const std::string& typeId) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    ClothingItem& item = items_[index];
    item.type = typeId;
    const ClothTypePreset* preset = typePreset(typeId);
    if (!preset || typeId == "auto") return;

    // garment bbox at the current fitScale+rotation (world = R*S*raw + fitOffset)
    Mat4 rs = item.fitMatrixNoTrans();
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const Mesh& m : item.model.meshes)
        for (const Primitive& p : m.prims)
            for (const Vec3& v : p.pos) {
                Vec3 w = rs.transformPoint(v);
                mn.x = std::min(mn.x, w.x); mx.x = std::max(mx.x, w.x);
                mn.y = std::min(mn.y, w.y); mx.y = std::max(mx.y, w.y);
                mn.z = std::min(mn.z, w.z); mx.z = std::max(mx.z, w.z);
            }
    if (mn.x > mx.x) return;

    // center X/Z on the body axis, move to the anchor height
    item.fitOffset.x = -(mn.x + mx.x) * 0.5f;
    item.fitOffset.z = -(mn.z + mx.z) * 0.5f;
    if (preset->bottomAlign)
        item.fitOffset.y = preset->yOffset - mn.y;
    else
        item.fitOffset.y = preset->yOffset - (mn.y + mx.y) * 0.5f;
}

} // namespace ce
