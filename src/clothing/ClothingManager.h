#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include <functional>
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
    float shrink = 0.f;     // 0..1 shrink-wrap ("magnet") towards the body surface
    float looseness = 0.f;  // 0..1 drape: smooths the fit, bridges concavities
                            // (cleavage!) instead of painting the body shape
    bool visible = true;
    std::string type = "auto"; // clothing type id ("auto" = keep authored position)
    std::string slot;          // equipment slot id ("" = free, not slot-bound)
    Vec3 rawCenter{0, 0, 0};   // bbox center in raw space (gizmo pivot)
    Vec3 rawMin{0, 0, 0}, rawMax{0, 0, 0}; // raw-space bbox (for size-to-body)

    bool weightsReady = false; // false until transferWeights succeeded

    // per-mesh / per-prim fitted data (kept for cheap padding updates)
    struct PrimFit {
        std::vector<Vec3> basePos;   // fitted, before padding/shrink
        std::vector<Vec3> pushDir;   // away-from-body direction
        std::vector<Vec3> targetPos; // nearest body surface point
        std::vector<Vec3> drapePos;  // nearest point on the DRAPE ENVELOPE
                                     // (smoothed body: concavities filled)
        std::vector<Vec3> drapeDir;  // envelope normal
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
//  2) deterministic placement: type anchor (height on the body) + size-to-body
//     width match (no iterative search — see applyType)
//  3) weight transfer from nearest body surface points (spatial hash grid)
//  4) padding along the away-from-body direction
// Body-shape parameters deform clothing automatically afterwards, because
// clothing vertices are skinned by the same skeleton.
//
// The body point cloud + its lookup grid are cached and shared by all items;
// call invalidateBody() when the body shape/pose changes.
class ClothingManager {
public:
    ClothingManager();
    ~ClothingManager(); // out-of-line: CloudGrid is incomplete here (pimpl)

    void bind(Model& body, Skeleton& skeleton);

    // Loads a clothing model, auto-detects its unit scale, places it by type
    // and transfers weights. deferFit=true skips placement+transfer (the
    // caller sets stored fit params and calls refit() itself — preset load).
    // Returns item index or -1 on error (err filled).
    int add(const std::string& path, std::string& err, bool deferFit = false);
    void remove(int index);
    void clear();

    // Recomputes fitted vertex data + weight transfer (after fit transform change)
    void refit(int index);
    // Cheap: re-applies padding/shrink from stored basePos/pushDir/targetPos
    void applyPadding(int index);
    // Cheap: re-transforms vertices with the current fit matrix (gizmo dragging);
    // clamps against stored (stale) targets. Call refit() on release.
    void applyFit(int index);

    // ---- clothing types with anchor offsets ----
    // Applies the type's anchor: centers the garment X/Z on the body axis and
    // moves it to the type's target height. With scaleToBody=true (new
    // placement, "Подогнать под тело") the garment is also uniformly scaled so
    // its width matches the body width measured at the garment's top band.
    // "auto"/"accessory" keep the authored placement. Call refit() after.
    void applyType(int index, const std::string& typeId, bool scaleToBody = false);

    static std::string detectType(const std::string& fileName);
    // Equipment slot for a clothing type ("" = not slot-bound, coexists freely).
    static std::string slotForType(const std::string& typeId);

    struct SlotDef {
        std::string id;   // "top"
        std::string name; // "Верх"
    };
    const std::vector<SlotDef>& slots() const { return slots_; }
    int itemInSlot(const std::string& slotId) const; // item index or -1

    const std::vector<ClothTypePreset>& types() const { return types_; }
    std::vector<ClothTypePreset>& types() { return types_; }
    const ClothTypePreset* typePreset(const std::string& typeId) const;
    void loadTypes(const std::string& configPath); // missing file -> builtins
    bool saveTypes(const std::string& configPath) const;

    // The body shape/pose changed: rebuild the shared point cloud on next use.
    void invalidateBody() { cloudDirty_ = true; }

    std::vector<ClothingItem>& items() { return items_; }
    const std::vector<ClothingItem>& items() const { return items_; }

    // Fired after add/remove/clear: the items vector may have reallocated or
    // shifted, so outside observers holding pointers INTO items (e.g. the
    // renderer's per-slot Model*) must re-resolve them.
    std::function<void()> onItemsChanged;

    // skin index of the body mesh (clothing joints index into this skin)
    int bodySkinIndex() const { return bodySkinIndex_; }

private:
    Model* body_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    std::vector<ClothingItem> items_;
    std::vector<ClothTypePreset> types_;
    std::vector<SlotDef> slots_;
    int bodySkinIndex_ = -1;

    void bakeNodeTransforms(Model& model);
    float autoUnitScale(const Model& model) const;
    bool transferWeights(ClothingItem& item);

    // body surface point cloud for nearest queries (world space, current pose)
    struct BodyPoint {
        Vec3 pos;
        Vec3 nrm;   // skinned surface normal (true outward direction)
        uint16_t joints[4];
        float weights[4];
        bool arm;   // dominant joint belongs to an arm (excluded from width match)
    };
    struct CloudGrid;
    std::vector<BodyPoint> cloud_;       // cached, shared by all items
    std::unique_ptr<CloudGrid> grid_;    // spatial hash over cloud_
    std::vector<BodyPoint> drapeCloud_;  // smoothed copy of cloud_ (drape envelope:
                                         // concavities filled, convex parts restored)
    std::unique_ptr<CloudGrid> drapeGrid_;
    std::vector<uint32_t> bodyAdjOff_, bodyAdjIdx_; // cloud vertex adjacency (CSR)
    std::vector<bool> armJoints_;        // per body-skin joint: under an arm bone
    bool cloudDirty_ = true;

    const std::vector<BodyPoint>& bodyCloud(); // builds cloud_+grids when dirty
    void buildArmJointFlags();
    // drape envelope: smoothing + convex restoration of the body cloud
    static void smoothCloud(std::vector<BodyPoint>& cloud, const std::vector<uint32_t>& off,
                            const std::vector<uint32_t>& idx, int iters, float lambda);
    // Exact K nearest points of a cloud (expanding grid rings + safe
    // early-out, brute force only as a last resort). Returns count (<= k).
    int knnCloud(const Vec3& p, int k, int* outIdx, float* outDistSq,
                 const std::vector<BodyPoint>& cloud, const CloudGrid& grid) const;
    int knnBody(const Vec3& p, int k, int* outIdx, float* outDistSq) const {
        return knnCloud(p, k, outIdx, outDistSq, cloud_, *grid_);
    }
    // Body width (m) at a height band, arm points excluded; 0 when unknown.
    float bodyWidthInBand(float y0, float y1) const;
    // Body center (x,z) of the cloud; falls back to origin when empty.
    void bodyAxisCenter(float& cx, float& cz) const;
};

} // namespace ce
