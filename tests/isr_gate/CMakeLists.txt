cmake_minimum_required(VERSION 3.20)
project(isr_gate_test C)
if(NOT WIN32)
    message(FATAL_ERROR "The dispatch gate is a Win32 critical section")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(isr_gate_test test_main.c)
target_include_directories(isr_gate_test PRIVATE ../../src/kernel)
target_link_libraries(isr_gate_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME isr_gate_try COMMAND isr_gate_test)
# Negative control: with the gate switched off, the try must not report busy,
# or "busy" in the arm above could come from something other than the holder.
add_test(NAME isr_gate_off COMMAND isr_gate_test)
set_tests_properties(isr_gate_off PROPERTIES
                     ENVIRONMENT "RECOMP_ISR_GATE=0;ISR_GATE_EXPECT_OFF=1")
