#pragma once
#include <string>
#include <vector>

namespace ce {

// Native Windows open-file dialog (GetOpenFileNameW).
// filter example: L"glTF / VRM\0*.vrm;*.glb;*.gltf\0All files\0*.*\0"
// With multi=true returns all selected files; empty vector = cancelled.
std::vector<std::string> openFileDialog(const wchar_t* title, const wchar_t* filter,
                                        bool multi = false);

} // namespace ce
