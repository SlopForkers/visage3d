#pragma once
#include <string>
#include <vector>

namespace ce {

class Model;
class Skeleton;
struct ClothingItem;

// Exports the current character as a binary glTF (.glb):
//  - body shape: skeleton scale/translate offsets are baked into node TRS
//    (inverse bind matrices untouched, so standard skinning reproduces the
//    current figure);
//  - morphs + vertex effectors/deforms: baked into POSITION/NORMAL
//    (blended* arrays); morph TARGETS are kept (weights zero) so expressions
//    remain usable in other tools;
//  - clothing items (visible ones): extra meshes skinned to the body
//    skeleton (transferred joints/weights), identity node transform;
//  - textures re-encoded to PNG and embedded.
bool exportGlb(const std::string& path, const Model& body, const Skeleton& skeleton,
               const std::vector<ClothingItem>& clothing, int bodySkinIndex,
               std::string& err);

} // namespace ce
