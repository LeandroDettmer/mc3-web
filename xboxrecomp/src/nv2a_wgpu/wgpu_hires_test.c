/*
 * wgpu_hires_test.c -- host test: tools/hud_hd_pack.py's keys of a stock .tex
 * equal the keys wgpu_tex.c computes from what the title puts in guest memory
 * (wgpu_hires.h), and a pack's index maps recolour as the renderer does.
 * Builds natively, no browser, no game (WSL):
 *
 *   cc -std=c11 -O2 -I xboxrecomp/src -I xboxrecomp/src/nv2a_gl \
 *      xboxrecomp/src/nv2a_wgpu/wgpu_hires_test.c -o hires_test -lm
 *   python3 tools/hud_hd_pack.py keys <dir>/<each>.tex > keys.tsv  (stdlib only)
 *   ./hires_test keys.tsv [<pack dir>]                      exit 0: every check passes
 *
 * Per file (keys.tsv: key, index key or '-', WxH, .tex type, path), the guest
 * image is built the ways the title may store it and decoded as wgpu_tex.c does:
 *   - DXT1/3/5: the blocks as in the file (level 0); the renderer hashes guest
 *     memory as is.
 *   - P8/PA8/P4/PA4, as NV2A SZ_I8_A8R8G8B8 (0x0B): indices swizzled
 *     (xbox_swizzle_rect), palette 256 entries (P4: 16, length code 32); the
 *     renderer's decode_indexed: xbox_unswizzle_rect + wg_hires_recolour.
 *     Colour key from the looked-up texels, index key (WG_HIRES_INDEXED) from
 *     the unswizzled indices.
 *   - the same image turned into swizzled A8R8G8B8 (0x06): xbox_unswizzle_rect
 *     at 4 bytes a texel.
 * With a pack dir, each palettised file's index map (<pack>/textures/<index
 * key>.png) must be an 8-bit grey PNG a whole multiple of the stock size,
 * every index inside the palette, the middle texel of at least 90% of its
 * blocks the stock texel's colour within 16 a channel (xbrz moves edges), and recoloured with
 * the palette tinted as MC3 tints the speedometer (sub_000DC040: an entry with
 * G > R and G > B becomes the tune colour x G / 255, alpha the smaller) each
 * texel the tinted entry, with a full level chain (wg_hires_mip) whose 1x1
 * level is the average of the image.
 */
#include "d3d/d3d8_swizzle.h"
#include "nv2a_wgpu/wgpu_hires.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_pack;
static int s_maps;

static uint8_t *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc((size_t)*n);
    if (d && fread(d, 1, (size_t)*n, f) != (size_t)*n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

/* MC3's tint (sub_000DC040) of a palette with tune colour c (0xAARRGGBB). */
static void tint(const uint32_t *pal, uint32_t n, uint32_t c, uint32_t *out)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t p = pal[i], r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF, a = p >> 24;
        uint32_t ta = c >> 24;
        if (g > r && g > b)
            p = (a < ta ? a : ta) << 24 | (((c >> 16) & 0xFF) * g / 255) << 16
              | (((c >> 8) & 0xFF) * g / 255) << 8 | ((c & 0xFF) * g / 255);
        out[i] = p;
    }
}

/* Two colours within 16 a channel (a snapped gradient texel may take the next
 * entry), or both (nearly) transparent: the colour under alpha 0 is not seen. */
static int near(uint32_t a, uint32_t b)
{
    int c, d;
    if ((a >> 24) < 16 && (b >> 24) < 16)
        return 1;
    for (c = 0; c < 32; c += 8) {
        d = (int)((a >> c) & 0xFF) - (int)((b >> c) & 0xFF);
        if (d < -16 || d > 16)
            return 0;
    }
    return 1;
}

