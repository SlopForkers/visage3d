#include "ui/Gizmo3D.h"
#include <algorithm>
#include <cmath>

namespace ce {

namespace {
const Vec3 AXES[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const ImU32 COLS[3] = {IM_COL32(230, 60, 60, 255), IM_COL32(80, 200, 80, 255),
                       IM_COL32(80, 120, 240, 255)};
const ImU32 COL_HOVER = IM_COL32(250, 220, 60, 255);

ImVec2 operator-(const ImVec2& a, const ImVec2& b) { return {a.x - b.x, a.y - b.y}; }
ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator*(const ImVec2& a, float s) { return {a.x * s, a.y * s}; }
float len2d(const ImVec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
ImVec2 norm2d(const ImVec2& v) {
    float l = len2d(v);
    return l > 1e-6f ? v * (1.f / l) : ImVec2{1, 0};
}
float dot2d(const ImVec2& a, const ImVec2& b) { return a.x * b.x + a.y * b.y; }

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    ImVec2 ab = b - a;
    float l2 = dot2d(ab, ab);
    if (l2 < 1e-6f) return len2d(p - a);
    float t = std::clamp(dot2d(p - a, ab) / l2, 0.f, 1.f);
    return len2d(p - (a + ab * t));
}

bool pointInQuad(ImVec2 p, const ImVec2* q) {
    bool sign = false;
    for (int i = 0; i < 4; ++i) {
        ImVec2 a = q[i], b = q[(i + 1) % 4];
        float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (i == 0) sign = cross > 0;
        else if ((cross > 0) != sign) return false;
    }
    return true;
}
} // namespace

bool Gizmo3D::project(const Mat4& vp, int w, int h, const Vec3& p, ImVec2& out) const {
    float cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    float cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    float cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (cw <= 1e-6f) return false;
    out.x = (cx / cw * 0.5f + 0.5f) * w;
    out.y = (1.f - (cy / cw * 0.5f + 0.5f)) * h;
    return true;
}

float Gizmo3D::gizmoWorldSize(const Camera& cam, int winH) const {
    // constant on-screen size (~90 px)
    float dist = (origin_ - cam.eye()).length();
    float pxWorld = 2.f * dist * std::tan(cam.fovY * 0.5f) / static_cast<float>(winH);
    return pxWorld * 90.f;
}

ImU32 Gizmo3D::partColor(int part, bool highlight) {
    if (highlight) return COL_HOVER;
    if (part >= 0 && part <= 2) return COLS[part];
    if (part >= 3 && part <= 5) return COLS[part - 3] & 0x80FFFFFF; // dimmer
    return IM_COL32(200, 200, 200, 255);
}

int Gizmo3D::hitTest(int winW, int winH, const Camera& cam, ImVec2 mouse) const {
    Mat4 vp = cam.projection(static_cast<float>(winW) / winH) * cam.view();
    float L = gizmoWorldSize(cam, winH);
    ImVec2 o;
    if (!project(vp, winW, winH, origin_, o)) return None;

    const float kHit = 8.f;

    if (mode == Scale) {
        // uniform: center box
        if (len2d(mouse - o) < kHit + 4.f) return Uniform;
    }

    // axes (and scale has axes too? uniform-only: skip axis boxes in scale mode)
    if (mode != Scale) {
        for (int a = 0; a < 3; ++a) {
            if (mode == Rotate) break;
            ImVec2 p1;
            if (!project(vp, winW, winH, origin_ + AXES[a] * L, p1)) continue;
            if (distToSegment(mouse, o, p1) < kHit) return a;
        }
    }

    if (mode == Translate) {
        // planes: quad at origin + a1*s + a2*s, s in [0.35, 0.65] * L
        const int pairs[3][2] = {{0, 1}, {1, 2}, {0, 2}};
        for (int pl = 0; pl < 3; ++pl) {
            Vec3 a1 = AXES[pairs[pl][0]], a2 = AXES[pairs[pl][1]];
            ImVec2 q[4];
            Vec3 c0 = origin_ + a1 * (0.35f * L) + a2 * (0.35f * L);
            Vec3 c1 = origin_ + a1 * (0.65f * L) + a2 * (0.35f * L);
            Vec3 c2 = origin_ + a1 * (0.65f * L) + a2 * (0.65f * L);
            Vec3 c3 = origin_ + a1 * (0.35f * L) + a2 * (0.65f * L);
            if (!project(vp, winW, winH, c0, q[0]) || !project(vp, winW, winH, c1, q[1]) ||
                !project(vp, winW, winH, c2, q[2]) || !project(vp, winW, winH, c3, q[3]))
                continue;
            if (pointInQuad(mouse, q)) return PlaneXY + pl;
        }
    }

    if (mode == Rotate) {
        // rings: circle of radius L perpendicular to each axis
        for (int a = 0; a < 3; ++a) {
            Vec3 u = AXES[(a + 1) % 3], v = AXES[(a + 2) % 3];
            ImVec2 prev;
            bool prevOk = false;
            for (int s = 0; s <= 48; ++s) {
                float t = s / 48.f * 6.2831853f;
                Vec3 p = origin_ + (u * std::cos(t) + v * std::sin(t)) * L;
                ImVec2 sp;
                bool ok = project(vp, winW, winH, p, sp);
                if (ok && prevOk && distToSegment(mouse, prev, sp) < kHit) return a;
                prev = sp;
                prevOk = ok;
            }
        }
    }
    return None;
}

bool Gizmo3D::frame(int winW, int winH, const Camera& cam, Vec3 origin, bool& changed,
                    bool& ended) {
    changed = false;
    ended = false;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool down = io.MouseDown[ImGuiMouseButton_Left];
    bool clicked = io.MouseClicked[ImGuiMouseButton_Left];

    if (!dragging) {
        origin_ = origin;
        // do not steal clicks meant for ImGui widgets (e.g. gizmo under the panel)
        if (io.WantCaptureMouse) {
            hover_ = None;
            return false;
        }
        hover_ = hitTest(winW, winH, cam, mouse);
        if (clicked && hover_ != None) {
            dragging = true;
            active_ = hover_;
            startOffset_ = offset;
            startRot_ = rot;
            startScale_ = scale;
            startMouse_ = mouse;
        }
        return hover_ != None;
    }

    // dragging
    if (!down) {
        dragging = false;
        active_ = None;
        ended = true;
        return false;
    }

    Mat4 vp = cam.projection(static_cast<float>(winW) / winH) * cam.view();
    float L = gizmoWorldSize(cam, winH);
    ImVec2 o;
    bool projOk = project(vp, winW, winH, origin_, o);

    if (mode == Translate && projOk) {
        ImVec2 mdelta = mouse - startMouse_;
        auto axisDelta = [&](int a) {
            ImVec2 p1;
            if (!project(vp, winW, winH, origin_ + AXES[a] * L, p1)) return 0.f;
            ImVec2 dir = norm2d(p1 - o);
            float plen = len2d(p1 - o);
            if (plen < 1.f) return 0.f;
            return dot2d(mdelta, dir) * (L / plen); // world units
        };
        Vec3 delta{0, 0, 0};
        if (active_ >= AxisX && active_ <= AxisZ) {
            (&delta.x)[active_] = axisDelta(active_);
        } else if (active_ >= PlaneXY && active_ <= PlaneXZ) {
            const int pairs[3][2] = {{0, 1}, {1, 2}, {0, 2}};
            int pl = active_ - PlaneXY;
            (&delta.x)[pairs[pl][0]] = axisDelta(pairs[pl][0]);
            (&delta.x)[pairs[pl][1]] = axisDelta(pairs[pl][1]);
        }
        offset = startOffset_ + delta;
        changed = (offset.x != startOffset_.x) || (offset.y != startOffset_.y) ||
                  (offset.z != startOffset_.z);
    } else if (mode == Rotate && projOk && active_ >= AxisX && active_ <= AxisZ) {
        float a0 = std::atan2(startMouse_.y - o.y, startMouse_.x - o.x);
        float a1 = std::atan2(mouse.y - o.y, mouse.x - o.x);
        float delta = a1 - a0;
        // sign: correct for axis facing (screen y is down)
        Vec3 toCam = (cam.eye() - origin_).normalized();
        float f = AXES[active_].dot(toCam);
        delta *= (f > 0.f ? -1.f : 1.f);
        rot = (Quat::axisAngle(AXES[active_], delta) * startRot_).normalized();
        changed = true;
    } else if (mode == Scale && projOk) {
        float r0 = std::max(len2d(startMouse_ - o), 4.f);
        float r1 = len2d(mouse - o);
        scale = std::clamp(startScale_ * (r1 / r0), 0.01f, 100.f);
        changed = std::fabs(scale - startScale_) > 1e-6f;
    }
    return true;
}

void Gizmo3D::draw(int winW, int winH, const Camera& cam) const {
    Mat4 vp = cam.projection(static_cast<float>(winW) / winH) * cam.view();
    float L = gizmoWorldSize(cam, winH);
    ImVec2 o;
    if (!project(vp, winW, winH, origin_, o)) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto col = [&](int part) { return partColor(part, part == hover_ || part == active_); };

    if (mode == Translate) {
        // plane quads
        const int pairs[3][2] = {{0, 1}, {1, 2}, {0, 2}};
        for (int pl = 0; pl < 3; ++pl) {
            Vec3 a1 = AXES[pairs[pl][0]], a2 = AXES[pairs[pl][1]];
            ImVec2 q[4];
            Vec3 cs[4] = {origin_ + a1 * (0.35f * L) + a2 * (0.35f * L),
                          origin_ + a1 * (0.65f * L) + a2 * (0.35f * L),
                          origin_ + a1 * (0.65f * L) + a2 * (0.65f * L),
                          origin_ + a1 * (0.35f * L) + a2 * (0.65f * L)};
            bool ok = true;
            for (int i = 0; i < 4; ++i) ok = ok && project(vp, winW, winH, cs[i], q[i]);
            if (!ok) continue;
            ImU32 c = col(PlaneXY + pl);
            dl->AddConvexPolyFilled(q, 4, (c & 0x00FFFFFF) | 0x30000000);
            dl->AddPolyline(q, 4, c, 0, 1.5f);
        }
        // axes
        for (int a = 0; a < 3; ++a) {
            ImVec2 p1;
            if (!project(vp, winW, winH, origin_ + AXES[a] * L, p1)) continue;
            ImU32 c = col(a);
            dl->AddLine(o, p1, c, 3.f);
            // arrow head
            ImVec2 dir = norm2d(p1 - o);
            ImVec2 nrm{-dir.y, dir.x};
            dl->AddTriangleFilled(p1 + dir * 10.f, p1 + nrm * 4.5f, p1 - nrm * 4.5f, c);
        }
    } else if (mode == Rotate) {
        for (int a = 0; a < 3; ++a) {
            Vec3 u = AXES[(a + 1) % 3], v = AXES[(a + 2) % 3];
            ImU32 c = col(a);
            ImVec2 prev;
            bool prevOk = false;
            for (int s = 0; s <= 48; ++s) {
                float t = s / 48.f * 6.2831853f;
                Vec3 p = origin_ + (u * std::cos(t) + v * std::sin(t)) * L;
                ImVec2 sp;
                bool ok = project(vp, winW, winH, p, sp);
                if (ok && prevOk) dl->AddLine(prev, sp, c, 3.f);
                prev = sp;
                prevOk = ok;
            }
        }
    } else { // Scale
        dl->AddRectFilled(o - ImVec2{6, 6}, o + ImVec2{6, 6}, col(Uniform));
        dl->AddRect(o - ImVec2{6, 6}, o + ImVec2{6, 6}, IM_COL32(30, 30, 30, 255));
    }
}

} // namespace ce
