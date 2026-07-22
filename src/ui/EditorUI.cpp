#include "ui/EditorUI.h"
#include "app/FileDialog.h"
#include "imgui.h"
#include <algorithm>
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

    // ---- header: model open ----
    if (ImGui::Button("Открыть модель...", ImVec2(-1, 0))) {
        std::vector<std::string> files = openFileDialog(
            L"Открыть модель", L"glTF / VRM\0*.vrm;*.glb;*.gltf\0Все файлы\0*.*\0", false);
        if (!files.empty() && cb.openModel) cb.openModel(files.front());
    }
    if (model && !model->fileName.empty()) {
        ImGui::TextDisabled("%s", model->fileName.c_str());
        size_t verts = 0, tris = 0;
        for (const Mesh& m : model->meshes)
            for (const Primitive& p : m.prims) {
                verts += p.pos.size();
                tris += p.indices.size() / 3;
            }
        ImGui::TextDisabled("Вершин: %zu  Треуг.: %zu  Костей: %zu", verts, tris,
                            model->nodes.size());
    }

    // ---- tabs ----
    if (ImGui::BeginTabBar("##maintabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Тело")) {
            drawBodyTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Лицо")) {
            drawFaceTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Причёска")) {
            drawHairTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Одежда")) {
            drawClothingTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Пресеты")) {
            drawPresetsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Вид")) {
            drawViewTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // ---- footer ----
    ImGui::Separator();
    ImGui::TextDisabled("%.1f FPS", fps);
    if (!status.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void EditorUI::drawParamSlider(ShapeParam& p) {
    ImGui::PushID(p.id.c_str());
    std::string label = p.name;
    if (p.type == ShapeParam::Type::BoneScale && !p.rules.empty()) {
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

void EditorUI::drawBodyTab() {
    if (!controller) return;
    if (ImGui::Button("Сбросить всё", ImVec2(-1, 0))) controller->resetAll();
    ImGui::Spacing();
    for (ShapeParam& p : controller->params())
        if (p.group == "Тело" || p.group == "Body") drawParamSlider(p);
}

void EditorUI::drawFaceTab() {
    if (!controller) return;
    bool hasFaceBones = false;
    for (ShapeParam& p : controller->params())
        if (p.type == ShapeParam::Type::BoneScale && (p.group == "Лицо" || p.group == "Face")) {
            if (!hasFaceBones) {
                ImGui::TextDisabled("Форма");
                hasFaceBones = true;
            }
            drawParamSlider(p);
        }
    if (hasFaceBones) ImGui::Separator();
    drawMorphGroups();
}

void EditorUI::drawMorphGroups() {
    std::vector<std::string> groupOrder;
    std::map<std::string, std::vector<ShapeParam*>> groups;
    for (ShapeParam& p : controller->params()) {
        if (p.type != ShapeParam::Type::Morph) continue;
        if (groups.find(p.group) == groups.end()) groupOrder.push_back(p.group);
        groups[p.group].push_back(&p);
    }
    for (const std::string& g : groupOrder) {
        bool open = (g.find("Эмоции") != std::string::npos ||
                     g.find("Expressions") != std::string::npos);
        if (ImGui::TreeNodeEx(g.c_str(), open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            for (ShapeParam* p : groups[g]) drawParamSlider(*p);
            ImGui::TreePop();
        }
    }
}

void EditorUI::drawHairTab() {
    ImGui::Checkbox("Показывать волосы", &hairVisible);
    ImGui::ColorEdit3("Цвет волос", hairTint);
    ImGui::Spacing();
    ImGui::TextDisabled("Своя причёска: наденьте меш волос\nиз каталога на вкладке «Одежда» —\n"
                        "веса перенесутся на кость головы\nавтоматически (слот «Причёска»).");
}

void EditorUI::drawClothingTab() {
    if (!clothing) return;
    if (ImGui::Button("Добавить одежду...", ImVec2(-1, 0))) {
        std::vector<std::string> files = openFileDialog(
            L"Добавить одежду", L"glTF / VRM\0*.vrm;*.glb;*.gltf\0Все файлы\0*.*\0", true);
        for (const std::string& f : files)
            if (cb.wearClothing) cb.wearClothing(f);
    }
    ImGui::TextDisabled("Веса переносятся с тела автоматически.\n"
                        "Одежда повторяет изменения фигуры.");
    ImGui::Checkbox("Авто-подгонка под фигуру", &autoRefit);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Пересчитывать посадку одежды после изменения\n"
                          "параметров тела (с задержкой ~0.8 с)");

    // ---- equipment slots ----
    if (ImGui::TreeNodeEx("Слоты", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& items = clothing->items();
        bool any = false;
        for (const ClothingManager::SlotDef& s : clothing->slots()) {
            int idx = clothing->itemInSlot(s.id);
            ImGui::PushID(s.id.c_str());
            ImGui::TextDisabled("%s:", s.name.c_str());
            ImGui::SameLine(90);
            if (idx < 0) {
                ImGui::TextDisabled("—");
            } else {
                any = true;
                if (ImGui::SmallButton("x")) {
                    if (cb.removeClothing) cb.removeClothing(idx);
                    ImGui::PopID();
                    continue;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Снять");
                ImGui::SameLine();
                if (ImGui::Selectable(items[idx].name.c_str(), selectedClothing == idx))
                    selectedClothing = idx;
            }
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("Слоты пусты — наденьте из каталога");
        ImGui::TreePop();
    }

    // ---- catalog of garments found in models/ ----
    if (catalog && ImGui::TreeNodeEx("Каталог", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Обновить"))
            if (cb.rescanCatalog) cb.rescanCatalog();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Пересканировать папку %s/", catalog->dir().c_str());
        if (catalog->entries().empty()) {
            ImGui::TextDisabled("В папке %s/ нет моделей (.gltf/.glb/.vrm)",
                                catalog->dir().c_str());
        } else {
            // group by slot category, in slot order; slot-less items last
            auto slotName = [&](const std::string& slotId) -> std::string {
                for (const ClothingManager::SlotDef& s : clothing->slots())
                    if (s.id == slotId) return s.name;
                return "Прочее";
            };
            std::vector<std::string> order;
            for (const ClothingManager::SlotDef& s : clothing->slots()) order.push_back(s.id);
            order.push_back("");
            auto& items = clothing->items();
            for (const std::string& slotId : order) {
                bool any = false;
                for (const CatalogEntry& e : catalog->entries())
                    if (e.slot == slotId) { any = true; break; }
                if (!any) continue;
                if (!ImGui::TreeNode(slotName(slotId).c_str())) continue; // group collapsed
                for (const CatalogEntry& e : catalog->entries()) {
                    if (e.slot != slotId) continue;
                    bool worn = false;
                    for (const ClothingItem& it : items)
                        if (it.path == e.path) { worn = true; break; }
                    ImGui::PushID(e.path.c_str());
                    if (worn) {
                        ImGui::Bullet();
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", e.name.c_str());
                    } else if (ImGui::Selectable(e.name.c_str())) {
                        if (cb.wearClothing) cb.wearClothing(e.path);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.path.c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }

    // ---- type anchors editor ----
    if (ImGui::TreeNode("Якоря типов одежды")) {
        ImGui::TextDisabled("Высота положения на теле:");
        for (ClothTypePreset& t : clothing->types()) {
            if (t.id == "auto" || t.id == "accessory") continue;
            ImGui::PushID(t.id.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat(t.name.c_str(), &t.yOffset, 0.f, 1.7f, "%.2f м")) {
                clothing->saveTypes("config/clothing_types.json");
                // re-apply the moved anchor to items wearing this type
                // (keep the current scale — only the height changes)
                for (int i = 0; i < static_cast<int>(clothing->items().size()); ++i)
                    if (clothing->items()[i].type == t.id) {
                        clothing->applyType(i, t.id, false);
                        if (cb.refitClothing) cb.refitClothing(i);
                    }
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }

    auto& items = clothing->items();
    if (items.empty()) {
        ImGui::TextDisabled("Нет одежды");
        return;
    }

    // ---- gizmo mode ----
    ImGui::TextDisabled("Gizmo:");
    ImGui::SameLine();
    auto modeButton = [&](const char* label, int m) {
        bool active = gizmoMode == m;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(label)) gizmoMode = m;
        if (active) ImGui::PopStyleColor();
    };
    modeButton("Перемещение", 0);
    ImGui::SameLine();
    modeButton("Вращение", 1);
    ImGui::SameLine();
    modeButton("Масштаб", 2);

    if (selectedClothing >= static_cast<int>(items.size()))
        selectedClothing = static_cast<int>(items.size()) - 1;

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        ClothingItem& item = items[i];
        ImGui::PushID(i);
        ImGui::Separator();
        bool vis = item.visible;
        if (ImGui::Checkbox("##vis", &vis)) {
            item.visible = vis;
            if (cb.clothingVisible) cb.clothingVisible(i, vis);
        }
        ImGui::SameLine();
        if (ImGui::Selectable(item.name.c_str(), selectedClothing == i))
            selectedClothing = i;
        if (!item.weightsReady) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "(нет переноса весов)");
        }

        // type combo (anchor snap + size-to-body)
        {
            const ClothTypePreset* cur = clothing->typePreset(item.type);
            std::string curName = cur ? cur->name : item.type;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##type", curName.c_str())) {
                for (const ClothTypePreset& t : clothing->types()) {
                    bool selected = (t.id == item.type);
                    if (ImGui::Selectable(t.name.c_str(), selected)) {
                        clothing->applyType(i, t.id, true);
                        if (cb.refitClothing) cb.refitClothing(i);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::TreeNode("Подгонка")) {
            float padMm = item.padding * 1000.f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("Отступ, мм", &padMm, 0.f, 30.f)) {
                item.padding = padMm / 1000.f;
                if (cb.padClothing) cb.padClothing(i); // cheap live update
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("Обтяжка", &item.shrink, 0.f, 1.f)) {
                if (cb.padClothing) cb.padClothing(i); // same cheap path
            }
            float scale = item.fitScale;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("Масштаб", &scale, 0.2f, 3.f, "x%.3f")) {
                item.fitScale = scale;
                if (cb.liveFitClothing) cb.liveFitClothing(i); // live during drag
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                if (cb.refitClothing) cb.refitClothing(i);
            float off[3] = {item.fitOffset.x, item.fitOffset.y, item.fitOffset.z};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat3("Смещение", off, -0.5f, 0.5f, "%.3f м")) {
                item.fitOffset = {off[0], off[1], off[2]};
                if (cb.liveFitClothing) cb.liveFitClothing(i);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
                if (cb.refitClothing) cb.refitClothing(i);
            if (ImGui::Button("Подогнать под тело")) {
                // deterministic re-placement: type anchor + size-to-body
                clothing->applyType(i, item.type, true);
                if (cb.refitClothing) cb.refitClothing(i);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Сбросить посадку: якорь типа + размер по телу");
            ImGui::SameLine();
            if (ImGui::Button("Удалить")) {
                if (cb.removeClothing) cb.removeClothing(i);
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

nlohmann::json EditorUI::clothingToJson() const {
    nlohmann::json arr = nlohmann::json::array();
    if (!clothing) return arr;
    for (const ClothingItem& item : clothing->items()) {
        nlohmann::json e;
        e["path"] = item.path;
        e["fitScale"] = item.fitScale;
        e["fitOffset"] = {item.fitOffset.x, item.fitOffset.y, item.fitOffset.z};
        e["padding"] = item.padding;
        e["shrink"] = item.shrink;
        e["visible"] = item.visible;
        e["type"] = item.type;
        e["slot"] = item.slot;
        e["fitRot"] = {item.fitRot.x, item.fitRot.y, item.fitRot.z, item.fitRot.w};
        arr.push_back(std::move(e));
    }
    return arr;
}

void EditorUI::drawPresetsTab() {
    if (!presets || !controller) return;

    ImGui::InputTextWithHint("##presetname", "Имя пресета", presetName_, sizeof(presetName_));
    if (ImGui::Button("Сохранить", ImVec2(-1, 0))) {
        std::string err;
        if (presets->save(presetName_, model ? model->fileName : "", controller->values(),
                          clothingToJson(), err)) {
            status = "Пресет сохранён: " + std::string(presetName_);
            presetListDirty_ = true;
        } else {
            status = "Ошибка: " + err;
        }
    }
    ImGui::Spacing();

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
            nlohmann::json clothJson;
            if (presets->load(name, vals, clothJson, err)) {
                controller->setValues(vals);
                if (cb.applyClothingPreset) cb.applyClothingPreset(clothJson);
                std::strncpy(presetName_, name.c_str(), sizeof(presetName_) - 1);
                status = "Пресет загружен: " + name;
            } else {
                status = "Ошибка: " + err;
            }
        }
        ImGui::PopID();
    }
}

void EditorUI::drawViewTab() {
    ImGui::Checkbox("Сетка", &showGrid);
    ImGui::Checkbox("Каркас", &wireframe);
    ImGui::Spacing();
    if (ImGui::Button("Сброс камеры", ImVec2(-1, 0)))
        if (cb.resetCamera) cb.resetCamera();
    if (ImGui::Button("Сохранить скриншот", ImVec2(-1, 0)))
        if (cb.saveScreenshot) cb.saveScreenshot("screenshot.png");
}

} // namespace ce
