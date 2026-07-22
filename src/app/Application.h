#pragma once
#include "clothing/ClothingCatalog.h"
#include "clothing/ClothingManager.h"
#include "editor/Presets.h"
#include "editor/ShapeController.h"
#include "json.hpp"
#include "model/GltfLoader.h"
#include "model/Skeleton.h"
#include "render/Camera.h"
#include "render/ModelRenderer.h"
#include "ui/EditorUI.h"
#include "ui/Gizmo3D.h"
#include <string>
#include <vector>

struct GLFWwindow;

namespace ce {

class Application {
public:
    bool init(int argc, char** argv);
    int run();

private:
    GLFWwindow* window_ = nullptr;
    Model model_;
    Skeleton skeleton_;
    ShapeController controller_;
    ModelRenderer renderer_;
    Camera camera_;
    EditorUI ui_;
    Presets presets_;
    ClothingManager clothing_;
    ClothingCatalog catalog_;
    std::string bodyModelPath_; // loaded body model (excluded from the catalog)
    int bodySlot_ = -1;
    Gizmo3D gizmo_;

    // debounced clothing refit on body-shape change
    uint64_t seenRevision_ = 0;
    double refitAt_ = 0.0;

    // CLI options
    std::string optModel_ = "female_base.vrm";
    std::string optPreset_;
    std::string optScreenshot_;
    int optFrames_ = -1; // >0: batch mode (render N frames, optional screenshot, exit)
    int optW_ = 1280, optH_ = 800;
    float optYawDeg_ = 0.f; // initial camera yaw (0 = front, 180 = back)
    float optDist_ = 0.f;   // initial camera distance (0 = default)
    float optTargetY_ = 0.f; // camera target height (0 = default)
    std::vector<std::pair<std::string, float>> optSet_; // --set id=value overrides
    std::vector<std::string> optClothes_;               // --clothe path
    bool listParams_ = false;                           // --listparams debug

    std::string pendingScreenshot_;

    bool loadModel(const std::string& path);
    void applyPreset(const std::string& name);
    void saveScreenshot(const std::string& path);
    void resetCamera();
    void handleInput(float dt, bool mouseConsumed);
    bool updateGizmo(int winW, int winH); // returns true when gizmo consumes the mouse

    // clothing pipeline (UI callbacks)
    void addClothing(const std::string& path);
    void wearClothing(const std::string& path); // add + unequip the slot's previous item
    void refitClothing(int index);
    void padClothing(int index);
    void liveFitClothing(int index);
    void removeClothing(int index);
    void setClothingVisible(int index, bool visible);
    void applyClothingPreset(const nlohmann::json& items);
    void clearClothing();
};

} // namespace ce
