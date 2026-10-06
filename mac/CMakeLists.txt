# paintify-mac: the Metal build of the renderer, for macOS.
#
#   brew install cmake glslang spirv-cross
#   cmake -S . -B build-mac -DSYPHON_FRAMEWORK_DIR=/path/containing/Syphon.framework
#   cmake --build build-mac
#
# tools/build-mac.sh wraps this and also fetches/builds Syphon.

# --- shaders: GLSL -> SPIR-V -> MSL -> metallib -----------------------------
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(SPIRV_CROSS spirv-cross REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND xcrun -sdk macosx --find metal OUTPUT_VARIABLE METAL_COMPILER
                OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE METAL_RESULT)
if(NOT METAL_RESULT EQUAL 0)
    message(FATAL_ERROR "The Metal shader compiler was not found. Install Xcode "
                        "(not only the Command Line Tools) and run "
                        "`sudo xcode-select -s /Applications/Xcode.app`.")
endif()

set(MSL_DIR ${CMAKE_CURRENT_BINARY_DIR}/msl)
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
file(GLOB GLSL_SOURCES ${SHADER_SRC_DIR}/*.comp ${SHADER_SRC_DIR}/*.vert
                       ${SHADER_SRC_DIR}/*.frag ${SHADER_SRC_DIR}/*.glsl)

set(MSL_STAMP ${MSL_DIR}/translated.stamp)
add_custom_command(
    OUTPUT ${MSL_STAMP}
    COMMAND ${CMAKE_COMMAND} -E env
            GLSLANG=${GLSLANG_VALIDATOR} SPIRV_CROSS=${SPIRV_CROSS}
            ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/glsl2msl.py
            ${SHADER_SRC_DIR} ${MSL_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${MSL_STAMP}
    DEPENDS ${GLSL_SOURCES} ${CMAKE_SOURCE_DIR}/tools/glsl2msl.py
    COMMENT "Translating GLSL shaders to MSL"
    VERBATIM)

# One MSL file per translated shader (named <shader>_<stage>.metal) plus the
# hand-written helpers.
set(TRANSLATED_NAMES blur_comp canvas_comp energy_comp error_comp etf_comp
                     features_comp flow_comp impasto_comp pool_comp relax_comp
                     seeds_comp stroke_frag stroke_vert trace_comp)
set(AIR_FILES)
foreach(name ${TRANSLATED_NAMES} util)
    if(name STREQUAL "util")
        set(msl_file ${CMAKE_CURRENT_SOURCE_DIR}/shaders/util.metal)
        set(deps ${msl_file})
    else()
        set(msl_file ${MSL_DIR}/${name}.metal)
        set(deps ${MSL_STAMP})
    endif()
    set(air_file ${MSL_DIR}/${name}.air)
    add_custom_command(
        OUTPUT ${air_file}
        COMMAND xcrun -sdk macosx metal -c ${msl_file} -o ${air_file}
                -std=metal2.3 -mmacosx-version-min=11.0 -fno-fast-math
        DEPENDS ${deps}
        COMMENT "Compiling ${name}.metal"
        VERBATIM)
    list(APPEND AIR_FILES ${air_file})
endforeach()

set(METALLIB ${CMAKE_CURRENT_BINARY_DIR}/paintify.metallib)
add_custom_command(
    OUTPUT ${METALLIB}
    COMMAND xcrun -sdk macosx metallib ${AIR_FILES} -o ${METALLIB}
    DEPENDS ${AIR_FILES}
    COMMENT "Linking paintify.metallib"
    VERBATIM)
add_custom_target(paintify-metallib ALL DEPENDS ${METALLIB})

# --- Syphon -----------------------------------------------------------------
set(SYPHON_FRAMEWORK_DIR "" CACHE PATH "Directory containing Syphon.framework")
if(NOT SYPHON_FRAMEWORK_DIR OR NOT EXISTS "${SYPHON_FRAMEWORK_DIR}/Syphon.framework")
    message(FATAL_ERROR "Set -DSYPHON_FRAMEWORK_DIR=<dir containing Syphon.framework> "
                        "(tools/build-mac.sh fetches and builds it).")
endif()

# --- the executable ---------------------------------------------------------
add_executable(paintify-mac
    main_mac.mm
    metal_pipeline.mm
    syphon_live.mm
    ${CMAKE_SOURCE_DIR}/src/options.cpp
    ${CMAKE_SOURCE_DIR}/src/paramfile.cpp
    ${CMAKE_SOURCE_DIR}/src/brush_atlas.cpp)
add_dependencies(paintify-mac paintify-metallib)

target_include_directories(paintify-mac PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(main_mac.mm metal_pipeline.mm syphon_live.mm
                            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(paintify-mac PRIVATE -F${SYPHON_FRAMEWORK_DIR})
target_link_options(paintify-mac PRIVATE -F${SYPHON_FRAMEWORK_DIR})
target_link_libraries(paintify-mac PRIVATE
    "-framework Syphon" "-framework Metal" "-framework Foundation"
    "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")

# The framework travels next to the executable (see tools/build-mac.sh).
set_target_properties(paintify-mac PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@executable_path/Frameworks")

add_custom_command(TARGET paintify-mac POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:paintify-mac>/Frameworks
    COMMAND rm -rf $<TARGET_FILE_DIR:paintify-mac>/Frameworks/Syphon.framework
    COMMAND cp -R ${SYPHON_FRAMEWORK_DIR}/Syphon.framework
                  $<TARGET_FILE_DIR:paintify-mac>/Frameworks/
    COMMAND ${CMAKE_COMMAND} -E copy ${METALLIB} $<TARGET_FILE_DIR:paintify-mac>/paintify.metallib
    COMMENT "Bundling Syphon.framework and paintify.metallib"
    VERBATIM)
