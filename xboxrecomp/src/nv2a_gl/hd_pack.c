/*
 * hd_pack.c -- replacement textures from a folder beside the game, and the
 * dump that makes them.
 *
 * The title's own files stay untouched (game/); a pack lives in its own
 * folder (RECOMP_GL_HD_DIR, default "hd"; MC3 points it beside game/):
 *
 *   <hd>/textures/<hash>.png   replaces the texture with that content hash,
 *                              at any size
 *   <hd>/dump/<hash>.png       written with RECOMP_GL_TEX_DUMP=1: every
 *                              texture the title uses, once, as decoded
 *
 * The hash (nv2a_gl.c tex_full_hash) covers the texture's level-0 bytes,
 * format, size and, for a palettised image, its palette: one key per
 * distinct image. Delete the folder and the title looks as it always did.
 * Loading and saving PNG is stb_image / stb_image_write (public domain).
 *
 * Loading happens on a worker thread (hd_pack_request / hd_pack_poll): the
 * renderer draws the original until the replacement is ready. On the
 * Switch, decoding, mipmapping and uploading 760 PNGs in the frame that
 * first drew them froze MC3's menu for 6 s, and the uncompressed RGBA cost
 * frame rate (2026-10-21). The worker also builds the mip chain and
 * compresses every level to DXT1 (opaque) or DXT5 (alpha carries data) with
 * stb_dxt: a quarter of the memory and bandwidth of RGBA, the format the
 * originals use.
 */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)          /* Windows: no mode argument */
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "hd_pack.h"

static char      s_dir[512];
static uint64_t *s_have;           /* sorted hashes of <hd>/textures/<hash>.png */
static size_t    s_nhave;
static uint64_t *s_dumped;         /* hashes written this run (unsorted, appended) */
static size_t    s_ndumped, s_capdumped;
static int       s_on = -1, s_dump = -1;

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* "0123456789abcdef.png" -> 1 and the hash. */
static int name_hash(const char *n, uint64_t *h)
{
    char *end;
    size_t len = strlen(n);
    if (len != 20 || strcmp(n + 16, ".png") != 0)
        return 0;
    *h = strtoull(n, &end, 16);
    return end == n + 16;
}

int hd_pack_init(void)
{
    const char *e;
    char path[600];
    DIR *d;
    struct dirent *de;
    size_t cap = 0;

    if (s_on >= 0)
        return s_on;
    e = getenv("RECOMP_GL_HD_DIR");
    snprintf(s_dir, sizeof s_dir, "%s", e && *e ? e : "hd");
    e = getenv("RECOMP_GL_TEX_DUMP");
    s_dump = e && *e == '1';

    snprintf(path, sizeof path, "%s/textures", s_dir);
    if ((d = opendir(path)) != NULL) {
        while ((de = readdir(d)) != NULL) {
            uint64_t h;
            if (!name_hash(de->d_name, &h))
                continue;
            if (s_nhave == cap) {
                size_t nc = cap ? cap * 2 : 256;
                uint64_t *n = realloc(s_have, nc * sizeof *n);
                if (!n) break;
                s_have = n;
                cap = nc;
            }
            s_have[s_nhave++] = h;
        }
        closedir(d);                      /* Atmosphere: never leave one open */
        qsort(s_have, s_nhave, sizeof *s_have, cmp_u64);
    }
    if (s_dump) {
        snprintf(path, sizeof path, "%s", s_dir);
        mkdir(path, 0777);
        snprintf(path, sizeof path, "%s/dump", s_dir);
        mkdir(path, 0777);
    }
    s_on = s_nhave > 0 || s_dump;
    if (s_on)
        fprintf(stderr, "  [GL] HD pack %s: %zu replacement textures%s\n", s_dir, s_nhave,
                s_dump ? ", dumping every texture to dump/ (RECOMP_GL_TEX_DUMP)" : "");
    return s_on;
}

