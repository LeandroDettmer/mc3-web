/*
 * wgpu_hires_test.c -- host test: tools/hud_hd_pack.py's key of a stock .tex
 * equals the key wgpu_tex.c computes from what the title puts in guest memory
 * (wgpu_hires.h). Builds natively, no browser, no game (WSL):
 *
 *   cc -std=c11 -O2 -I xboxrecomp/src xboxrecomp/src/nv2a_wgpu/wgpu_hires_test.c -o hires_test
 *   python3 tools/hud_hd_pack.py keys <dir>/<each>.tex > keys.tsv  (stdlib only)
 *   ./hires_test keys.tsv                                        exit 0: every key agrees
 *
 * Per file (keys.tsv: key, WxH, .tex type, path), the guest image is built the
 * ways the title may store it and decoded as wgpu_tex.c does:
 *   - DXT1/3/5: the blocks as in the file (level 0); the renderer hashes guest
 *     memory as is.
 *   - P8/PA8/P4/PA4, as NV2A SZ_I8_A8R8G8B8 (0x0B): indices swizzled
 *     (xbox_swizzle_rect), palette 256 entries (P4: 16, length code 32); the
 *     renderer's decode_indexed: xbox_unswizzle_rect + lookup, an index past
 *     the palette reads entry 0 (its lines copied here).
 *   - the same image turned into swizzled A8R8G8B8 (0x06): xbox_unswizzle_rect
 *     at 4 bytes a texel.
 */
#include "d3d/d3d8_swizzle.h"
#include "nv2a_wgpu/wgpu_hires.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* 0: agrees; prints a line either way. */
static int check(unsigned long long want, const char *path)
{
    long n = 0;
    uint8_t *d = load(path, &n);
    uint32_t w, h, t, k, entries, pal[256];
    uint8_t *idx, *guest8, *idx2;
    uint32_t *argb, *guest32, *out;
    uint64_t ka, kb;
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
        bad = ka != want;
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
    for (k = 0; k < w * h; k++)
        out[k] = idx2[k] < entries ? pal[idx2[k]] : pal[0];
    ka = wg_hires_key(out, w * h * 4, w, h, 0);
    /* 0x06: the title converted it to A8R8G8B8 */
    for (k = 0; k < w * h; k++)
        argb[k] = pal[idx[k]];
    xbox_swizzle_rect(guest32, argb, w, h, 4);
    xbox_unswizzle_rect(out, guest32, w, h, 4);
    kb = wg_hires_key(out, w * h * 4, w, h, 0);
    bad = ka != want || kb != want;
    printf("%s %016llx %016llx %s (type 0x%02X as I8 / A8R8G8B8)\n", bad ? "FAIL" : "ok  ",
           (unsigned long long)ka, (unsigned long long)kb, path, t);
    free(idx); free(guest8); free(idx2); free(argb); free(guest32); free(out); free(d);
    return bad;
}

int main(int argc, char **argv)
{
    char line[1200], path[1024], size[32], type[16];
    unsigned long long want;
    int files = 0, fail = 0;
    FILE *f;

    if (argc != 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s <keys.tsv from tools/hud_hd_pack.py keys>\n", argv[0]);
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%llx\t%31s\t%15s\t%1023[^\r\n]", &want, size, type, path) != 4)
            continue;
        files++;
        fail += check(want, path);
    }
    fclose(f);
    printf("%d files, %d failed\n", files, fail);
    return fail || !files;
}
