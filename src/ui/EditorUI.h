#pragma once
#include "clothing/ClothingCatalog.h"
#include "clothing/ClothingManager.h"
#include "editor/Presets.h"
#include "editor/ShapeController.h"
#include "json.hpp"
#include "model/Model.h"
#include "render/Camera.h"
#include <functional>
#include <string>
#include <vector>

namespace ce {

struct UICallbacks {
    std::function<void(const std::string&)> openModel;      // via native dialog
    std::function<void(const std::string&)> wearClothing;   // wear (replaces slot content)
    std::function<void()> rescanCatalog;                    // rescan the models/ dir
    std::function<void(const std::string&)> saveScreenshot; // path
    std::function<void()> resetCamera;
    std::function<void(int)> refitClothing;    // refit + GPU re-upload
    std::function<void(int)> padClothing;      // cheap padding update + upload
    std::function<void(int)> liveFitClothing;  // cheap fit transform + upload (dragging)
    std::function<void(int)> removeClothing;   // remove slot + item
    std::function<void(int, bool)> clothingVisible;
    std::function<void(const nlohmann::json&)> applyClothingPreset;
};

// Left panel with VRoid-style top tabs: Тело | Лицо | Причёска | Одежда | Пресеты | Вид.
class EditorUI {
public:
    ShapeController* controller = nullptr;
    Presets* presets = nullptr;
    Model* model = nullptr;
    ClothingManager* clothing = nullptr;
    ClothingCatalog* catalog = nullptr;
    UICallbacks cb;

    bool showGrid = true;
    bool wireframe = false;
    bool hairVisible = true;
    float hairTint[3] = {1.f, 1.f, 1.f};
    bool autoRefit = true; // debounced clothing refit on body-shape change
    int selectedClothing = -1; // gizmo target (index into ClothingManager::items)
    int gizmoMode = 0;         // 0 translate, 1 rotate, 2 scale
    std::string status; // last operation result (error or info)

    void draw(int winW, int winH, float fps);
    int panelWidth() const { return 340; }

private:
    char presetName_[128] = "default";
    std::vector<std::string> presetList_;
    bool presetListDirty_ = true;

    void drawBodyTab();
    void drawFaceTab();
    void drawHairTab();
    void drawClothingTab();
    void drawPresetsTab();
    void drawViewTab();
    void drawParamSlider(ShapeParam& p);
    void drawMorphGroups();
    nlohmann::json clothingToJson() const;
};

} // namespace ce
