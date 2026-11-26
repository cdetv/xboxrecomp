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
 * folder. The namer is set before the first list that needs it.
 * tex_replace_set_packs may be called at any time from any thread: before
 * the first texture its list is the first one listed; after it, the back
 * end lists the folders again between two frames (tex_replace_reload) and
 * draws every texture again, so packs switch while the title runs. dirs are
 * UTF-8 paths; n = 0 for none. 0 when out of memory (nothing changes). */
typedef uint64_t (*TexReplaceNamer)(const char *path_in_pack, void *ctx);
void tex_replace_set_namer(TexReplaceNamer namer, void *ctx);
int  tex_replace_set_packs(const char *const *dirs, int n);

/* 1 when there are replacements (RECOMP_TEX_REPLACE or packs) or
 * RECOMP_TEX_HASH_DUMP is set (read once). */
int tex_replace_active(void);

/* For the back end, between two frames on its own thread: when
 * tex_replace_set_packs gave a new list since the folders were listed, they
 * are listed again (every decoded picture of the old list freed) and this
 * returns 1: every texture made from the old list must go, as must every one
 * a new pack may now replace. 0 otherwise (cheap: a flag). */
int tex_replace_reload(void);

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
