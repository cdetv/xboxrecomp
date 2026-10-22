cmake_minimum_required(VERSION 3.20)
project(postfx_scale_regression C)
enable_testing()
# Windows only (D3D11; WARP, so no GPU is needed). The filter chain with its
# vendored shader texts built in, as src/kernel/CMakeLists.txt does.
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(texts)
foreach(pair "fxaa|fxaa/Fxaa3_11.h" "smaa|smaa/SMAA.hlsl"
             "ffx_a|fsr1/ffx_a.h" "ffx_fsr1|fsr1/ffx_fsr1.h")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 name)
    list(GET pair 1 in)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/postfx_text_${name}.c")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} "-DIN=${ROOT}/third_party/${in}"
            "-DOUT=${out}" "-DNAME=postfx_text_${name}"
            -P "${ROOT}/cmake/embed_text.cmake"
        DEPENDS "${ROOT}/third_party/${in}" "${ROOT}/cmake/embed_text.cmake"
        VERBATIM)
    list(APPEND texts "${out}")
endforeach()
add_executable(postfx-scale-test postfx_scale_test.c
    ../../src/kernel/postfx_d3d11.c ${texts})
target_include_directories(postfx-scale-test PRIVATE ../../src/kernel
    ../../third_party)
target_link_libraries(postfx-scale-test PRIVATE d3d11 dxguid d3dcompiler)
add_test(NAME postfx-scale COMMAND postfx-scale-test)
