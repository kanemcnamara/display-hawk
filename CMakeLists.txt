cmake_minimum_required(VERSION 3.21)
project(DisplayHawk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# QRhi (Qt's Metal / Direct3D / Vulkan / OpenGL abstraction) needs Qt 6.6+ and the GuiPrivate target.
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network ShaderTools)
qt_standard_project_setup()

qt_add_executable(DisplayHawk
    src/main.cpp
    src/model/Project.h
    src/model/Project.cpp
    src/model/Document.h
    src/model/CabinetMapping.h
    src/model/CabinetMapping.cpp
    src/model/Document.cpp
    src/render/Patterns.h
    src/render/Patterns.cpp
    src/render/Effects.h
    src/render/Effects.cpp
    src/render/EffectRenderer.h
    src/render/EffectRenderer.cpp
    src/render/RhiQuad.h
    src/render/RhiQuad.cpp
    src/render/RenderEngine.h
    src/render/RenderEngine.cpp
    src/render/RenderThread.h
    src/render/RenderThread.cpp
    src/render/YuvConverter.h
    src/render/YuvConverter.cpp
    src/audio/AudioAnalyzer.h
    src/audio/AudioAnalyzer.cpp
    src/audio/AudioReactive.h
    src/audio/AudioReactive.cpp
    src/control/Osc.h
    src/control/Osc.cpp
    src/control/ArtNet.h
    src/control/ArtNet.cpp
    src/control/Sacn.h
    src/control/Sacn.cpp
    src/control/Midi.h
    src/control/Midi.cpp
    src/control/ControlRouter.h
    src/control/AutopilotController.h
    src/control/TempoClock.h
    src/control/TempoClock.cpp
    src/control/ModulationHub.h
    src/control/ModulationHub.cpp
    src/control/AutopilotController.cpp
    src/control/ControlRouter.cpp
    src/media/MediaSource.h
    src/media/MediaSource.cpp
    src/media/Snappy.h
    src/media/Snappy.cpp
    src/media/HapDecoder.h
    src/media/HapDecoder.cpp
    src/media/MovReader.h
    src/media/MovReader.cpp
    src/media/Ndi.h
    src/media/Ndi.cpp
    src/media/HapPlayer.h
    src/media/HapPlayer.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/CanvasView.h
    src/ui/CanvasView.cpp
    src/ui/AudioReactiveDialog.h
    src/ui/AudioReactiveDialog.cpp
    src/ui/CabinetLayoutEditor.h
    src/ui/CabinetLayoutEditor.cpp
    src/ui/LfoDialog.h
    src/ui/LfoDialog.cpp
    src/ui/MaskEditor.h
    src/ui/MaskEditor.cpp
    src/ui/EffectChainEditor.h
    src/ui/EffectChainEditor.cpp
    src/ui/ControlDialog.h
    src/ui/ControlDialog.cpp
    src/ui/ClipDeck.h
    src/ui/ClipDeck.cpp
    src/ui/IdentifyOverlay.h
    src/ui/IdentifyOverlay.cpp
    src/ui/PanelGridEditor.h
    src/ui/PanelGridEditor.cpp
    src/ui/OutputWindow.h
    src/ui/OutputWindow.cpp
    src/ui/ReportExporter.h
    src/ui/ReportExporter.cpp
    src/ui/PropertyPanel.h
    src/ui/PropertyPanel.cpp
)

# App icon: embedded in the .exe on Windows, bundled as an .icns on macOS, and used for every window at runtime.
if(WIN32)
    target_sources(DisplayHawk PRIVATE src/app.rc)
elseif(APPLE)
    set(DISPLAYHAWK_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/displayhawk.icns")
    target_sources(DisplayHawk PRIVATE "${DISPLAYHAWK_ICNS}")
    set_source_files_properties("${DISPLAYHAWK_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
qt_add_resources(DisplayHawk "icons"
    PREFIX "/"
    FILES assets/icon/displayhawk.png
)

set_target_properties(DisplayHawk PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayhawk.app"
    MACOSX_BUNDLE_ICON_FILE "displayhawk.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/macos/Info.plist.in"
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
        shaders/yuv.vert
        shaders/yuv.frag
        shaders/mesh.vert
        shaders/fx.vert
        shaders/fx.frag
)

target_include_directories(DisplayHawk PRIVATE src)

target_link_libraries(DisplayHawk PRIVATE
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Multimedia Qt6::Network
)

# GPU texture sharing with other apps: Spout (Windows, vendored SpoutDX) / Syphon (macOS, vendored).
target_sources(DisplayHawk PRIVATE src/media/TextureShare.h)
if(WIN32)
    file(GLOB SPOUT_SOURCES CONFIGURE_DEPENDS third_party/spout/*.cpp)
    target_sources(DisplayHawk PRIVATE src/media/TextureShareSpout.cpp ${SPOUT_SOURCES})
    target_include_directories(DisplayHawk PRIVATE third_party/spout)
    target_link_libraries(DisplayHawk PRIVATE d3d11 dxgi psapi shell32 advapi32 version comctl32)
    if(NOT MSVC)
        # SpoutCopy uses SSSE3 intrinsics (guarded by a runtime CPU check); GCC needs them enabled.
        set_source_files_properties(third_party/spout/SpoutCopy.cpp PROPERTIES COMPILE_OPTIONS "-mssse3")
    endif()
elseif(APPLE)
    enable_language(C OBJC OBJCXX)
    file(GLOB SYPHON_SOURCES CONFIGURE_DEPENDS third_party/syphon/Syphon/*.m third_party/syphon/Syphon/*.c)
    target_sources(DisplayHawk PRIVATE src/media/TextureShareSyphon.mm ${SYPHON_SOURCES})
    target_include_directories(DisplayHawk PRIVATE third_party/syphon third_party/syphon/Syphon)
    # Built like Syphon's Xcode target: ARC, plus its prefix header (Cocoa import, SYPHONLOG).
    set_source_files_properties(${SYPHON_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-fobjc-arc;-include;${CMAKE_CURRENT_SOURCE_DIR}/third_party/syphon/Syphon/Syphon_Prefix.pch")
    set_source_files_properties(src/media/TextureShareSyphon.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(DisplayHawk PRIVATE "-framework Foundation" "-framework AppKit" "-framework Metal" "-framework IOSurface")
else()
    target_sources(DisplayHawk PRIVATE src/media/TextureShareNone.cpp)
endif()

# MIDI input uses the OS API directly.
if(WIN32)
    target_link_libraries(DisplayHawk PRIVATE winmm)
elseif(APPLE)
    target_link_libraries(DisplayHawk PRIVATE "-framework CoreMIDI" "-framework CoreFoundation")
endif()
