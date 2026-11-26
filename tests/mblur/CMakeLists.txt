cmake_minimum_required(VERSION 3.20)
project(mblur_regression C)
enable_testing()
# Windows only (D3D11; WARP, so no GPU is needed): the host motion blur
# (src/kernel/mblur_d3d11.c) against the same maths done on the CPU.
add_executable(mblur-test mblur_test.c ../../src/kernel/mblur_d3d11.c)
target_include_directories(mblur-test PRIVATE ../../src/kernel)
target_link_libraries(mblur-test PRIVATE d3d11 dxguid d3dcompiler)
add_test(NAME mblur COMMAND mblur-test)
