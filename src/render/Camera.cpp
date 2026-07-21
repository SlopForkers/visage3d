#include "render/Camera.h"
#include <algorithm>
#include <cmath>

namespace ce {

void Camera::rotate(float dx, float dy) {
    yaw += dx * 0.008f;
    pitch = std::clamp(pitch + dy * 0.008f, -1.45f, 1.45f);
}

void Camera::pan(float dx, float dy) {
    Vec3 fwd = (target - eye()).normalized();
    Vec3 right = fwd.cross(Vec3{0, 1, 0}).normalized();
    Vec3 up = right.cross(fwd);
    float k = distance * 0.0016f;
    target += right * (-dx * k) + up * (dy * k);
}

void Camera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(0.9f, steps), 0.3f, 20.f);
}

Vec3 Camera::eye() const {
    float cp = std::cos(pitch), sp = std::sin(pitch);
    float cy = std::cos(yaw), sy = std::sin(yaw);
    return target + Vec3{sy * cp, -sp, cy * cp} * distance;
}

Mat4 Camera::view() const { return Mat4::lookAt(eye(), target, {0, 1, 0}); }

Mat4 Camera::projection(float aspect) const {
    return Mat4::perspective(fovY, aspect, zNear, zFar);
}

} // namespace ce
