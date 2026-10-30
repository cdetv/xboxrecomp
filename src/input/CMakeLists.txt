# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    kbm_pad.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform xinput)
    target_sources(xbox_input PRIVATE sdl_pads.c)
    # PlayStation pads (sdl_pads.c): the game project builds SDL3 and names
    # its target here, e.g. SDL3::SDL3-static. Without one, XInput only.
    set(XBOXRECOMP_SDL3_TARGET "" CACHE STRING
        "SDL3 target for PlayStation controllers (empty: XInput only)")
    if(XBOXRECOMP_SDL3_TARGET)
        if(NOT TARGET ${XBOXRECOMP_SDL3_TARGET})
            message(FATAL_ERROR "XBOXRECOMP_SDL3_TARGET: no target "
                                "${XBOXRECOMP_SDL3_TARGET}")
        endif()
        target_link_libraries(xbox_input PRIVATE ${XBOXRECOMP_SDL3_TARGET})
        target_compile_definitions(xbox_input PRIVATE XBOX_INPUT_SDL3=1)
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
