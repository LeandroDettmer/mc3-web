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

#endif