int hd_pack_has(uint64_t h)
{
    return s_nhave && bsearch(&h, s_have, s_nhave, sizeof *s_have, cmp_u64) != NULL;
}

unsigned char *hd_pack_load(uint64_t h, int *w, int *hgt)
{
    char path[600];
    int comp = 0;
    unsigned char *px;
    snprintf(path, sizeof path, "%s/textures/%016llx.png", s_dir, (unsigned long long)h);
    px = stbi_load(path, w, hgt, &comp, 4);
    if (!px)
        fprintf(stderr, "  [GL] HD pack: cannot read %s (%s)\n", path, stbi_failure_reason());
    return px;
}

void hd_pack_free(unsigned char *px)
{
    stbi_image_free(px);
}

int hd_pack_dumping(void)
{
    return s_dump > 0;
}

/* argb: the decoded level 0 as A8R8G8B8 dwords (the renderer's layout). */
void hd_pack_dump(uint64_t h, const uint32_t *argb, int w, int hgt)
{
    char path[600];
    unsigned char *rgba;
    size_t i, n = (size_t)w * hgt;
    for (i = 0; i < s_ndumped; i++)
        if (s_dumped[i] == h)
            return;
    if (s_ndumped == s_capdumped) {
        size_t nc = s_capdumped ? s_capdumped * 2 : 256;
        uint64_t *nd = realloc(s_dumped, nc * sizeof *nd);
        if (!nd) return;
        s_dumped = nd;
        s_capdumped = nc;
    }
    s_dumped[s_ndumped++] = h;
    snprintf(path, sizeof path, "%s/dump/%016llx.png", s_dir, (unsigned long long)h);
    {
        struct stat st;
        if (stat(path, &st) == 0)             /* an earlier run wrote it */
            return;
    }
    if (!(rgba = malloc(n * 4)))
        return;
    for (i = 0; i < n; i++) {
        uint32_t p = argb[i];
        rgba[i * 4 + 0] = (unsigned char)(p >> 16);
        rgba[i * 4 + 1] = (unsigned char)(p >> 8);
        rgba[i * 4 + 2] = (unsigned char)p;
        rgba[i * 4 + 3] = (unsigned char)(p >> 24);
    }
    if (!stbi_write_png(path, w, hgt, 4, rgba, w * 4))
        fprintf(stderr, "  [GL] HD pack: cannot write %s\n", path);
    free(rgba);
}

/* ── Background loading ───────────────────────────────────────────── */

static pthread_mutex_t s_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_q_cv = PTHREAD_COND_INITIALIZER;
static uint64_t       *s_req;                 /* waiting requests (FIFO) */
static size_t          s_nreq, s_capreq;
static HdTex          *s_done;                /* finished, for hd_pack_poll */
static int             s_worker = -1;

/* A w x h RGBA image's next mip level (box filter, at least 1x1). */
static unsigned char *mip_down(const unsigned char *src, int w, int h, int *nw, int *nh)
{
    int x, y, c, w2 = w > 1 ? w / 2 : 1, h2 = h > 1 ? h / 2 : 1;
    unsigned char *d = malloc((size_t)w2 * h2 * 4);
    if (!d)
        return NULL;
    for (y = 0; y < h2; y++)
        for (x = 0; x < w2; x++)
            for (c = 0; c < 4; c++) {
                int x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
                int y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
                int sum = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c]
                        + src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
                d[((size_t)y * w2 + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
            }
    *nw = w2;
    *nh = h2;
    return d;
}

/* One level, DXT-compressed: 4x4 blocks, edge texels repeated. */
static unsigned char *dxt_level(const unsigned char *px, int w, int h, int alpha, uint32_t *bytes)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, x, y;
    size_t bsz = alpha ? 16 : 8;
    unsigned char *out = malloc((size_t)bw * bh * bsz), blk[64];
    if (!out)
        return NULL;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    int sx = bx * 4 + x < w ? bx * 4 + x : w - 1;
                    int sy = by * 4 + y < h ? by * 4 + y : h - 1;
                    memcpy(blk + (y * 4 + x) * 4, px + ((size_t)sy * w + sx) * 4, 4);
                }
            stb_compress_dxt_block(out + ((size_t)by * bw + bx) * bsz, blk, alpha, STB_DXT_NORMAL);
        }
    *bytes = (uint32_t)((size_t)bw * bh * bsz);
    return out;
}

