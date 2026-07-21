#pragma once
#include "editor/Presets.h"
#include "editor/ShapeController.h"
#include "model/Model.h"
#include "render/Camera.h"
#include <functional>
#include <string>
#include <vector>

namespace ce {

struct UICallbacks {
    std::function<void(const std::string&)> openModel;      // path
    std::function<void(const std::string&)> saveScreenshot; // path
    std::function<void()> resetCamera;
};

// Fixed left-side ImGui panel: model info, body sliders, face sliders, presets.
class EditorUI {
public:
    ShapeController* controller = nullptr;
    Presets* presets = nullptr;
    Model* model = nullptr;
    UICallbacks cb;

    bool showGrid = true;
    bool wireframe = false;
    std::string status; // last operation result (error or info)

    void draw(int winW, int winH, float fps);
    int panelWidth() const { return 340; }

private:
    char presetName_[128] = "default";
    char openPath_[512] = "";
    std::vector<std::string> presetList_;
    bool presetListDirty_ = true;

    void drawModelSection();
    void drawParams();
    void drawPresets();
    void drawParamSlider(ShapeParam& p);
};

} // namespace ce
