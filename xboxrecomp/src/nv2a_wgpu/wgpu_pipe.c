/*
 * wgpu_pipe.c -- shader and pipeline registries of the WebGPU back end
 * (render pthread; ADR 0011, docs/webgpu-plan.md, wgpu_int.h).
 *
 * Every vertex module, fragment module and pipeline gets a C-side id the
 * first time it is needed: the generator (wgsl_vsh.c, wgsl_psh.c) writes the
 * WGSL once, WG_OP_SHADER hands it to web/wgpu.js, and WG_OP_PIPE_CREATE
 * names a pipeline by (vs, fs, WgPipeKey). Later draws only look the id up.
 *
 * M3: the key list across runs. Every module key (the vertex program words,
 * or none for the fixed module; the combiner key + texture dims) and every
 * pipeline (the two modules' places in the list + WgPipeKey) made is
 * appended to /opfs/cache/wgpu-pipes.bin. When the title says a static
 * screen is up (nv2a_wgpu_prewarm_start: MC3's legal screen, as nv2a_gl's
 * program list), the list is replayed through the generators a few
 * pipelines a frame; web/wgpu.js makes a PIPE_CREATE that no SET_PIPE of
 * its frame uses with createRenderPipelineAsync, so the compiles happen
 * off the frames that later draw with them. A file with another header
 * (WG_VERSION, WP_EMIT_VERSION, key sizes) is started anew; a bad record
 * ends the list there (the file is rewritten to its good part).
 * RECOMP_WGPU_PREWARM=0: no prewarm (the list is still kept), =<n>
 * pipelines a frame (default 16). RECOMP_WGPU_PIPE_CACHE=0: no file.
 *
 * The seed: a deploy may ship a list made in the owner's browser beside its
 * module (<app>-pipes-seed.bin); the start page copies it to
 * /opfs/cache/wgpu-pipes-seed.bin before the game starts (web/index.html,
 * which also saves a browser's list as a seed: mc3ExportPipes()). At load
 * the seed's records missing from the player's list are appended to it
 * (indices remapped), so a first visit prewarms what the owner met. A seed
 * with another header is ignored.
 */
#include "wgpu_int.h"
#include "nv2a_wgpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ── A small hash table: key bytes -> id ──────────────────────────── */

typedef struct {
    uint64_t h;
    uint8_t *key;               /* NULL: free slot */
    uint32_t len, id;
} WpEnt;

typedef struct {
    WpEnt   *e;
    uint32_t cap, n;            /* cap: a power of two */
} WpTab;

static uint64_t wp_hash(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    while (n--)
        h = (h ^ *b++) * 1099511628211ull;
    return h;
}

/* The entry for key, or NULL. */
static WpEnt *wp_find(const WpTab *t, uint64_t h, const void *key, uint32_t len)
{
    uint32_t i;
    if (!t->cap)
        return NULL;
    for (i = (uint32_t)h & (t->cap - 1); t->e[i].key; i = (i + 1) & (t->cap - 1))
        if (t->e[i].h == h && t->e[i].len == len && !memcmp(t->e[i].key, key, len))
            return &t->e[i];
    return NULL;
}

static void wp_put(WpTab *t, uint64_t h, const void *key, uint32_t len, uint32_t id)
{
    uint32_t i;
    if ((t->n + 1) * 2 > t->cap) {              /* keep it at most half full */
        WpTab g;
        g.cap = t->cap ? t->cap * 2 : 256;
        g.n = 0;
        g.e = (WpEnt *)calloc(g.cap, sizeof *g.e);
        if (!g.e) {
            fprintf(stderr, "  [WGPU] registry of %u entries: out of memory\n", g.cap);
            abort();
        }
        for (i = 0; i < t->cap; i++)
            if (t->e[i].key) {
                uint32_t j = (uint32_t)t->e[i].h & (g.cap - 1);
                while (g.e[j].key)
                    j = (j + 1) & (g.cap - 1);
                g.e[j] = t->e[i];
                g.n++;
            }
        free(t->e);
        *t = g;
    }
    for (i = (uint32_t)h & (t->cap - 1); t->e[i].key; i = (i + 1) & (t->cap - 1)) { }
    t->e[i].key = (uint8_t *)malloc(len ? len : 1);     /* the fixed module's key is empty */
    if (!t->e[i].key) {
        fprintf(stderr, "  [WGPU] registry key: out of memory\n");
        abort();
    }
    memcpy(t->e[i].key, key, len);
    t->e[i].h = h;
    t->e[i].len = len;
    t->e[i].id = id;
    t->n++;
}

