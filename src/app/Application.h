#pragma once
#include "editor/Presets.h"
#include "editor/ShapeController.h"
#include "model/GltfLoader.h"
#include "model/Skeleton.h"
#include "render/Camera.h"
#include "render/ModelRenderer.h"
#include "ui/EditorUI.h"
#include <string>

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

    bool headlessScreenshot_ = false;
    std::string pendingScreenshot_;

    bool loadModel(const std::string& path);
    void applyPreset(const std::string& name);
    void saveScreenshot(const std::string& path);
    void resetCamera();
    void handleInput(float dt);
};

} // namespace ce
