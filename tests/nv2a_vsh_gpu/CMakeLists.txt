cmake_minimum_required(VERSION 3.20)
project(nv2a_vsh_gpu_regression C)
enable_testing()
# Windows only (D3D11; WARP, so no GPU is needed).
add_executable(nv2a-vsh-gpu-test nv2a_vsh_gpu_test.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_vsh_hlsl.c
    ../../src/kernel/nv2a_vsh_gpu.c)
target_include_directories(nv2a-vsh-gpu-test PRIVATE ../../src/kernel)
target_link_libraries(nv2a-vsh-gpu-test PRIVATE d3d11 dxguid d3dcompiler)
add_test(NAME nv2a-vsh-gpu COMMAND nv2a-vsh-gpu-test)