/* ── The key list file ────────────────────────────────────────────── */

/* Bump when wgsl_vsh.c / wgsl_psh.c change what a key means (the list holds
 * keys, not WGSL: a new emitter rebuilds the same keys by itself). */
#define WP_EMIT_VERSION 1
#define WP_MAGIC        0x50504757u     /* "WGPP" */
#define WP_PATH         "/opfs/cache/wgpu-pipes.bin"
#define WP_SEED_PATH    "/opfs/cache/wgpu-pipes-seed.bin"
#define WP_MAX_BYTES    (32u << 20)     /* larger: something is wrong, start anew */
#define WP_SAVE_FRAMES  30              /* new records are written at most this often */

/* Header: WP_MAGIC, WG_VERSION, WP_EMIT_VERSION, sizeof(Nv2aPshKey),
 * sizeof(WgPipeKey). Then records: a u32 (type | payload bytes << 8), the
 * payload, zero-padded to 4 bytes:
 *   'V' vertex key: empty (fixed module) or start, slots, program words
 *   'F' fragment key: WpFsKey
 *   'P' WpFilePipe: indices of an earlier V and F (in file order) + key */
typedef struct { Nv2aPshKey k; uint8_t dim[4]; } WpFsKey;
typedef struct { uint32_t vi, fi; WgPipeKey key; } WpFilePipe;

static int       s_cache;               /* 0 not loaded yet, 1 on, -1 off */
static WpTab     s_fv_tab, s_ff_tab, s_fp_tab;  /* key -> its index in the file */
static uint32_t  s_nv, s_nf, s_np;      /* records in the file (+ pending) */
static uint32_t *s_fidx;                /* C shader id -> file index + 1 (0: not listed) */
static uint32_t  s_fidx_cap;
static uint8_t  *s_out;                 /* records not written yet */
static uint32_t  s_out_len, s_out_cap, s_saved_frame;

/* The prewarm: the file as loaded, walked a few pipelines a frame. */
static uint8_t  *s_pw;
static uint32_t  s_pw_len, s_pw_pos, s_pw_vn, s_pw_fn, s_pw_per_frame = 16;
static uint32_t *s_pw_vid, *s_pw_fid;   /* file index -> C shader id (0: failed) */
static uint32_t  s_pw_pipes, s_pw_frames;
static double    s_pw_ms;
static int       s_pw_go;               /* set by the title's thread */

static double wp_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void wp_hdr(uint32_t h[5])
{
    h[0] = WP_MAGIC;
    h[1] = WG_VERSION;
    h[2] = WP_EMIT_VERSION;
    h[3] = sizeof(Nv2aPshKey);
    h[4] = sizeof(WgPipeKey);
}

/* Rewrite the file as hdr + body (body may be empty). */
static int wp_rewrite(const uint8_t *body, uint32_t len)
{
    uint32_t h[5];
    FILE *f = fopen(WP_PATH, "wb");
    if (!f)
        return 0;
    wp_hdr(h);
    fwrite(h, sizeof h, 1, f);
    if (len)
        fwrite(body, len, 1, f);
    fclose(f);
    return 1;
}

/* Walk records from body[0..len): call back per good record; returns the
 * bytes of good records (a bad one ends the walk). */
