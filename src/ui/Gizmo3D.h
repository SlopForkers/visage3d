#pragma once
#include "core/Math3D.h"
#include "render/Camera.h"
#include "imgui.h"

namespace ce {

// Self-contained screen-space manipulation gizmo drawn via ImGui drawlist.
// Modes: translate (3 axes + 3 planes), rotate (3 rings), scale (uniform).
// The caller owns the manipulated values (offset/rot/scale); the gizmo edits
// its working copies and reports changes.
class Gizmo3D {
public:
    enum Mode { Translate = 0, Rotate = 1, Scale = 2 };
    enum Part {
        None = -1,
        AxisX = 0, AxisY = 1, AxisZ = 2,
        PlaneXY = 3, PlaneYZ = 4, PlaneXZ = 5,
        Uniform = 6
    };

    Mode mode = Translate;
    bool dragging = false;

    // working copies (synced from the item when not dragging)
    Vec3 offset{0, 0, 0};
    Quat rot{};
    float scale = 1.f;

    // Must be called every frame before camera input.
    // origin: world-space gizmo pivot. winW/winH: framebuffer size.
    // Returns true when the gizmo consumes the mouse (hover or drag).
    // changed: offset/rot/scale were modified this frame.
    // ended: a drag finished this frame (caller should do heavy refresh).
    bool frame(int winW, int winH, const Camera& cam, Vec3 origin, bool& changed, bool& ended);
    void draw(int winW, int winH, const Camera& cam) const;

private:
    Vec3 origin_{0, 0, 0};
    int hover_ = None;
    int active_ = None;
    // drag snapshots
    Vec3 startOffset_{0, 0, 0};
    Quat startRot_{};
    float startScale_ = 1.f;
    ImVec2 startMouse_{0, 0};

    bool project(const Mat4& vp, int w, int h, const Vec3& p, ImVec2& out) const;
    float gizmoWorldSize(const Camera& cam, int winH) const;
    int hitTest(int winW, int winH, const Camera& cam, ImVec2 mouse) const;
    static ImU32 partColor(int part, bool highlight);
};

} // namespace ce
