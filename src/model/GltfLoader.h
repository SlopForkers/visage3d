#pragma once
#include "model/Model.h"
#include <string>

namespace ce {

// Loads .vrm / .glb (binary) and .gltf (ascii) files into the internal Model.
class GltfLoader {
public:
    static bool load(const std::string& path, Model& out, std::string& error);
};

} // namespace ce