typedef void (*WpRecFn)(uint32_t type, const uint8_t *p, uint32_t n);
static uint32_t wp_walk(const uint8_t *body, uint32_t len, WpRecFn fn)
{
    uint32_t pos = 0, nv = 0, nf = 0;
    while (pos + 4 <= len) {
        uint32_t w, type, n, step;
        const uint8_t *p = body + pos + 4;
        memcpy(&w, body + pos, 4);
        type = w & 0xFF;
        n = w >> 8;
        step = 4 + ((n + 3) & ~3u);
        if (step > len - pos)
            break;
        if (type == 'V') {
            uint32_t k[2];
            if (n) {
                if (n < 8 + 16 || (n - 8) % 16 || (n - 8) / 16 > 136)
                    break;
                memcpy(k, p, 8);
                if (k[1] > 136 || k[0] + (n - 8) / 16 > k[1])
                    break;
            }
            nv++;
        } else if (type == 'F') {
            if (n != sizeof(WpFsKey))
                break;
            nf++;
        } else if (type == 'P') {
            WpFilePipe fp;
            if (n != sizeof fp)
                break;
            memcpy(&fp, p, sizeof fp);
            if (fp.vi >= nv || fp.fi >= nf)
                break;
        } else {
            break;
        }
        if (fn)
            fn(type, p, n);
        pos += step;
    }
    return pos;
}

static void wp_load_rec(uint32_t type, const uint8_t *p, uint32_t n)
{
    if (type == 'V') {
        wp_put(&s_fv_tab, wp_hash(p, n), p, n, s_nv++);
    } else if (type == 'F') {
        wp_put(&s_ff_tab, wp_hash(p, n), p, n, s_nf++);
    } else {
        wp_put(&s_fp_tab, wp_hash(p, n), p, n, s_np++);
    }
}

/* A list file's body (after a header like ours), or NULL: missing, too
 * large, another header (logged) or unreadable. */
static uint8_t *wp_read(const char *path, const char *what, uint32_t *len)
{
    uint32_t h[5], want[5];
    uint8_t *buf = NULL;
    long size = 0;
    FILE *f = fopen(path, "rb");

    *len = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    rewind(f);
    wp_hdr(want);
    if (size >= (long)sizeof h && size <= (long)WP_MAX_BYTES
        && fread(h, sizeof h, 1, f) == 1 && !memcmp(h, want, sizeof h)) {
        size -= (long)sizeof h;
        buf = (uint8_t *)malloc(size ? (size_t)size : 1);
        if (buf && size && fread(buf, (size_t)size, 1, f) != 1) {
            free(buf);
            buf = NULL;
        }
        *len = (uint32_t)size;
    } else if (size) {
        fprintf(stderr, "  [WGPU] pipeline %s %s: another version or damaged (%ld bytes): %s\n",
                what, path, size, strcmp(path, WP_PATH) ? "ignored" : "started anew");
    }
    fclose(f);
    return buf;
}

static int wp_out(uint32_t type, const void *p, uint32_t n)
{
    uint32_t w = type | n << 8, need = s_out_len + 4 + ((n + 3) & ~3u);
    if (need > s_out_cap) {
        uint32_t cap = s_out_cap ? s_out_cap : 4096;
        uint8_t *b;
        while (cap < need)
            cap *= 2;
        b = (uint8_t *)realloc(s_out, cap);
        if (!b) {
            s_cache = -1;               /* stop listing: the indices would drift */
            return 0;
        }
        s_out = b;
        s_out_cap = cap;
    }
    memcpy(s_out + s_out_len, &w, 4);
    memcpy(s_out + s_out_len + 4, p, n);
    memset(s_out + s_out_len + 4 + n, 0, ((n + 3) & ~3u) - n);
    s_out_len = need;
    return 1;
}

/* A module key's place in the list, added (and queued for the file) if
 * new. 0: listing stopped (out of memory). */
static int wp_add_shader(uint32_t type, const void *key, uint32_t n, uint32_t *idx)
{
    WpTab *t = type == 'V' ? &s_fv_tab : &s_ff_tab;
    uint32_t *count = type == 'V' ? &s_nv : &s_nf;
    uint64_t h = wp_hash(key, n);
    WpEnt *e = wp_find(t, h, key, n);

    if (e) {
        *idx = e->id;
        return 1;
    }
    if (!wp_out(type, key, n))
        return 0;
    *idx = (*count)++;
    wp_put(t, h, key, n, *idx);
    return 1;
}

