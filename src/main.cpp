#include "app/Application.h"
#include <cstdio>

// Character editor for glTF/VRM models (female_base.vrm and similar).
//
// Usage:
//   character_editor [model.vrm|.glb|.gltf]
//                    [--model path] [--preset name]
//                    [--screenshot out.png] [--frames N] [--size WxH]
//
// Interactive mode by default. With --frames N the app renders N frames,
// optionally saves a screenshot, and exits (used for automated testing).
int main(int argc, char** argv) {
    ce::Application app;
    if (!app.init(argc, argv)) {
        std::fprintf(stderr, "Failed to initialize application\n");
        return 1;
    }
    return app.run();
}
