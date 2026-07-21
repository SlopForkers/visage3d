#pragma once
#include "core/Math3D.h"

namespace ce {

// Orbit camera: rotate (LMB drag), pan (RMB drag / Shift+LMB), zoom (wheel).
class Camera {
public:
    Vec3 target{0.f, 0.95f, 0.f};
    float yaw = 0.f;          // radians, 0 = looking from +Z
    float pitch = -0.12f;     // radians, negative = slightly above
    float distance = 2.6f;
    float fovY = 0.8f;        // ~46 degrees
    float zNear = 0.02f, zFar = 100.f;

    void rotate(float dx, float dy);
    void pan(float dx, float dy);
    void zoom(float steps);

    Vec3 eye() const;
    Mat4 view() const;
    Mat4 projection(float aspect) const;
};

} // namespace ce
