cmake_minimum_required(VERSION 3.20)
project(character_editor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------- GLFW ----------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(external/glfw)

# ---------- Dear ImGui ----------
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends
)
target_link_libraries(imgui PUBLIC glfw)

# ---------- Editor ----------
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(character_editor ${EDITOR_SOURCES})

target_include_directories(character_editor PRIVATE
    src
    external/tinygltf
)
target_link_libraries(character_editor PRIVATE imgui glfw opengl32 comdlg32)

if(MINGW)
    target_compile_options(character_editor PRIVATE -Wno-unused-parameter)
endif()

# Run from the project root so relative asset paths (female_base.vrm, config/, presets/) resolve.
set_target_properties(character_editor PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
