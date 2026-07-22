#include "clothing/ClothingCatalog.h"
#include "clothing/ClothingManager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ce {

void ClothingCatalog::scan(const std::string& dir, const std::string& excludeFileName) {
    entries_.clear();
    dir_ = dir;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };

    for (const auto& it : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (!it.is_regular_file()) continue;
        std::string ext = lower(it.path().extension().string());
        if (ext != ".gltf" && ext != ".glb" && ext != ".vrm") continue;
        std::string fileName = it.path().filename().string();
        if (!excludeFileName.empty() && lower(fileName) == lower(excludeFileName)) continue;

        CatalogEntry e;
        e.path = it.path().generic_string(); // forward slashes (presets, fopen OK)

        // Sketchfab exports are all "scene.gltf" — the meaningful name lives
        // in the parent directory
        std::string stem = it.path().stem().string();
        if (lower(stem) == "scene" && it.path().has_parent_path() &&
            it.path().parent_path() != std::filesystem::path(dir))
            e.name = it.path().parent_path().filename().string();
        else
            e.name = stem;
        std::replace(e.name.begin(), e.name.end(), '_', ' ');

        e.type = ClothingManager::detectType(e.path);
        e.slot = ClothingManager::slotForType(e.type);
        entries_.push_back(std::move(e));
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const CatalogEntry& a, const CatalogEntry& b) { return a.name < b.name; });
}

} // namespace ce
