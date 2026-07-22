#pragma once
#include "model/Model.h"
#include "model/Skeleton.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ce {

// A clothing item: a static (unskinned) mesh auto-skinned onto the body by
// nearest-point weight transfer. The garment mesh itself is NEVER deformed:
// it keeps its authored shape and follows the body through the shared
// skeleton (GPU skinning). Body morphs are bone scales/translates, so the
// clothing inherits every figure change automatically, in real time —
// no refitting, no surface clamping, no tearing/collapse by construction.
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
    bool visible = true;
    std::string type = "auto"; // clothing type id ("auto" = keep authored position)
    std::string slot;          // equipment slot id ("" = free, not slot-bound)
    Vec3 rawCenter{0, 0, 0};   // bbox center in raw space (gizmo pivot)
    Vec3 rawMin{0, 0, 0}, rawMax{0, 0, 0}; // raw-space bbox

    bool weightsReady = false; // false until transferWeights succeeded
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

// Owns clothing items and the binding pipeline:
//  1) unit auto-detection (mm/cm/dm/m) from bounding box magnitude
//  2) deterministic placement: type anchor (height on the body) — position
//     only, the scale is the user's (gizmo / slider), presets store it
//  3) weight transfer: each garment vertex (at its current transform) takes
//     an inverse-square blend of the K nearest body points' skin weights
//     (spatial hash grid, exact expanding-ring KNN)
// After binding, the garment is rendered with GPU skinning against the body
// skeleton, so it tracks every body-shape change with zero extra work.
//
// The body point cloud + its lookup grid are cached and shared by all items;
// call invalidateBody() when the body shape/pose changes (affects future
// binds only — already bound items follow the skeleton live).
class ClothingManager {
public:
    ClothingManager();
    ~ClothingManager(); // out-of-line: CloudGrid is incomplete here (pimpl)

    void bind(Model& body, Skeleton& skeleton);

    // Loads a clothing model, auto-detects its unit scale, places it by type
    // and transfers weights. deferBind=true skips placement+transfer (the
    // caller sets the stored transform and calls rebind() itself — presets).
    // Returns item index or -1 on error (err filled).
    int add(const std::string& path, std::string& err, bool deferBind = false);
    void remove(int index);
    void clear();

    // Full weight (re)transfer at the item's current transform (after gizmo
    // release / slider edit / transform restore from a preset).
    void rebind(int index);
    // Cheap: re-transforms vertices with the current fit matrix (gizmo
    // dragging). Weights stay as-is; call rebind() on release.
    void applyFit(int index);

    // ---- clothing types with anchor offsets ----
    // Applies the type's anchor: centers the garment X/Z on the body axis and
    // moves it to the type's target height. The scale is NOT touched — the
    // user fits the size (gizmo / slider), presets store it. "auto"/"accessory"
    // keep the authored placement entirely. Call rebind() after.
    void applyType(int index, const std::string& typeId);

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
        uint16_t joints[4];
        float weights[4];
    };
    struct CloudGrid;
    std::vector<BodyPoint> cloud_;    // cached, shared by all items
    std::unique_ptr<CloudGrid> grid_; // spatial hash over cloud_
    bool cloudDirty_ = true;

    const std::vector<BodyPoint>& bodyCloud(); // builds cloud_+grid when dirty
    // Exact K nearest points of the cloud (expanding grid rings + safe
    // early-out, brute force only as a last resort). Returns count (<= k).
    int knnCloud(const Vec3& p, int k, int* outIdx, float* outDistSq) const;
    // Body center (x,z) of the cloud; falls back to origin when empty.
    void bodyAxisCenter(float& cx, float& cz) const;
};

} // namespace ce