/* A pipeline record, added if new: 1 if added. */
static int wp_add_pipe(const WpFilePipe *fp)
{
    uint64_t h = wp_hash(fp, sizeof *fp);
    if (wp_find(&s_fp_tab, h, fp, sizeof *fp) || !wp_out('P', fp, sizeof *fp))
        return 0;
    wp_put(&s_fp_tab, h, fp, sizeof *fp, s_np++);
    return 1;
}

/* The seed's merge: its module indices -> the list's. */
static uint32_t *s_sd_v, *s_sd_f, s_sd_nv, s_sd_nf, s_sd_new;

static void wp_seed_rec(uint32_t type, const uint8_t *p, uint32_t n)
{
    WpFilePipe fp;
    if (s_cache <= 0)
        return;
    if (type == 'V') {
        wp_add_shader(type, p, n, &s_sd_v[s_sd_nv++]);
    } else if (type == 'F') {
        wp_add_shader(type, p, n, &s_sd_f[s_sd_nf++]);
    } else {
        memcpy(&fp, p, sizeof fp);
        fp.vi = s_sd_v[fp.vi];          /* wp_walk: vi, fi name earlier records */
        fp.fi = s_sd_f[fp.fi];
        s_sd_new += (uint32_t)wp_add_pipe(&fp);
    }
}

static void wp_seed(void)
{
    uint32_t len, good;
    uint8_t *seed = wp_read(WP_SEED_PATH, "seed", &len);

    if (!seed)
        return;
    /* a record takes at least 4 bytes: room for every index */
    s_sd_v = (uint32_t *)calloc(len / 4 + 1, sizeof *s_sd_v);
    s_sd_f = (uint32_t *)calloc(len / 4 + 1, sizeof *s_sd_f);
    s_sd_nv = s_sd_nf = s_sd_new = 0;
    if (s_sd_v && s_sd_f) {
        good = wp_walk(seed, len, wp_seed_rec);
        fprintf(stderr, "  [WGPU] pipeline seed " WP_SEED_PATH ": %u pipelines new to the list%s\n",
                s_sd_new, good != len ? " (the seed ends in a bad record)" : "");
    }
    free(s_sd_v); free(s_sd_f); free(seed);
    s_sd_v = s_sd_f = NULL;
}

/* Once, on the render thread before the first module is listed. */
static void wp_load(void)
{
    const char *e = getenv("RECOMP_WGPU_PIPE_CACHE"), *pw = getenv("RECOMP_WGPU_PREWARM");
    uint32_t size, good = 0;
    uint8_t *buf;

    if (s_cache)
        return;
    s_cache = -1;
    if (e && *e == '0')
        return;
    if (pw && atoi(pw) > 0)
        s_pw_per_frame = (uint32_t)atoi(pw);
    mkdir("/opfs/cache", 0777);
    buf = wp_read(WP_PATH, "list", &size);
    if (buf) {
        good = wp_walk(buf, size, wp_load_rec);
        if (good != size) {
            fprintf(stderr, "  [WGPU] pipeline list " WP_PATH ": bad record at +%u of %u bytes:"
                    " the rest dropped\n", good, size);
            if (!wp_rewrite(buf, good)) {
                free(buf);
                return;
            }
        }
    } else if (!wp_rewrite(NULL, 0)) {
        fprintf(stderr, "  [WGPU] pipeline list " WP_PATH ": cannot write it (no OPFS?):"
                " pipelines are not kept across runs\n");
        return;
    }
    s_cache = 1;
    s_saved_frame = wg_frame;
    wp_seed();
    /* The prewarm walks the list as the file will hold it: the part loaded,
     * then the seed's new records (queued in s_out for wp_save). */
    if (s_out_len) {
        uint8_t *b = (uint8_t *)realloc(buf, good + s_out_len);
        if (b) {
            memcpy(b + good, s_out, s_out_len);
            buf = b;
            good += s_out_len;
        }
    }
    if (s_np && buf && !(pw && *pw == '0')) {
        s_pw = buf;
        s_pw_len = good;
        s_pw_vid = (uint32_t *)calloc(s_nv, sizeof *s_pw_vid);
        s_pw_fid = (uint32_t *)calloc(s_nf, sizeof *s_pw_fid);
        if (!s_pw_vid || !s_pw_fid) {
            free(s_pw_vid); free(s_pw_fid); free(s_pw);
            s_pw = NULL; s_pw_vid = s_pw_fid = NULL;
        }
        fprintf(stderr, "  [WGPU] pipeline list " WP_PATH ": %u pipelines (%u vertex, %u fragment"
                " modules)%s\n", s_np, s_nv, s_nf, s_pw ? ", made once the title starts it"
                " (nv2a_wgpu_prewarm_start)" : ", no prewarm (RECOMP_WGPU_PREWARM=0)");
    } else {
        free(buf);
    }
}

