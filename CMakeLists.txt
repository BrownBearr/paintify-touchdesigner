cmake_minimum_required(VERSION 3.21)
# Apple Silicon Macs shipped with macOS 11, and nothing here needs newer.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(gpu-sbr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# ── GPU backend ───────────────────────────────────────────────────────────────
# GL is OpenGL 4.6, which Spout needs on Windows. VULKAN runs on macOS through
# MoltenVK: Apple's OpenGL stops at 4.1 and has no compute shaders, so the GL
# backend cannot run there at all. Both backends run the same shaders.
if(APPLE)
    set(PAINTIFY_GPU_DEFAULT VULKAN)
else()
    set(PAINTIFY_GPU_DEFAULT GL)
endif()
set(PAINTIFY_GPU ${PAINTIFY_GPU_DEFAULT} CACHE STRING "GPU backend: GL or VULKAN")
set_property(CACHE PAINTIFY_GPU PROPERTY STRINGS GL VULKAN)
if(APPLE AND NOT PAINTIFY_GPU STREQUAL "VULKAN")
    message(FATAL_ERROR "macOS has no OpenGL 4.6 (it stops at 4.1, without compute "
                        "shaders). Build with -DPAINTIFY_GPU=VULKAN.")
endif()
message(STATUS "Paintify GPU backend: ${PAINTIFY_GPU}")

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
# vcpkg (Windows) supplies everything. Elsewhere glfw, Vulkan and glslang come
# from the system package manager, and the two source-only libraries -- stb
# and Dear ImGui -- are fetched at pinned versions when they are not found.
find_package(glfw3 CONFIG REQUIRED)

find_path(STB_INCLUDE_DIRS "stb_image.h")
if(NOT STB_INCLUDE_DIRS)
    FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20)
    FetchContent_MakeAvailable(stb)
    set(STB_INCLUDE_DIRS ${stb_SOURCE_DIR})
endif()

if(PAINTIFY_GPU STREQUAL "GL")
    find_package(glad CONFIG REQUIRED)
    find_package(imgui CONFIG QUIET)
else()
    find_package(Vulkan REQUIRED)
    find_package(glslang CONFIG REQUIRED)
endif()

