enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 17)
include(${CMAKE_CURRENT_LIST_DIR}/core/tools/mac/dependencies.cmake)
set(CORE ${CMAKE_CURRENT_LIST_DIR}/core/src)
add_executable(paintify-metal mac/worker.mm ${CORE}/pipeline.cpp ${CORE}/brush_atlas.cpp
    ${CORE}/paramfile.cpp ${CORE}/styles.cpp ${CORE}/app_resources.cpp ${CORE}/metal/backend.mm)
target_include_directories(paintify-metal PRIVATE ${CORE})
target_compile_definitions(paintify-metal PRIVATE SBR_METAL GLFW_INCLUDE_NONE
    SBR_ROOT_DIR="${CMAKE_CURRENT_LIST_DIR}/core"
    SBR_GLSLANG="$<TARGET_FILE:glslang-standalone>")
target_compile_options(paintify-metal PRIVATE -fobjc-arc)
target_link_libraries(paintify-metal PRIVATE sbr_imgui glm::glm spirv-cross-msl
    "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
add_dependencies(paintify-metal glslang-standalone)