/* The index map of a palettised image: 0 passes. */
static int check_map(unsigned long long ckey, unsigned long long ikey, uint32_t w, uint32_t h, const uint8_t *idx,
                     const uint32_t *pal, uint32_t entries, const char *path)
{
    char png[1200];
    int mw = 0, mh = 0, comp = 0;
    uint8_t *m;
    uint32_t s, x, y, k, blocks = 0, same = 0, tinted[256], *out, *lv;
    size_t n, need, off;

    snprintf(png, sizeof png, "%s/textures/%016llx.png", s_pack, ckey);
    if (!stbi_info(png, &mw, &mh, &comp))
        return 0;                               /* not in the pack (the *_proxy stand-ins) */
    snprintf(png, sizeof png, "%s/textures/%016llx.png", s_pack, ikey);
    if (!stbi_info(png, &mw, &mh, &comp) || comp != 1) {
        printf("FAIL %s: index map %s missing or not grey (%d channels)\n", path, png, comp);
        return 1;
    }
    m = stbi_load(png, &mw, &mh, &comp, 1);
    if (!m || mw % (int)w || mh % (int)h || mw / (int)w != mh / (int)h) {
        printf("FAIL %s: index map %dx%d not a whole multiple of %ux%u\n", path, mw, mh, w, h);
        stbi_image_free(m);
        return 1;
    }
    s = (uint32_t)mw / w;
    n = (size_t)mw * mh;
    for (k = 0; k < n; k++)
        if (m[k] >= entries) {
            printf("FAIL %s: index %u past the palette (%u)\n", path, m[k], entries);
            stbi_image_free(m);
            return 1;
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            blocks++;
            same += near(pal[m[(size_t)(y * s + s / 2) * mw + x * s + s / 2]], pal[idx[y * w + x]]);
        }
    tint(pal, entries, 0xFFFF3020u, tinted);
    for (need = 0, x = (uint32_t)mw, y = (uint32_t)mh;; x = x > 1 ? x / 2 : 1, y = y > 1 ? y / 2 : 1) {
        need += (size_t)x * y;
        if (x == 1 && y == 1)
            break;
    }
    out = (uint32_t *)malloc(need * 4);
    wg_hires_recolour(m, (uint32_t)n, tinted, entries, out);
    for (k = 0; k < n; k++)
        if (out[k] != tinted[m[k]]) {
            printf("FAIL %s: texel %u recoloured %08X, tinted entry %08X\n", path, k, out[k], tinted[m[k]]);
            free(out);
            stbi_image_free(m);
            return 1;
        }
    for (off = 0, x = (uint32_t)mw, y = (uint32_t)mh; x > 1 || y > 1; x = x > 1 ? x / 2 : 1, y = y > 1 ? y / 2 : 1) {
        wg_hires_mip(out + off, x, y, out + off + (size_t)x * y);
        off += (size_t)x * y;
    }
    lv = out + off;                             /* the 1x1 level; the average, within rounding */
    {
        double sum[4] = { 0, 0, 0, 0 };
        int c, bad = 0;
        for (k = 0; k < n; k++)
            for (c = 0; c < 4; c++)
                sum[c] += (out[k] >> (8 * c)) & 0xFF;
        for (c = 0; c < 4; c++) {
            double want = sum[c] / n, got = (*lv >> (8 * c)) & 0xFF;
            if (got < want - 6.0 || got > want + 6.0)   /* rounding, once a level */
                bad = 1;
        }
        if (bad) {
            printf("FAIL %s: the 1x1 level %08X is not the image's average\n", path, *lv);
            free(out);
            stbi_image_free(m);
            return 1;
        }
    }
    free(out);
    stbi_image_free(m);
    if (same * 10 < blocks * 9) {
        printf("FAIL %s: index map %ux: only %u of %u blocks show the stock colour\n", path, s, same, blocks);
        return 1;
    }
    printf("ok   %s: index map %ux%u (%ux), %u of %u blocks as stock, recoloured with a tint\n",
           path, mw, mh, s, same, blocks);
    s_maps++;
    return 0;
}

