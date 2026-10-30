# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough
#
# Three targets (apu_host.h explains the split):
#   xbox_apu_emu  the APU itself (LGPL, from xemu) + its audio output
#   xemu_apu      Windows: those objects as xemu_apu.dll, which the game's
#                 build copies next to its .exe (templates/new-game)
#   xbox_apu      what the .exe links: the host table, the DLL loader and
#                 the register trap; elsewhere the APU objects themselves
# xbox_apu_emu_static: the APU objects as a static library, for tests that
# call into the APU directly.

add_library(xbox_apu_emu OBJECT
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_xaudio2.c
    apu_dll.c
)

target_include_directories(xbox_apu_emu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu_emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

# getenv and the log through the .exe (apu_dll_io.h); built for the DLL.
target_compile_definitions(xbox_apu_emu PRIVATE XEMU_APU_BUILD)
set_target_properties(xbox_apu_emu PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(xbox_apu_emu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
        /FI${CMAKE_CURRENT_SOURCE_DIR}/apu_dll_io.h
    )
    target_compile_definitions(xbox_apu_emu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
else()
    target_compile_options(xbox_apu_emu PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/apu_dll_io.h)
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    set(_apu_emu_libs platform kernel32 winmm xaudio2_8 ole32)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    set(_apu_emu_libs platform SDL2::SDL2 m)
endif()
target_link_libraries(xbox_apu_emu PRIVATE ${_apu_emu_libs})

add_library(xbox_apu_emu_static STATIC $<TARGET_OBJECTS:xbox_apu_emu>)
target_include_directories(xbox_apu_emu_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_apu_emu_static PUBLIC ${_apu_emu_libs})

add_library(xbox_apu STATIC
    apu_host.c
    apu_loader.c
    apu_mmio_hook.c
)
target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_apu PRIVATE platform)

if(WIN32)
    add_library(xemu_apu SHARED $<TARGET_OBJECTS:xbox_apu_emu>)
    target_link_libraries(xemu_apu PRIVATE ${_apu_emu_libs})
    # Loaded at run time, not linked: built whenever the runtime is.
    add_dependencies(xbox_apu xemu_apu)
else()
    target_sources(xbox_apu PRIVATE $<TARGET_OBJECTS:xbox_apu_emu>)
    target_link_libraries(xbox_apu PUBLIC ${_apu_emu_libs})
endif()

if(MSVC)
    target_compile_options(xbox_apu PRIVATE /W3)
    target_compile_definitions(xbox_apu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
