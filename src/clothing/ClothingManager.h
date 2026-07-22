#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include <memory>
#include <string>
#include <vector>

namespace ce {

// A clothing item: a static (unskinned) mesh auto-fitted and auto-skinned
// onto the body by nearest-surface weight transfer.
struct ClothingItem {
    std::string name;      // display name (file name)
    std::string path;      // source file (for presets)
    Model model;           // loaded mesh, node transforms baked into vertices
    int renderSlot = -1;

    // fit transform: worldPos = fitRot * (rawPos * fitScale) + fitOffset
    float fitScale = 1.f;
    float unitScale = 1.f; // auto-detected unit conversion (for UI hints)
    Quat fitRot{};          // rotation (gizmo)
    Vec3 fitOffset{0, 0, 0};

    Mat4 fitMatrix() const {
        return Mat4::translation(fitOffset) * Mat4::rotation(fitRot) *
               Mat4::scaling({fitScale, fitScale, fitScale});
    }
    Mat4 fitMatrixNoTrans() const {
        return Mat4::rotation(fitRot) * Mat4::scaling({fitScale, fitScale, fitScale});
    }
    float padding = 0.004f; // surface offset (m) against body poke-through
    float shrink = 0.f;     // 0..1 shrink-wrap towards the body surface
    bool visible = true;
    std::string type = "auto"; // clothing type id ("auto" = keep authored position)
    Vec3 rawCenter{0, 0, 0};   // bbox center in raw space (gizmo pivot)

    bool weightsReady = false; // false until transferWeights succeeded

    // per-mesh / per-prim fitted data (kept for cheap padding updates)
    struct PrimFit {
        std::vector<Vec3> basePos;   // fitted, before padding/shrink
        std::vector<Vec3> pushDir;   // away-from-body direction
        std::vector<Vec3> targetPos; // nearest body surface point
    };
    std::vector<std::vector<PrimFit>> fits; // [mesh][prim]
};

// A clothing-type anchor preset: where a garment of this type should sit on
// the body. yOffset is the target height (m) of the garment bbox center
// (or bbox bottom when bottomAlign=true, e.g. shoes on the ground).
struct ClothTypePreset {
    std::string id;   // "panties"
    std::string name; // "Трусы"
    float yOffset = 0.9f;
    bool bottomAlign = false;
};

// Owns clothing items and the auto-fitting pipeline:
//  1) unit auto-detection (mm/cm/dm/m) from bounding box magnitude
//  2) weight transfer from nearest body surface points (spatial hash grid)
//  3) padding along the away-from-body direction
// Body-shape parameters deform clothing automatically afterwards, because
// clothing vertices are skinned by the same skeleton.
class ClothingManager {
public:
    void bind(Model& body, Skeleton& skeleton);

    // Loads a clothing model, auto-detects its unit scale, transfers weights.
    // Returns item index or -1 on error (err filled).
    int add(const std::string& path, std::string& err);
    void remove(int index);
    void clear();

    // Recomputes fitted vertex data + weight transfer (after fit transform change)
    void refit(int index);
    // Cheap: re-applies padding/shrink from stored basePos/pushDir/targetPos
    void applyPadding(int index);
    // Cheap: re-transforms vertices with the current fit matrix (gizmo dragging);
    // clamps against stored (stale) targets. Call refit() on release.
    void applyFit(int index);

    // Coordinate-descent fit: searches scale + offset minimizing the mean
    // distance from clothing vertices to the body surface.
    void autoFitToBody(int index);

    // ---- clothing types with anchor offsets ----
    // Applies the type's anchor: centers the garment X/Z and moves it to the
    // type's target height. "auto" keeps the current fit. Call refit() after.
    void applyType(int index, const std::string& typeId);
    std::string detectType(const std::string& fileName) const;
    const std::vector<ClothTypePreset>& types() const { return types_; }
    std::vector<ClothTypePreset>& types() { return types_; }
    const ClothTypePreset* typePreset(const std::string& typeId) const;
    void loadTypes(const std::string& configPath); // missing file -> builtins
    bool saveTypes(const std::string& configPath) const;

    std::vector<ClothingItem>& items() { return items_; }
    const std::vector<ClothingItem>& items() const { return items_; }

    // skin index of the body mesh (clothing joints index into this skin)
    int bodySkinIndex() const { return bodySkinIndex_; }

private:
    Model* body_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    std::vector<ClothingItem> items_;
    std::vector<ClothTypePreset> types_;
    int bodySkinIndex_ = -1;

    void bakeNodeTransforms(Model& model);
    float autoUnitScale(const Model& model) const;
    bool transferWeights(ClothingItem& item);

    // body surface point cloud for nearest queries (world space, current pose)
    struct BodyPoint {
        Vec3 pos;
        Vec3 nrm; // skinned surface normal (true outward direction)
        uint16_t joints[4];
        float weights[4];
    };
    std::vector<BodyPoint> buildBodyPointCloud();

    struct CloudGrid;
    std::unique_ptr<CloudGrid> makeGrid(const std::vector<BodyPoint>& cloud, float cell);
    float distToCloud(const Vec3& p, const CloudGrid& grid) const;
};

} // namespace ce
