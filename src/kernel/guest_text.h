/*
 * guest_text.h - a title's 8-bit text <-> UTF-16, as the Xbox kernel does it.
 *
 * The Xbox has one 8-bit character set: byte 0xNN is U+00NN (Latin-1), and a
 * character past U+00FF has no byte, so it becomes '?'. One byte per character
 * both ways, so a title's buffer sizes (Rtl*String sizes its result one byte or
 * one WCHAR per character) always hold.
 *
 * The PC's code page has no say here. Converting with CP_ACP made a title's text
 * depend on the player's Windows: on a Japanese PC (932) a byte past 0x7F starts
 * a two-byte character, and in a process whose manifest asks for a UTF-8 code
 * page (Windows 10 1903+, the way a game takes folder names with any letters)
 * 'é' turns into two bytes, or into U+FFFD the other way. The POSIX shim's
 * CP_ACP has always been Latin-1 (platform/win32_compat.c); this is the same.
 *
 * Drop-in for MultiByteToWideChar / WideCharToMultiByte on guest strings:
 * src_len -1 = up to and including the NUL; dst NULL or dst_cap 0 = the count
 * needed; a dst too small = 0, as the Windows calls fail then. Host paths
 * (folder names from the command line) are UTF-8 and never come through here.
 */
#ifndef XBOXRECOMP_GUEST_TEXT_H
#define XBOXRECOMP_GUEST_TEXT_H

#include <string.h>

static inline int xbox_guest_text_to_wide(const char *src, int src_len,
                                          WCHAR *dst, int dst_cap)
{
    int i;

    if (!src)
        return 0;
    if (src_len < 0)
        src_len = (int)strlen(src) + 1;
    if (!dst || dst_cap <= 0)
        return src_len;
    if (dst_cap < src_len)
        return 0;
    for (i = 0; i < src_len; i++)
        dst[i] = (WCHAR)(unsigned char)src[i];
    return src_len;
}

static inline int xbox_guest_text_from_wide(const WCHAR *src, int src_len,
                                            char *dst, int dst_cap)
{
    int i;

    if (!src)
        return 0;
    if (src_len < 0) {
        src_len = 0;
        while (src[src_len])
            src_len++;
        src_len++;
    }
    if (!dst || dst_cap <= 0)
        return src_len;
    if (dst_cap < src_len)
        return 0;
    for (i = 0; i < src_len; i++)
        dst[i] = src[i] <= 0xFF ? (char)(unsigned char)src[i] : '?';
    return src_len;
}

#endif /* XBOXRECOMP_GUEST_TEXT_H */
