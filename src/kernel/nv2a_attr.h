/**
 * One NV097 vertex attribute element as floats: the formats the CPU fetch
 * (pb_fetch_attr) reads and the GPU fetch (nv2a_vsh_hlsl.c, vsh_fetch) must
 * match. Portable C, no state; tests include it as the reference.
 */
#ifndef XBOXRECOMP_NV2A_ATTR_H
#define XBOXRECOMP_NV2A_ATTR_H

#include <stdint.h>

/* Bytes one element of `type` with `size` components reads, 0 for a type
 * nothing decodes (the caller then leaves the attribute absent). */
static inline uint32_t nv2a_attr_bytes(uint32_t type, uint32_t size)
{
    uint32_t n = size < 4 ? size : 4;
    switch (type) {
    case 0: case 6: return 4;            /* D3DCOLOR, packed 11:11:10 */
    case 2: return 4 * n;                /* float */
    case 4: return n;                    /* unsigned byte, normalised */
    case 1: case 5: return 2 * n;        /* signed short */
    default: return 0;
    }
}

/* Element at p into out, components the format does not carry left as the
 * caller set them (0,0,0,1). 0 for a type nothing decodes. */
static inline int nv2a_attr_decode(const uint8_t *p, uint32_t type,
                                   uint32_t size, float out[4])
{
    uint32_t i;

    switch (type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    /* The formats 3D geometry uses and screen-space quads never did. */
    case 1:                                  /* signed short, normalised */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i] / 32767.0f;
        return 1;
    case 5:                                  /* signed short, as is */
        for (i = 0; i < size && i < 4; i++)
            out[i] = (float)((const int16_t *)p)[i];
        return 1;
    case 6: {                                /* packed 11:11:10 normal */
        uint32_t v = *(const uint32_t *)p;
        out[0] = (float)((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)v >> 22) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

#endif
