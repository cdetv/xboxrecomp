/*
 * Forced into every file of xemu_apu.dll (CMakeLists.txt, /FI): its getenv
 * and its log lines go through the .exe (apu_host.h says why). Included
 * first, so the C runtime's own declarations are already read and only the
 * calls are renamed. Other files (the WAV dump) are written directly.
 *
 * The toolkit's own file (MIT).
 */
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

char *apu_dll_getenv(const char *name);
int   apu_dll_fprintf(FILE *f, const char *fmt, ...);
int   apu_dll_fflush(FILE *f);

#define getenv  apu_dll_getenv
#define fprintf apu_dll_fprintf
#define fflush  apu_dll_fflush
