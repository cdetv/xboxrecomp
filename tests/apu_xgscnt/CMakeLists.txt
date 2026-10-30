cmake_minimum_required(VERSION 3.20)
project(apu_xgscnt_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_xgscnt_test test_main.c)
target_include_directories(apu_xgscnt_test PRIVATE ../../src ../../src/apu)
# xbox_apu_emu_static first: the APU internals linked in (the .exe gets them
# from xemu_apu.dll, which exports only xemu_apu_connect).
target_link_libraries(apu_xgscnt_test PRIVATE xbox_apu_emu_static xboxrecomp)
enable_testing()
# The read caches its getenv in a static, so each arm is its own process.
# "wall" is the negative control: the old ns / 100 read must fail the same
# checks, or the "samples" arm proves nothing.
add_test(NAME apu_xgscnt_samples COMMAND apu_xgscnt_test)
set_tests_properties(apu_xgscnt_samples PROPERTIES ENVIRONMENT "APU_XGSCNT_ARM=samples")
add_test(NAME apu_xgscnt_wall COMMAND apu_xgscnt_test)
set_tests_properties(apu_xgscnt_wall PROPERTIES
                     ENVIRONMENT "RECOMP_APU_XGSCNT=wall;APU_XGSCNT_ARM=wall")
