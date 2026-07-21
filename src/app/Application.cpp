#include "app/Application.h"
#include "core/GL.h"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace ce {

namespace {

std::string& droppedFile() {
    static std::string f;
    return f;
}

float& scrollAccum() {
    static float v = 0.f;
    return v;
}

void dropCallback(GLFWwindow*, int count, const char** paths) {
    if (count > 0) droppedFile() = paths[0];
}

void scrollCallback(GLFWwindow*, double, double yoff) {
    scrollAccum() += static_cast<float>(yoff);
}

} // namespace

bool Application::init(int argc, char** argv) {
    // ---- parse CLI ----
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--model") optModel_ = next();
        else if (a == "--preset") optPreset_ = next();
        else if (a == "--screenshot") optScreenshot_ = next();
        else if (a == "--frames") optFrames_ = std::atoi(next().c_str());
        else if (a == "--yaw") optYawDeg_ = std::atof(next().c_str());
        else if (a == "--dist") optDist_ = std::atof(next().c_str());
        else if (a == "--targety") optTargetY_ = std::atof(next().c_str());
        else if (a == "--set") {
            std::string s = next(); // id=value
            size_t eq = s.find('=');
            if (eq != std::string::npos)
                optSet_.emplace_back(s.substr(0, eq), std::atof(s.substr(eq + 1).c_str()));
        }
        else if (a == "--size") {
            std::string s = next();
            size_t x = s.find('x');
            if (x != std::string::npos) {
                optW_ = std::atoi(s.substr(0, x).c_str());
                optH_ = std::atoi(s.substr(x + 1).c_str());
            }
        } else if (a.size() > 4 && a[0] != '-') {
            optModel_ = a; // positional model path
        }
    }

    // ---- window & GL context ----
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    window_ = glfwCreateWindow(optW_, optH_, "Character Editor", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    if (!loadGLFunctions())
        std::fprintf(stderr, "Warning: some GL functions failed to load\n");

    // ---- ImGui ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "imgui.ini";
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.f;
    style.FrameRounding = 3.f;

    // Cyrillic-capable font (Russian UI); fall back to default if unavailable
    const char* fontCandidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
    bool fontLoaded = false;
    for (const char* f : fontCandidates) {
        if (std::filesystem::exists(f)) {
            io.Fonts->AddFontFromFileTTF(f, 17.f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            fontLoaded = true;
            break;
        }
    }
    if (!fontLoaded) io.Fonts->AddFontDefault();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- renderer ----
    std::string err;
    if (!renderer_.init(err)) {
        std::fprintf(stderr, "Renderer init failed: %s\n", err.c_str());
        return false;
    }

    glfwSetDropCallback(window_, dropCallback);
    glfwSetScrollCallback(window_, scrollCallback);

    ui_.controller = &controller_;
    ui_.presets = &presets_;
    ui_.model = &model_;
    ui_.cb.openModel = [this](const std::string& p) { loadModel(p); };
    ui_.cb.saveScreenshot = [this](const std::string& p) { saveScreenshot(p); };
    ui_.cb.resetCamera = [this]() { resetCamera(); };

    // ---- initial state ----
    if (!optModel_.empty()) loadModel(optModel_);
    if (!optPreset_.empty()) applyPreset(optPreset_);
    for (const auto& kv : optSet_) controller_.setValue(kv.first, kv.second);
    camera_.yaw = optYawDeg_ * 0.01745329252f;
    if (optDist_ > 0.f) camera_.distance = optDist_;
    if (optTargetY_ > 0.f) camera_.target.y = optTargetY_;
    if (!optScreenshot_.empty()) pendingScreenshot_ = optScreenshot_;

    return true;
}

bool Application::loadModel(const std::string& path) {
    std::string err;
    Model newModel;
    if (!GltfLoader::load(path, newModel, err)) {
        ui_.status = "Не удалось загрузить: " + err;
        std::fprintf(stderr, "Load failed: %s\n", err.c_str());
        return false;
    }
    model_ = std::move(newModel);
    skeleton_.bind(model_);
    controller_.bind(model_, skeleton_);
    controller_.scan("config/body_params.json");
    renderer_.upload(model_);
    controller_.morphsDirty = true;
    ui_.status = "Загружено: " + model_.fileName;
    return true;
}

void Application::applyPreset(const std::string& name) {
    std::string err;
    std::map<std::string, float> vals;
    if (presets_.load(name, vals, err)) {
        controller_.setValues(vals);
        ui_.status = "Пресет загружен: " + name;
    } else {
        ui_.status = "Пресет не найден: " + err;
    }
}

void Application::resetCamera() {
    camera_ = Camera{};
}

void Application::saveScreenshot(const std::string& path) {
    pendingScreenshot_ = path; // captured after the next scene render
}

void Application::handleInput(float /*dt*/) {
    // file dropped onto the window
    if (!droppedFile().empty()) {
        loadModel(droppedFile());
        droppedFile().clear();
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;

    static double prevX = 0, prevY = 0;
    double x, y;
    glfwGetCursorPos(window_, &x, &y);
    double dx = x - prevX, dy = y - prevY;
    prevX = x;
    prevY = y;

    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
        camera_.rotate(static_cast<float>(dx), static_cast<float>(dy));
    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
        glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
        camera_.pan(static_cast<float>(dx), static_cast<float>(dy));

    if (scrollAccum() != 0.f) {
        camera_.zoom(scrollAccum());
        scrollAccum() = 0.f;
    }
}

int Application::run() {
    int framesLeft = optFrames_;
    double prevTime = glfwGetTime();
    float fps = 60.f;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();

        double now = glfwGetTime();
        float dt = static_cast<float>(now - prevTime);
        prevTime = now;
        if (dt > 0.f) fps = fps * 0.95f + (1.f / dt) * 0.05f;

        int winW, winH;
        glfwGetFramebufferSize(window_, &winW, &winH);
        if (winW == 0 || winH == 0) continue;
        float aspect = static_cast<float>(winW) / static_cast<float>(winH);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        handleInput(dt);

        skeleton_.update();
        if (controller_.morphsDirty) {
            renderer_.syncMorphs(model_);
            controller_.morphsDirty = false;
        }

        // ---- render scene ----
        glViewport(0, 0, winW, winH);
        glClearColor(0.11f, 0.12f, 0.14f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (ui_.showGrid) renderer_.drawGrid(camera_, aspect);
        if (renderer_.hasModel())
            renderer_.draw(model_, skeleton_, camera_, aspect, ui_.wireframe);

        // screenshot of the pure 3D scene (before UI is drawn)
        if (!pendingScreenshot_.empty()) {
            std::vector<unsigned char> px(static_cast<size_t>(winW) * winH * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, winW, winH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            std::vector<unsigned char> flipped(px.size()); // GL rows are bottom-up
            size_t row = static_cast<size_t>(winW) * 3;
            for (int y = 0; y < winH; ++y)
                std::memcpy(flipped.data() + (winH - 1 - y) * row, px.data() + y * row, row);
            if (stbi_write_png(pendingScreenshot_.c_str(), winW, winH, 3, flipped.data(),
                               static_cast<int>(row)))
                ui_.status = "Скриншот: " + pendingScreenshot_;
            else
                ui_.status = "Не удалось сохранить скриншот";
            pendingScreenshot_.clear();
        }

        ui_.draw(winW, winH, fps);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);

        if (framesLeft > 0 && --framesLeft == 0)
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    renderer_.release();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

} // namespace ce
