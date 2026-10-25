# kbm_pad

Keyboard and mouse as player 1's pad (`src/input/kbm_pad.c`). Checks the
bindings (keys to buttons and sticks), the shortest press (a tap lasts
about two frames), wheel notches as separate presses, walking tilt (the
same on a diagonal), the mouse as right-stick speed placed in the title's
own stick range past its dead zone, inverted looking, and the weapon-wheel
mode (a held key makes the mouse steer the left stick).

    cmake -S tests/kbm_pad -B build/kbm-pad -G Ninja
    cmake --build build/kbm-pad
    ctest --test-dir build/kbm-pad --output-on-failure
