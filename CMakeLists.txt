cmake_minimum_required(VERSION 3.21)
project(DisplayHawk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QRhi (Qt's Metal / Direct3D / Vulkan / OpenGL abstraction) needs Qt 6.6+ and the GuiPrivate target.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Multimedia ShaderTools)
qt_standard_project_setup()

qt_add_executable(DisplayHawk
    src/main.cpp
    src/model/Project.h
    src/model/Project.cpp
    src/model/Document.h
    src/model/Document.cpp
    src/render/Patterns.h
    src/render/Patterns.cpp
    src/render/RhiQuad.h
    src/render/RhiQuad.cpp
    src/render/RenderEngine.h
    src/render/RenderEngine.cpp
    src/render/RenderThread.h
    src/render/RenderThread.cpp
    src/media/MediaSource.h
    src/media/MediaSource.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/CanvasView.h
    src/ui/CanvasView.cpp
    src/ui/IdentifyOverlay.h
    src/ui/IdentifyOverlay.cpp
    src/ui/OutputWindow.h
    src/ui/OutputWindow.cpp
    src/ui/PropertyPanel.h
    src/ui/PropertyPanel.cpp
)

set_target_properties(DisplayHawk PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayhawk.app"
    MACOSX_BUNDLE_BUNDLE_NAME "Display Hawk"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)

# Shaders are written once in Vulkan-style GLSL and compiled to SPIR-V, HLSL, MSL and GLSL at build time.
qt_add_shaders(DisplayHawk "shaders"
    PREFIX "/"
    GLSL "330"
    HLSL 50
    MSL 12
    FILES
        shaders/quad.vert
        shaders/quad.frag
)

target_include_directories(DisplayHawk PRIVATE src)

target_link_libraries(DisplayHawk PRIVATE
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Multimedia
)
