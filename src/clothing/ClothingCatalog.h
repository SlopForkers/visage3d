#pragma once
#include <string>
#include <vector>

namespace ce {

// A garment file discovered in the catalog directory.
struct CatalogEntry {
    std::string path; // path to .gltf/.glb/.vrm (forward slashes)
    std::string name; // display name (parent dir for Sketchfab "scene.gltf")
    std::string type; // detected clothing type id
    std::string slot; // equipment slot id ("" = free, not slot-bound)
};

// Scans a directory (models/ by default) recursively for wearable garment
// files and detects their type/slot from path keywords.
class ClothingCatalog {
public:
    // Recursively scans dir; excludeFileName (e.g. the loaded body model) is
    // skipped by file name comparison.
    void scan(const std::string& dir, const std::string& excludeFileName = {});

    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const std::string& dir() const { return dir_; }

private:
    std::vector<CatalogEntry> entries_;
    std::string dir_;
};

} // namespace ce
