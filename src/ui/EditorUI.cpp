#include "ui/EditorUI.h"
#include "imgui.h"
#include <cstring>
#include <map>

namespace ce {

void EditorUI::draw(int winW, int winH, float fps) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(panelWidth()), static_cast<float>(winH)),
                             ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("Редактор персонажа", nullptr, flags);

    drawModelSection();
    ImGui::Separator();
    drawParams();
    ImGui::Separator();
    drawPresets();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%.1f FPS", fps);
    if (!status.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void EditorUI::drawModelSection() {
    if (!ImGui::CollapsingHeader("Модель", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (model && !model->fileName.empty()) {
        ImGui::TextDisabled("%s", model->fileName.c_str());
        size_t verts = 0, tris = 0, morphs = 0;
        for (const Mesh& m : model->meshes)
            for (const Primitive& p : m.prims) {
                verts += p.pos.size();
                tris += p.indices.size() / 3;
                morphs = std::max(morphs, p.morphs.size());
            }
        ImGui::TextDisabled("Вершин: %zu  Треугольников: %zu", verts, tris);
        ImGui::TextDisabled("Костей: %zu  Морф-таргетов: %zu", model->nodes.size(), morphs);
    } else {
        ImGui::TextDisabled("Модель не загружена");
    }

    ImGui::InputTextWithHint("##openpath", "Путь к .vrm / .glb / .gltf", openPath_,
                             sizeof(openPath_));
    if (ImGui::Button("Открыть", ImVec2(-1, 0)) && openPath_[0])
        if (cb.openModel) cb.openModel(openPath_);
    ImGui::TextDisabled("Или перетащите файл в окно");

    ImGui::Checkbox("Сетка", &showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Каркас", &wireframe);

    if (ImGui::Button("Сброс камеры"))
        if (cb.resetCamera) cb.resetCamera();
    ImGui::SameLine();
    if (ImGui::Button("Скриншот"))
        if (cb.saveScreenshot) cb.saveScreenshot("screenshot.png");
}

void EditorUI::drawParamSlider(ShapeParam& p) {
    ImGui::PushID(p.id.c_str());
    std::string label = p.name;
    if (p.type == ShapeParam::Type::BoneScale) {
        float s = p.minS + (p.maxS - p.minS) * p.value;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "x%.2f", s);
        label += "  " + std::string(buf);
    }
    ImGui::TextUnformatted(label.c_str());
    ImGui::SetNextItemWidth(-34.f);
    if (ImGui::SliderFloat("##v", &p.value, 0.f, 1.f, "")) controller->apply();
    ImGui::SameLine(0, 4);
    if (ImGui::SmallButton("R")) {
        p.value = p.defValue;
        controller->apply();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Сброс к значению по умолчанию");
    ImGui::PopID();
}

void EditorUI::drawParams() {
    if (!controller) return;
    if (!ImGui::CollapsingHeader("Параметры", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (ImGui::Button("Сбросить всё", ImVec2(-1, 0))) controller->resetAll();

    // group params by group name, preserving first-appearance order
    std::vector<std::string> groupOrder;
    std::map<std::string, std::vector<ShapeParam*>> groups;
    for (ShapeParam& p : controller->params()) {
        if (groups.find(p.group) == groups.end()) groupOrder.push_back(p.group);
        groups[p.group].push_back(&p);
    }

    for (const std::string& g : groupOrder) {
        bool open = (g.find("Body") == 0 || g.find("Тело") == 0 ||
                     g.find("Эмоции") != std::string::npos ||
                     g.find("Expressions") != std::string::npos);
        if (ImGui::TreeNodeEx(g.c_str(), open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            for (ShapeParam* p : groups[g]) drawParamSlider(*p);
            ImGui::TreePop();
        }
    }
}

void EditorUI::drawPresets() {
    if (!presets || !controller) return;
    if (!ImGui::CollapsingHeader("Пресеты", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::InputTextWithHint("##presetname", "Имя пресета", presetName_, sizeof(presetName_));
    if (ImGui::Button("Сохранить", ImVec2(-1, 0))) {
        std::string err;
        if (presets->save(presetName_, model ? model->fileName : "", controller->values(), err)) {
            status = "Пресет сохранён: " + std::string(presetName_);
            presetListDirty_ = true;
        } else {
            status = "Ошибка: " + err;
        }
    }

    if (presetListDirty_) {
        presetList_ = presets->list();
        presetListDirty_ = false;
    }
    if (presetList_.empty()) {
        ImGui::TextDisabled("Нет сохранённых пресетов");
        return;
    }
    for (const std::string& name : presetList_) {
        ImGui::PushID(name.c_str());
        if (ImGui::Button("X")) {
            presets->remove(name);
            presetListDirty_ = true;
            ImGui::PopID();
            break;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Удалить");
        ImGui::SameLine();
        if (ImGui::Button(name.c_str(), ImVec2(-1, 0))) {
            std::string err;
            std::map<std::string, float> vals;
            if (presets->load(name, vals, err)) {
                controller->setValues(vals);
                std::strncpy(presetName_, name.c_str(), sizeof(presetName_) - 1);
                status = "Пресет загружен: " + name;
            } else {
                status = "Ошибка: " + err;
            }
        }
        ImGui::PopID();
    }
}

} // namespace ce
