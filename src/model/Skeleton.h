#pragma once
#include "model/Model.h"

namespace ce {

// Computes world-space node transforms and skinning matrices.
// Supports multiplicative per-node local scale offsets (used by body-shape
// parameters, e.g. breast / buttocks size) with optional child compensation.
class Skeleton {
public:
    void bind(const Model& model);

    // Recomputes world matrices from bind pose + current scale offsets.
    void update();

    // Skinning matrices for a skin: world(joint) * inverseBind, plus
    // inverse-transpose 3x3 for correct normals under non-uniform scale.
    void skinMatrices(int skinIndex, std::vector<Mat4>& outMats, std::vector<Mat3>& outNormalMats) const;

    // Multiplicative local scale offset per node (default (1,1,1)).
    std::vector<Vec3> scaleOffset;
    // Additive local translation offset per node (default (0,0,0)).
    std::vector<Vec3> translateOffset;

    const std::vector<Mat4>& world() const { return world_; }
    int jointOfNode(int nodeIndex) const; // index within skin joint array, or -1

private:
    const Model* model_ = nullptr;
    std::vector<Mat4> world_;
    std::vector<int> order_; // parent-before-children traversal order
};

} // namespace ce
