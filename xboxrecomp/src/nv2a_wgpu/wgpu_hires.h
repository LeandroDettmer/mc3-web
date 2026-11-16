/*
 * wgpu_hires.h -- the content key of the WebGPU renderer's hi-res texture
 * replacements (wgpu_tex.c "Hi-res replacements"). Header only: the renderer,
 * its host test (wgpu_hires_test.c) and tools/hud_hd_pack.py (key()) compute
 * the same 64-bit value; change one, change all three.
 *
 * What is hashed: level 0 only, whatever the title's mip count.
 *   - DXT (NV2A colour 0x0C DXT1, 0x0E DXT2/3, 0x0F DXT4/5): the blocks as
 *     they sit in guest memory (= the .tex file's: not swizzled), kind =
 *     that colour byte.
 *   - Every other format: the texels as the renderer decodes them, w x h
 *     rows top first, each 0xAARRGGBB little-endian (bytes B, G, R, A),
 *     kind = 0. A palettised image is its palette looked up, so the key
 *     does not depend on how the title stored it (P8 or 4-bit indices,
 *     swizzled or linear, a 16- or 256-entry palette) and a new palette is
 *     a new key.
 * Key: h = 0xCBF29CE484222325 ^ w << 48 ^ h << 32 ^ kind; then for each
 * whole 8-byte little-endian word x: h = (h ^ x) * 0x100000001B3; then for
 * each byte left: h = (h ^ byte) * 0x100000001B3 (all mod 2^64); 0 becomes
 * 1. A pack names a replacement <key as 16 lowercase hex digits>.png
 * (hd_pack.c's naming).
 *
 * Index key (2026-11-16): a palettised image (NV2A I8, 0x0B) also has a key
 * of its indices alone -- w x h u8, rows top first (unswizzled), kind
 * WG_HIRES_INDEXED -- for a palette the title changes at run time (MC3 tints
 * the speedometer's and tachometer's green-dominant entries with the car's
 * tune colour: sub_000DA300 copies the palette, sub_000DC040 tints it), which
 * the colour key never matches. The renderer asks for it when the colour key
 * is not in the pack. Its file is an 8-bit grey PNG holding a palette index
 * per texel (an index map, any size: tools/hud_hd_pack.py upscales the image
 * in its stock colours and snaps each texel to the nearest palette entry the
 * image uses); hd_pack.c hands it over undecoded and the renderer looks it up
 * in the palette the title has bound (wg_hires_recolour), as decode_indexed
 * does, then builds the levels (wg_hires_mip). Edges are as sharp as the
 * index map: no blend between entries (the colour key's file keeps those).
 */
#ifndef WGPU_HIRES_H
#define WGPU_HIRES_H

#include <stdint.h>
#include <string.h>

static inline uint64_t wg_hires_key(const void *data, uint32_t n, uint32_t w, uint32_t h,
                                    uint32_t kind)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t k = 0xCBF29CE484222325ull ^ (uint64_t)w << 48 ^ (uint64_t)h << 32 ^ kind, x;
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        memcpy(&x, p + i, 8);                   /* wasm and x86: little-endian */
        k = (k ^ x) * 0x100000001B3ull;
    }
    for (; i < n; i++)
        k = (k ^ p[i]) * 0x100000001B3ull;
    return k ? k : 1;
}

#define WG_HIRES_INDEXED 0x10Bu

/* n indices through a palette of `entries` 0xAARRGGBB colours; an index past
 * it reads entry 0 (wgpu_tex.c decode_indexed). */
static inline void wg_hires_recolour(const uint8_t *idx, uint32_t n, const uint32_t *pal,
                                     uint32_t entries, uint32_t *out)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        out[i] = idx[i] < entries ? pal[idx[i]] : pal[0];
}

/* The next level of a w x h image of 0xAARRGGBB texels: 2x2 box per byte,
 * at least 1x1 (an odd edge repeats its last texel). */
static inline void wg_hires_mip(const uint32_t *src, uint32_t w, uint32_t h, uint32_t *dst)
{
    uint32_t w2 = w > 1 ? w / 2 : 1, h2 = h > 1 ? h / 2 : 1, x, y, c;
    for (y = 0; y < h2; y++)
        for (x = 0; x < w2; x++) {
            uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            uint32_t y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            uint32_t a = src[y0 * w + x0], b = src[y0 * w + x1], d = src[y1 * w + x0], e = src[y1 * w + x1];
            uint32_t v = 0;
            for (c = 0; c < 32; c += 8)
                v |= ((((a >> c) & 0xFF) + ((b >> c) & 0xFF) + ((d >> c) & 0xFF) + ((e >> c) & 0xFF) + 2) / 4) << c;
            dst[y * w2 + x] = v;
        }
}

#endif
