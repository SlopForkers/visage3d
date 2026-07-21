#include "model/Skeleton.h"
#include <algorithm>
#include <functional>

namespace ce {

void Skeleton::bind(const Model& model) {
    model_ = &model;
    size_t n = model.nodes.size();
    world_.assign(n, Mat4::identity());
    scaleOffset.assign(n, Vec3{1, 1, 1});
    translateOffset.assign(n, Vec3{0, 0, 0});

    // Topological order: DFS from scene roots guarantees parents before children.
    order_.clear();
    order_.reserve(n);
    std::function<void(int)> visit = [&](int node) {
        order_.push_back(node);
        for (int c : model.nodes[node].children) visit(c);
    };
    for (int root : model.sceneRoots) visit(root);
    // Safety: also cover nodes unreachable from the scene.
    for (size_t i = 0; i < n; ++i)
        if (model.nodes[i].parent < 0 &&
            std::find(order_.begin(), order_.end(), static_cast<int>(i)) == order_.end())
            visit(static_cast<int>(i));
}

void Skeleton::update() {
    const Model& m = *model_;
    for (int i : order_) {
        const Node& node = m.nodes[i];
        Vec3 s = node.s * scaleOffset[i];
        Vec3 t = node.t + translateOffset[i];
        Mat4 local = Mat4::trs(t, node.r, s);
        world_[i] = (node.parent >= 0) ? world_[node.parent] * local : local;
    }
}

void Skeleton::skinMatrices(int skinIndex, std::vector<Mat4>& outMats,
                            std::vector<Mat3>& outNormalMats) const {
    const Skin& skin = model_->skins[skinIndex];
    size_t nj = skin.joints.size();
    outMats.resize(nj);
    outNormalMats.resize(nj);
    for (size_t i = 0; i < nj; ++i) {
        Mat4 jm = world_[skin.joints[i]];
        if (i < skin.inverseBindMatrices.size())
            jm = jm * skin.inverseBindMatrices[i];
        outMats[i] = jm;
        outNormalMats[i] = jm.upper3x3().inverted().transposed();
    }
}

int Skeleton::jointOfNode(int nodeIndex) const {
    if (!model_ || model_->skins.empty()) return -1;
    const Skin& skin = model_->skins[0];
    for (size_t i = 0; i < skin.joints.size(); ++i)
        if (skin.joints[i] == nodeIndex) return static_cast<int>(i);
    return -1;
}

} // namespace ce