static void wp_save(void)
{
    FILE *f;
    s_saved_frame = wg_frame;
    if (!s_out_len)
        return;                         /* nothing new: the file is not opened */
    f = fopen(WP_PATH, "ab");
    if (!f)
        return;                         /* kept, tried again: the indices stay in step */
    fwrite(s_out, s_out_len, 1, f);
    fclose(f);
    s_out_len = 0;
}

/* Module `id` was made from key: its place in the list (added if new). */
static void wp_list_shader(uint32_t type, uint32_t id, const void *key, uint32_t n)
{
    uint32_t idx;

    wp_load();
    if (s_cache <= 0 || !id || !wp_add_shader(type, key, n, &idx))
        return;
    if (id >= s_fidx_cap) {
        uint32_t cap = s_fidx_cap ? s_fidx_cap : 1024, *a;
        while (cap <= id)
            cap *= 2;
        a = (uint32_t *)realloc(s_fidx, cap * sizeof *a);
        if (!a)
            return;
        memset(a + s_fidx_cap, 0, (cap - s_fidx_cap) * sizeof *a);
        s_fidx = a;
        s_fidx_cap = cap;
    }
    s_fidx[id] = idx + 1;
}

static void wp_list_pipe(uint32_t vs, uint32_t fs, const WgPipeKey *key)
{
    WpFilePipe fp;

    if (s_cache <= 0 || vs >= s_fidx_cap || fs >= s_fidx_cap || !s_fidx[vs] || !s_fidx[fs])
        return;
    memset(&fp, 0, sizeof fp);
    fp.vi = s_fidx[vs] - 1;
    fp.fi = s_fidx[fs] - 1;
    fp.key = *key;
    wp_add_pipe(&fp);
}

/* ── Shader modules ───────────────────────────────────────────────── */

static WpTab    s_vs_tab, s_fs_tab;
static uint32_t s_shader_next = 1, s_vs_fixed;
static int      s_vs_fixed_done;
static char    *s_text;                         /* the generators' scratch */
#define WP_TEXT_CAP (512u << 10)

/* Hand a generated module to JS under a new id. The text stays allocated
 * for good: the SHADER record points at it until the list is submitted.
 * shortcut: modules are never freed (M2 makes a few hundred at most), free
 * them after the frame's submit if that grows. */
static uint32_t wp_shader_emit(int n, uint32_t stages)
{
    WgShader *s;
    char *keep;
    if (n < 0)
        return 0;
    keep = (char *)malloc((size_t)n + 1);
    if (!keep) {
        fprintf(stderr, "  [WGPU] shader text of %d bytes: out of memory\n", n);
        return 0;
    }
    memcpy(keep, s_text, (size_t)n + 1);
    s = (WgShader *)wg_emit(WG_OP_SHADER, sizeof *s);
    s->id = s_shader_next++;
    s->src_ptr = (uint32_t)(uintptr_t)keep;
    s->src_bytes = (uint32_t)n;
    s->stages = stages;
    return s->id;
}

static int wp_scratch(void)
{
    if (!s_text)
        s_text = (char *)malloc(WP_TEXT_CAP);
    return s_text != NULL;
}

/* Pre-transformed and fixed-function share one module: D[rec].xform picks
 * the path. */
