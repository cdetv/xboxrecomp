/**
 * Replacement textures (D3D11 back end; tex_replace.c): PNG files a player
 * puts in a folder, drawn in place of the title's own textures.
 *
 * A texture is known by the hash of its level-0 bytes (and palette), the
 * one nv2a_draw_d3d11.c's texture cache already takes, so it is found
 * wherever the title loads it to. RECOMP_TEX_REPLACE=<folder> holds the
 * PNGs, each named after the hash it replaces: 16 hex digits, then anything
 * ("0123456789abcdef_logo.png"). A PNG may be any size; it is drawn over
 * the same texture coordinates. RECOMP_TEX_HASH_DUMP=<folder> writes every
 * texture the title uploads, once per hash, as such a PNG
 * (<hash>_<w>x<h>_f<format>.png), to find a texture's hash.
 */
#ifndef TEX_REPLACE_H
#define TEX_REPLACE_H

#include <stdint.h>

/* Pack folders, read after RECOMP_TEX_REPLACE's (its PNGs right in it):
 * each pack with all its subfolders, in the order given; the first pack's
 * PNG wins on a hash two have, and any pack wins over the base folder. A
 * PNG in a pack goes by the namer when the title set one (its path inside
 * the pack, UTF-8 with '\' separators, e.g. "Level\Bank\wall.png" -> the
 * hash, 0 for none), else by the hash its name starts with, as in the base
 * folder. Both are set before the first texture: the folders are listed
 * once. tex_replace_set_packs returns 0 (and changes nothing) when that
 * already happened. dirs are UTF-8 paths; n = 0 for none. */
typedef uint64_t (*TexReplaceNamer)(const char *path_in_pack, void *ctx);
void tex_replace_set_namer(TexReplaceNamer namer, void *ctx);
int  tex_replace_set_packs(const char *const *dirs, int n);

/* 1 when there are replacements (RECOMP_TEX_REPLACE or packs) or
 * RECOMP_TEX_HASH_DUMP is set (read once). */
int tex_replace_active(void);

/* The replacement for hash: its pixels (B8G8R8A8, w x h, decoded on first
 * use and kept), or NULL when there is none or it could not be read. */
const uint32_t *tex_replace_find(uint64_t hash, uint32_t *w, uint32_t *h);

/* 1 when RECOMP_TEX_HASH_DUMP is set. */
int tex_replace_dumping(void);

/* 1 when hash was not dumped yet in this run (and is now counted as
 * dumped), so the caller decodes the texture once. */
int tex_replace_dump_new(uint64_t hash);

/* Write px (B8G8R8A8, w x h) as <hash>_<w>x<h>_f<fmt>.png. */
void tex_replace_dump(uint64_t hash, const uint32_t *px, uint32_t w,
                      uint32_t h, uint32_t fmt);

#endif