# The Vulkan build always uses its own ImGui, pinned, because the Vulkan
# backend's init struct changes between ImGui releases. 1.91.9 is also what the
# vcpkg baseline in vcpkg.json resolves to.
if(NOT TARGET imgui::imgui)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.9
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)
    if(PAINTIFY_GPU STREQUAL "GL")
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    else()
        list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    endif()
    add_library(paintify_imgui STATIC ${IMGUI_SOURCES})
    target_include_directories(paintify_imgui PUBLIC ${imgui_SOURCE_DIR}
                                                     ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(paintify_imgui PUBLIC glfw)
    if(PAINTIFY_GPU STREQUAL "VULKAN")
        target_link_libraries(paintify_imgui PUBLIC Vulkan::Vulkan)
    endif()
    add_library(imgui::imgui ALIAS paintify_imgui)
endif()

if(WIN32)
    find_package(Spout2 CONFIG REQUIRED)
endif()

# Syphon, for the TouchDesigner bridge on macOS. Only its base classes are
# built: they hand out raw IOSurfaces, so neither Syphon's OpenGL nor its Metal
# renderer is needed, and neither is Xcode's Metal compiler. The pinned commit
# is the first with the public SyphonSubclassing.h API that this relies on.
if(APPLE)
    FetchContent_Declare(syphon
        GIT_REPOSITORY https://github.com/Syphon/Syphon-Framework.git
        GIT_TAG        f4761677a45b8034a3c2069ec0f3d2553da81fba)
    FetchContent_MakeAvailable(syphon)

    # Syphon's headers include each other as <Syphon/...>, framework style.
    set(SYPHON_INCLUDE_ROOT ${CMAKE_BINARY_DIR}/syphon-include)
    file(MAKE_DIRECTORY ${SYPHON_INCLUDE_ROOT})
    if(NOT EXISTS ${SYPHON_INCLUDE_ROOT}/Syphon)
        file(CREATE_LINK ${syphon_SOURCE_DIR} ${SYPHON_INCLUDE_ROOT}/Syphon SYMBOLIC)
    endif()

    add_library(syphon_base STATIC
        ${syphon_SOURCE_DIR}/SyphonServerBase.m
        ${syphon_SOURCE_DIR}/SyphonClientBase.m
        ${syphon_SOURCE_DIR}/SyphonServerDirectory.m
        ${syphon_SOURCE_DIR}/SyphonPrivate.m
        ${syphon_SOURCE_DIR}/SyphonServerConnectionManager.m
        ${syphon_SOURCE_DIR}/SyphonClientConnectionManager.m
        ${syphon_SOURCE_DIR}/SyphonMessaging.m
        ${syphon_SOURCE_DIR}/SyphonMessageQueue.m
        ${syphon_SOURCE_DIR}/SyphonMessageReceiver.m
        ${syphon_SOURCE_DIR}/SyphonMessageSender.m
        ${syphon_SOURCE_DIR}/SyphonCFMessageReceiver.m
        ${syphon_SOURCE_DIR}/SyphonCFMessageSender.m
        ${syphon_SOURCE_DIR}/SyphonDispatch.c)
    target_include_directories(syphon_base PUBLIC ${SYPHON_INCLUDE_ROOT}
                                           PRIVATE ${syphon_SOURCE_DIR})
    # What Syphon's Xcode project sets: ARC, and its prefix header.
    target_compile_options(syphon_base PRIVATE
        $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>
        -include ${syphon_SOURCE_DIR}/Syphon_Prefix.pch
        -Wno-deprecated-declarations)
    target_link_libraries(syphon_base PUBLIC
        "-framework Foundation" "-framework Cocoa" "-framework IOSurface"
        "-framework CoreVideo")
endif()

# ── The renderer ──────────────────────────────────────────────────────────────
add_executable(gpu-sbr
    src/main.cpp
    src/pipeline.cpp
    src/gpu_program.cpp
    src/shader_source.cpp
    src/brush_atlas.cpp
    src/media.cpp
    src/jobs.cpp
    src/paramfile.cpp
)

if(PAINTIFY_GPU STREQUAL "GL")
    target_sources(gpu-sbr PRIVATE src/gpu_gl.cpp src/display_gl.cpp)
    target_link_libraries(gpu-sbr PRIVATE glad::glad)
    target_compile_definitions(gpu-sbr PRIVATE PAINTIFY_GL=1)
else()
    target_sources(gpu-sbr PRIVATE src/gpu_vk.cpp src/display_vk.cpp)
    target_link_libraries(gpu-sbr PRIVATE Vulkan::Vulkan glslang::glslang
                                          glslang::glslang-default-resource-limits)
    if(TARGET glslang::SPIRV)
        target_link_libraries(gpu-sbr PRIVATE glslang::SPIRV)
    endif()
    target_compile_definitions(gpu-sbr PRIVATE PAINTIFY_VULKAN=1)
endif()

if(APPLE)
    target_sources(gpu-sbr PRIVATE src/filedialog_mac.mm src/live_syphon.mm)
    set_source_files_properties(src/filedialog_mac.mm src/live_syphon.mm
                                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(gpu-sbr PRIVATE syphon_base "-framework Cocoa"
                                          "-framework IOSurface")
else()
    target_sources(gpu-sbr PRIVATE src/filedialog.cpp src/live_spout.cpp)
endif()

target_include_directories(gpu-sbr PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(gpu-sbr PRIVATE glfw imgui::imgui)
if(UNIX AND NOT APPLE)
    target_link_libraries(gpu-sbr PRIVATE ${CMAKE_DL_LIBS})
endif()

if(APPLE)
    # The live bridge end to end: a Syphon test pattern in, painted frames out.
    add_executable(paintify-syphon-smoke tests/syphon_smoke.mm)
    set_source_files_properties(tests/syphon_smoke.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(paintify-syphon-smoke PRIVATE syphon_base)
    add_dependencies(paintify-syphon-smoke gpu-sbr)
endif()

if(WIN32)
    target_link_libraries(gpu-sbr PRIVATE Spout2::Spout)

    add_executable(paintify-spout-smoke tests/spout_smoke.cpp)
    target_link_libraries(paintify-spout-smoke PRIVATE glfw glad::glad Spout2::Spout)
    target_compile_definitions(paintify-spout-smoke PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(gpu-sbr PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(gpu-sbr PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Shaders and brushes are loaded at runtime; point the exe at the source tree so
# hot-reload edits take effect without a rebuild. PAINTIFY_ROOT overrides it.
target_compile_definitions(gpu-sbr PRIVATE SBR_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