static uint32_t wp_vs_fixed(void)
{
    static int told;
    if (!s_vs_fixed_done && wp_scratch()) {
        s_vs_fixed_done = 1;
        s_vs_fixed = wp_shader_emit(wgsl_vsh_fixed(s_text, WP_TEXT_CAP), WG_SH_VERTEX);
        if (!s_vs_fixed && !told++)
            fprintf(stderr, "  [WGPU] the fixed vertex module did not generate: draws dropped\n");
        wp_list_shader('V', s_vs_fixed, "", 0);
    }
    return s_vs_fixed;
}

/* key: start slot, slots, the program words (n bytes); prog: the slots. */
static uint32_t wp_vs_key(const uint32_t *key, uint32_t n, const uint32_t (*prog)[4])
{
    static int told;
    uint64_t h = wp_hash(key, n);
    WpEnt *e = wp_find(&s_vs_tab, h, key, n);
    uint32_t id;
    int len;

    if (e)
        return e->id;
    /* Made once per program, failures included (id 0, not retried). */
    len = wp_scratch() ? wgsl_vsh_program(prog, key[1], key[0], s_text, WP_TEXT_CAP) : -1;
    id = wp_shader_emit(len, WG_SH_VERTEX);
    if (!id && !told++)
        fprintf(stderr, "  [WGPU] vertex program at slot %u did not generate: its draws are"
                " dropped (logged once)\n", key[0]);
    wp_put(&s_vs_tab, h, key, n, id);
    wp_list_shader('V', id, key, n);
    return id;
}

uint32_t wpipe_vs(const Nv2aRawBatch *b)
{
    static uint32_t last_gen, last_start, last_id;
    static int last_valid;
    uint32_t key[4 * 136 + 2], end;

    if (b->xform != 2)
        return wp_vs_fixed();
    if (!b->vp_program || b->vp_start >= b->vp_slots)
        return 0;
    if (last_valid && b->vp_prog_gen == last_gen && b->vp_start == last_start)
        return last_id;
    /* The key: the start slot and the program up to its FINAL instruction
     * (word 3 bit 0), so programs elsewhere in the slots do not count. */
    for (end = b->vp_start; end + 1 < b->vp_slots && !(b->vp_program[end][3] & 1u); end++) { }
    if (end - b->vp_start + 1 > 136)
        end = b->vp_start + 135;
    key[0] = b->vp_start;
    key[1] = b->vp_slots;
    memcpy(key + 2, b->vp_program[b->vp_start], (size_t)(end - b->vp_start + 1) * 16);
    last_id = wp_vs_key(key, (2 + (end - b->vp_start + 1) * 4) * 4, b->vp_program);
    last_valid = 1;
    last_gen = b->vp_prog_gen;
    last_start = b->vp_start;
    return last_id;
}

static uint32_t wp_fs_key(const WpFsKey *key)
{
    static int told;
    uint64_t h = wp_hash(key, sizeof *key);
    WpEnt *e = wp_find(&s_fs_tab, h, key, sizeof *key);
    uint32_t id;
    int len;

    if (e)
        return e->id;
    len = wp_scratch() ? wgsl_psh(&key->k, key->dim, s_text, WP_TEXT_CAP) : -1;
    id = wp_shader_emit(len, WG_SH_FRAGMENT);
    if (!id && told++ < 8)
        fprintf(stderr, "  [WGPU] combiner program (control %08X, stages %05X) did not"
                " generate: its draws are dropped\n", key->k.control, key->k.shader_program);
    wp_put(&s_fs_tab, h, key, sizeof *key, id);
    wp_list_shader('F', id, key, sizeof *key);
    return id;
}

uint32_t wpipe_fs(const Nv2aPshKey *k, const uint8_t tex_dim[4])
{
    static WpFsKey last;
    static uint32_t last_id;
    static int last_valid;
    WpFsKey key;

    memcpy(&key.k, k, sizeof key.k);
    memcpy(key.dim, tex_dim, 4);
    if (last_valid && !memcmp(&key, &last, sizeof key))
        return last_id;
    last_id = wp_fs_key(&key);
    last = key;
    last_valid = 1;
    return last_id;
}

