cmake_minimum_required(VERSION 3.20)
project(apu_mixdown_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_mixdown_test test_main.c)
target_include_directories(apu_mixdown_test PRIVATE ../../src ../../src/apu)
# The whole runtime: the APU delivers its interrupt and resolves physical
# addresses through the kernel.
target_link_libraries(apu_mixdown_test PRIVATE xboxrecomp)
enable_testing()
# One test per mixdown arm. The mixdown caches its getenv in a static, so one
# process can only exercise one arm -- and an arm that cannot fail is not a
# test. "evenodd" and "front" are the negative controls: each must show the
# defect the speakers table replaces, or the speakers arm proves nothing.
add_test(NAME apu_mixdown_speakers COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_speakers PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN=speakers;APU_MIXDOWN_ARM=speakers")
add_test(NAME apu_mixdown_evenodd COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_evenodd PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN=evenodd;APU_MIXDOWN_ARM=evenodd")
add_test(NAME apu_mixdown_two_bins COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_two_bins PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MIXDOWN_ALL=0;APU_MIXDOWN_ARM=front")
# The VP monitor mix (the default, RECOMP_APU_MIXDOWN=xemu): a stereo voice's
# centre column on both sides. "monitor_old" is its negative control.
add_test(NAME apu_mixdown_monitor COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_monitor PROPERTIES
                     ENVIRONMENT "APU_MIXDOWN_ARM=monitor")
add_test(NAME apu_mixdown_monitor_old COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_monitor_old PROPERTIES
                     ENVIRONMENT "RECOMP_APU_MONITOR_STEREO=old;APU_MIXDOWN_ARM=monitor_old")
