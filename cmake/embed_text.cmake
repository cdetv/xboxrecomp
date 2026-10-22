# A text file as a C array, for sources that are compiled at run time (the
# filter chain's FXAA and SMAA shaders, src/kernel/postfx_d3d11.c):
#
#   cmake -DIN=<file> -DOUT=<file.c> -DNAME=<symbol> -P embed_text.cmake
#
# writes  const unsigned char NAME[] = {..., 0};  const unsigned NAME_len = <bytes>;
# Bytes as they are on disk, line endings included; the 0 ends the string
# and is not counted.

file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR bytes "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
# 16 bytes a line (CMake regexes allow few groups, so one built by repeat).
string(REPEAT "0x[0-9a-f][0-9a-f]," 16 line)
string(REGEX REPLACE "(${line})" "\\1\n" hex "${hex}")
file(WRITE "${OUT}"
"/* Generated from ${IN} by cmake/embed_text.cmake; do not edit. */\n"
"const unsigned char ${NAME}[] = {\n${hex}0};\n"
"const unsigned ${NAME}_len = ${bytes};\n")