/* ── Pipelines ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t vs, fs;
    WgPipeKey key;
} WpPipeKey;

static WpTab    s_pipe_tab;
static uint32_t s_pipe_next = 1;

uint32_t wpipe_get(uint32_t vs, uint32_t fs, const WgPipeKey *key)
{
    static WpPipeKey last;
    static uint32_t last_id;
    WpPipeKey k;
    WgPipeCreate *pc;
    uint64_t h;
    WpEnt *e;

    memset(&k, 0, sizeof k);
    k.vs = vs;
    k.fs = fs;
    k.key = *key;
    if (last_id && !memcmp(&k, &last, sizeof k))
        return last_id;
    h = wp_hash(&k, sizeof k);
    e = wp_find(&s_pipe_tab, h, &k, sizeof k);
    if (!e) {
        pc = (WgPipeCreate *)wg_emit(WG_OP_PIPE_CREATE, sizeof *pc);
        pc->id = s_pipe_next++;
        pc->vs = vs;
        pc->fs = fs;
        pc->key = *key;
        wp_put(&s_pipe_tab, h, &k, sizeof k, pc->id);
        last_id = pc->id;
        wp_list_pipe(vs, fs, key);
    } else {
        last_id = e->id;
    }
    last = k;
    return last_id;
}

uint32_t wpipe_count(void) { return s_pipe_next - 1; }

/* ── Prewarm ──────────────────────────────────────────────────────── */

void nv2a_wgpu_prewarm_start(void)
{
    __atomic_store_n(&s_pw_go, 1, __ATOMIC_RELEASE);
}

/* Up to `max` pipelines of the loaded list through the registries (their
 * modules as met); the list is freed at its end. */
static void wp_prewarm_step(uint32_t max)
{
    static uint32_t prog[136][4];
    uint32_t made = 0;
    double t0 = wp_ms();

    while (s_pw_pos < s_pw_len && made < max) {
        uint32_t w, type, n;
        const uint8_t *p = s_pw + s_pw_pos + 4;
        memcpy(&w, s_pw + s_pw_pos, 4);
        type = w & 0xFF;
        n = w >> 8;
        s_pw_pos += 4 + ((n + 3) & ~3u);        /* wp_walk checked every record */
        if (type == 'V') {
            uint32_t key[4 * 136 + 2];
            memcpy(key, p, n);
            if (!n) {
                s_pw_vid[s_pw_vn++] = wp_vs_fixed();
                continue;
            }
            memset(prog, 0, sizeof prog);
            memcpy(prog[key[0]], key + 2, n - 8);
            s_pw_vid[s_pw_vn++] = wp_vs_key(key, n, (const uint32_t (*)[4])prog);
        } else if (type == 'F') {
            WpFsKey fk;
            memcpy(&fk, p, sizeof fk);
            s_pw_fid[s_pw_fn++] = wp_fs_key(&fk);
        } else {
            WpFilePipe fp;
            memcpy(&fp, p, sizeof fp);
            if (s_pw_vid[fp.vi] && s_pw_fid[fp.fi]) {
                wpipe_get(s_pw_vid[fp.vi], s_pw_fid[fp.fi], &fp.key);
                s_pw_pipes++;
            }
            made++;
        }
    }
    s_pw_ms += wp_ms() - t0;
    s_pw_frames++;
    if (s_pw_pos >= s_pw_len) {
        fprintf(stderr, "  [WGPU] %u pipelines prewarmed from " WP_PATH " over %u frames"
                " (%.0f ms of list building; compiles are asynchronous; a deploy's seed:"
                " mc3ExportPipes() on the start page)\n",
                s_pw_pipes, s_pw_frames, s_pw_ms);
        free(s_pw); free(s_pw_vid); free(s_pw_fid);
        s_pw = NULL; s_pw_vid = s_pw_fid = NULL;
    }
}

/* After each frame end (render thread, device ready): a prewarm step once
 * the title started it, new records written every WP_SAVE_FRAMES frames. */
void wpipe_frame_end(void)
{
    wp_load();
    if (s_pw && __atomic_load_n(&s_pw_go, __ATOMIC_ACQUIRE))
        wp_prewarm_step(s_pw_per_frame);
    if (s_cache > 0 && wg_frame - s_saved_frame >= WP_SAVE_FRAMES)
        wp_save();
}