/* 0: agrees; prints a line either way. */
static int check(unsigned long long want, const char *iwant, const char *path)
{
    long n = 0;
    uint8_t *d = load(path, &n);
    uint32_t w, h, t, k, entries, pal[256];
    uint8_t *idx, *guest8, *idx2;
    uint32_t *argb, *guest32, *out;
    uint64_t ka, kb, ki;
    unsigned long long iw = 0;
    int bad;

    if (!d || n < 14) {
        printf("FAIL %s: cannot read\n", path);
        return 1;
    }
    w = d[0] | d[1] << 8;
    h = d[2] | d[3] << 8;
    t = d[4] | d[5] << 8;
    if (t == 0x16 || t == 0x18 || t == 0x1A) {
        uint32_t colour = t == 0x16 ? 0x0C : t == 0x18 ? 0x0E : 0x0F;
        ka = wg_hires_key(d + 14, ((w + 3) / 4) * ((h + 3) / 4) * d3d8_format_dxt_block_bytes(colour),
                          w, h, colour);
        bad = ka != want || strcmp(iwant, "-") != 0;
        printf("%s %016llx %s (DXT colour 0x%02X)\n", bad ? "FAIL" : "ok  ", (unsigned long long)ka, path, colour);
        free(d);
        return bad;
    }
    memset(pal, 0, sizeof pal);
    idx = (uint8_t *)malloc(w * h);
    guest8 = (uint8_t *)malloc(w * h);
    idx2 = (uint8_t *)malloc(w * h);
    argb = (uint32_t *)malloc(w * h * 4);
    guest32 = (uint32_t *)malloc(w * h * 4);
    out = (uint32_t *)malloc(w * h * 4);
    if (t == 0x01 || t == 0x0E) {
        memcpy(pal, d + 14, 1024);
        memcpy(idx, d + 14 + 1024, w * h);
        entries = 256;
    } else {                                    /* 0x0F, 0x10: 4-bit, low nibble first */
        memcpy(pal, d + 14, 64);
        for (k = 0; k < w * h; k++)
            idx[k] = (d[14 + 64 + k / 2] >> (k & 1 ? 4 : 0)) & 15;
        entries = 32;                           /* the smallest palette length code */
    }
    /* 0x0B: what the renderer's decode_indexed does */
    xbox_swizzle_rect(guest8, idx, w, h, 1);
    xbox_unswizzle_rect(idx2, guest8, w, h, 1);
    wg_hires_recolour(idx2, w * h, pal, entries, out);
    ka = wg_hires_key(out, w * h * 4, w, h, 0);
    ki = wg_hires_key(idx2, w * h, w, h, WG_HIRES_INDEXED);
    /* 0x06: the title converted it to A8R8G8B8 */
    for (k = 0; k < w * h; k++)
        argb[k] = pal[idx[k]];
    xbox_swizzle_rect(guest32, argb, w, h, 4);
    xbox_unswizzle_rect(out, guest32, w, h, 4);
    kb = wg_hires_key(out, w * h * 4, w, h, 0);
    bad = ka != want || kb != want || sscanf(iwant, "%llx", &iw) != 1 || ki != iw;
    printf("%s %016llx %016llx %016llx %s (type 0x%02X as I8 / A8R8G8B8, index key)\n", bad ? "FAIL" : "ok  ",
           (unsigned long long)ka, (unsigned long long)kb, (unsigned long long)ki, path, t);
    if (!bad && s_pack)
        bad = check_map(want, iw, w, h, idx, pal, t == 0x01 || t == 0x0E ? 256 : 16, path);
    free(idx); free(guest8); free(idx2); free(argb); free(guest32); free(out); free(d);
    return bad;
}

int main(int argc, char **argv)
{
    char line[1300], path[1024], size[32], type[16], ik[32];
    unsigned long long want;
    int files = 0, fail = 0;
    FILE *f;

    if (argc < 2 || argc > 3 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s <keys.tsv from tools/hud_hd_pack.py keys> [<pack dir>]\n", argv[0]);
        return 2;
    }
    s_pack = argc == 3 ? argv[2] : NULL;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%llx\t%31s\t%31s\t%15s\t%1023[^\r\n]", &want, ik, size, type, path) != 5)
            continue;
        files++;
        fail += check(want, ik, path);
    }
    fclose(f);
    printf("%d files, %d index maps checked, %d failed\n", files, s_maps, fail);
    return fail || !files;
}