static HdTex *load_compressed(uint64_t key)
{
    int w = 0, h = 0, i, n, opaque = 1;
    unsigned char *px = hd_pack_load(key, &w, &h), *cur;
    HdTex *t;
    if (!px)
        return NULL;
    for (i = 0, n = w * h; i < n && opaque; i++)
        if (px[i * 4 + 3] != 255)
            opaque = 0;
    if (!(t = calloc(1, sizeof *t))) {
        hd_pack_free(px);
        return NULL;
    }
    t->key = key;
    t->w = w;
    t->h = h;
    t->alpha = !opaque;
    cur = px;
    for (t->levels = 0; t->levels < HD_MAX_LEVELS; ) {
        int nw, nh;
        unsigned char *next;
        t->data[t->levels] = dxt_level(cur, w, h, t->alpha, &t->bytes[t->levels]);
        t->lw[t->levels] = w;
        t->lh[t->levels] = h;
        if (!t->data[t->levels])
            break;
        t->levels++;
        if (w == 1 && h == 1)
            break;
        next = mip_down(cur, w, h, &nw, &nh);
        if (cur != px)
            free(cur);
        else
            hd_pack_free(px);
        cur = next;
        if (!cur)
            break;
        w = nw;
        h = nh;
    }
    if (cur && cur != px)
        free(cur);
    else if (cur == px)
        hd_pack_free(px);
    if (!t->levels) {
        free(t);
        return NULL;
    }
    return t;
}

static void *worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t key;
        HdTex *t;
        pthread_mutex_lock(&s_q_lock);
        while (!s_nreq)
            pthread_cond_wait(&s_q_cv, &s_q_lock);
        key = s_req[0];
        memmove(s_req, s_req + 1, (s_nreq - 1) * sizeof *s_req);
        s_nreq--;
        pthread_mutex_unlock(&s_q_lock);
        if ((t = load_compressed(key)) != NULL) {
            pthread_mutex_lock(&s_q_lock);
            t->next = s_done;
            s_done = t;
            pthread_mutex_unlock(&s_q_lock);
        }
    }
    return NULL;
}

void hd_pack_request(uint64_t key)
{
    size_t i;
    pthread_mutex_lock(&s_q_lock);
    if (s_worker < 0) {
        pthread_t th;
        s_worker = pthread_create(&th, NULL, worker_main, NULL) == 0;
        if (s_worker)
            pthread_detach(th);
    }
    for (i = 0; i < s_nreq; i++)
        if (s_req[i] == key) {
            pthread_mutex_unlock(&s_q_lock);
            return;
        }
    if (s_nreq == s_capreq) {
        size_t nc = s_capreq ? s_capreq * 2 : 256;
        uint64_t *n = realloc(s_req, nc * sizeof *n);
        if (!n) { pthread_mutex_unlock(&s_q_lock); return; }
        s_req = n;
        s_capreq = nc;
    }
    s_req[s_nreq++] = key;
    pthread_cond_signal(&s_q_cv);
    pthread_mutex_unlock(&s_q_lock);
}

HdTex *hd_pack_poll(void)
{
    HdTex *t;
    pthread_mutex_lock(&s_q_lock);
    t = s_done;
    if (t)
        s_done = t->next;
    pthread_mutex_unlock(&s_q_lock);
    return t;
}

void hd_pack_tex_free(HdTex *t)
{
    int i;
    if (!t)
        return;
    for (i = 0; i < t->levels; i++)
        free(t->data[i]);
    free(t);
}
