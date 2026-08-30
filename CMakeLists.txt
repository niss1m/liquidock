cmake_minimum_required(VERSION 3.21)

project(LiquiDock
    VERSION 0.1.0
    DESCRIPTION "A macOS-style dock for Windows with a real liquid glass background"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LiquiDock targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Shader embedding
#
# Shaders are compiled at runtime with D3DCompile rather than offline with fxc.
# That buys hot reload, which is the whole point of the glass tuning work in M1:
# edit a .hlsl, save, and the dock re-renders without a restart.
#
# To keep release builds self-contained, the HLSL *source* is embedded into a
# generated header at build time. Debug builds prefer the file on disk so the
# watcher can pick up edits. M5 switches release to precompiled bytecode to cut
# the few milliseconds of startup compile.
# ---------------------------------------------------------------------------
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EMBEDDED_SHADERS "${GENERATED_DIR}/EmbeddedShaders.h")

file(GLOB SHADER_FILES CONFIGURE_DEPENDS "${SHADER_DIR}/*.hlsl" "${SHADER_DIR}/*.hlsli")

add_custom_command(
    OUTPUT "${EMBEDDED_SHADERS}"
    COMMAND ${CMAKE_COMMAND}
            -DSHADER_DIR=${SHADER_DIR}
            -DOUTPUT=${EMBEDDED_SHADERS}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    DEPENDS ${SHADER_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    COMMENT "Embedding HLSL sources"
    VERBATIM)

add_custom_target(liquidock_shaders DEPENDS "${EMBEDDED_SHADERS}")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(liquidock WIN32
    src/app/main.cpp
    src/app/AppBar.cpp
    src/app/CoverWatch.cpp
    src/app/DockLayout.cpp
    src/app/DockWindow.cpp
    src/app/EdgeTrigger.cpp
    src/app/TrayIcon.cpp
    src/core/ConfigPaths.cpp
    src/core/ConfigWatcher.cpp
    src/core/CrashHandler.cpp
    src/core/Log.cpp
    src/core/Settings.cpp
    src/gfx/GraphicsDevice.cpp
    src/gfx/CompositionTarget.cpp
    src/gfx/IconAtlas.cpp
    src/gfx/ImageTexture.cpp
    src/gfx/ShaderCache.cpp
    src/gfx/TextLayer.cpp
    src/gfx/TextureDump.cpp
    src/glass/CaptureBackdrop.cpp
    src/glass/SnapshotBackdrop.cpp
    src/glass/WallpaperBackdrop.cpp
    src/glass/FrostChain.cpp
    src/model/AppCatalog.cpp
    src/model/IconLoader.cpp
    src/model/ItemStore.cpp
    src/model/ProfileStore.cpp
    src/model/RunningState.cpp
    src/ui/GlassMenu.cpp
    src/ui/PathIcon.cpp
    src/ui/SettingsWindow.cpp
    "${EMBEDDED_SHADERS}"
    # CMake collects .manifest sources and feeds them to mt.exe through
    # `cmake -E vs_link_exe --manifests`. Passing /MANIFEST:EMBED and
    # /MANIFESTINPUT by hand instead leaves that list empty, and mt.exe fails
    # with c10100a7 "No manifest inputs were specified".
    src/app/liquidock.manifest)

add_dependencies(liquidock liquidock_shaders)

target_include_directories(liquidock PRIVATE src "${GENERATED_DIR}")

target_compile_definitions(liquidock PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # Windows 10 2004 is the floor: WDA_EXCLUDEFROMCAPTURE and the Windows
    # Graphics Capture surface we need in M3 both land there.
    _WIN32_WINNT=0x0A00
    LIQUIDOCK_VERSION="${PROJECT_VERSION}"
    LIQUIDOCK_VERSION_W=L"${PROJECT_VERSION}"
    $<$<CONFIG:Debug>:LIQUIDOCK_DEBUG>
    $<$<CONFIG:Debug>:LIQUIDOCK_SHADER_DIR="${SHADER_DIR}">)

target_link_libraries(liquidock PRIVATE
    d3d11
    dxgi
    dcomp
    d3dcompiler
    d2d1
    dwrite
    windowscodecs
    dxguid
    shcore
    shell32
    ole32
    oleaut32
    uuid
    comctl32
    dbghelp
    dwmapi
    user32
    gdi32)

if(MSVC)
    target_compile_options(liquidock PRIVATE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /MP
        $<$<CONFIG:Release>:/O2 /GL /Gy>)
    # MANIFESTUAC:NO stops the linker synthesising its own trustInfo block, so
    # the requestedExecutionLevel in liquidock.manifest is the only one and
    # mt.exe has nothing to reconcile.
    target_link_options(liquidock PRIVATE
        "/MANIFESTUAC:NO"
        # A linker map turns the RVA in a Windows Error Reporting entry into a
        # function name, which is the difference between debugging a crash and
        # guessing at one. It costs nothing at runtime and is not shipped.
        "/MAP:$<TARGET_FILE_DIR:liquidock>/liquidock.map"
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

    # Static CRT keeps the portable build a single file with no redistributable.
    set_property(TARGET liquidock PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(liquidock PROPERTIES
    OUTPUT_NAME "liquidock"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
