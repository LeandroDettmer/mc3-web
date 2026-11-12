/* mods.c -- a mods/ folder next to the disc files, read before the game's
 * own data. Included by recomp_manual.c. Remix only, browser and native.
 *
 * Where the game's files come from (traced 2026-11-12):
 *  - Every read-only asset open (the "$/..." names through sub_0020D6F0, the
 *    tune and resource loaders) is sub_001A2280(name, readonly): it calls
 *    Open on the default device [0x004B26E8]. Once the Dave archives are
 *    mounted (sub_002B4120 -> reader sub_002B3D20) that device is the
 *    archive device 0x004B3B18, a table of cdecl function pointers:
 *      +0x00 Open(name, readonly) -> handle 0..15 or -1   sub_002B3B80
 *      +0x08 Read(h, buf, n) -> bytes                     sub_002B3C40
 *      +0x10 Seek(h, off, whence 0/1/2) -> position       sub_002B3C60
 *      +0x14 Close(h) -> 0                                sub_002B2BD0
 *      +0x18 FindFiles(...) (folder listing)              sub_002B3C80
 *      +0x1C Size(h) -> unpacked size                     sub_002B2C00
 *      +0x24 RawInfo(name, toc_out, file_out) -> bool     sub_002B3BD0
 *    Open walks the mounted archives [0x005587CC] (or only [0x005587C4]) and
 *    binary-searches each TOC (sub_002B36B0 -> sub_002B35E0): that is the one
 *    lookup point. A name in no archive fails (-1): there is no fallback to
 *    the disc. RawInfo serves the sector streamer sub_00130E60, which falls
 *    back to sub_001A2280 when it answers 0. All of these are reached only
 *    through that table (indirect calls), so recomp_lookup_manual catches
 *    them without a relift.
 *  - Loose disc files (the flash/ screens, resources/ outside the archives, audio,
 *    video) are opened through the kernel (CreateFile): the toolkit's
 *    xbox_host_path_hook rewrites the host path.
 *
 * The overlay: <disc>/mods/<name> (web: /game/mods/, i.e. a mods/ folder
 * next to the disc's files on the server; native: game-remix-<ed>/mods/),
 * scanned once at boot. A readonly Open of a name found there is served from
 * that file (whole file in memory; handles MODS_H0 + slot); any other Open
 * goes to the archives. A loose disc path found there is redirected. A file
 * there replaces an entry or adds one. '[MOD] <name> from mods/' once per
 * file at its first use. No mods/ folder (or an empty one): no hook is set
 * and the game runs as before.
 *
 * vehicle.lst guard: the showroom holds 127 cars (0x541700, fill
 * sub_0026BD80); a 128th would land on progress[0] at blob +0xA6F0. A
 * mods/tune/vehicle/vehicle.lst with more Vehicle blocks has the extras cut
 * ('[MOD] vehicle.lst: N cars, the last M dropped').
 *
 * Not covered: FindFiles (a folder listing of the archives does not list a
 * mod's added files). MC3_MOD_TRACE=1 logs every archive open's name. */

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

extern void (*xbox_host_path_hook)(char *host_path, size_t size, size_t base_len);
extern void sub_002B3B80(void), sub_002B3C40(void), sub_002B3C60(void), sub_002B2BD0(void),
            sub_002B2C00(void), sub_002B3BD0(void);

#define MODS_VA_LO  0x002B2BD0u         /* the archive device's functions, lowest */
#define MODS_VA_HI  0x002B3C80u         /*   and highest (FindFiles, not hooked) */
#define MODS_H0     0x4D00u             /* our handles: MODS_H0 + slot (the archives' are 0..15) */
#define MODS_OPEN   32
#define MODS_CARS   127                 /* showroom slots */

typedef struct { char *path, *key; int used; } mod_file_t;   /* path: as on disc, under mods/; key: lowercase, '/' */
typedef struct { uint8_t *data; uint32_t size, pos; } mod_open_t;

static mod_file_t *s_mods;
static int s_mods_n, s_mods_cap, s_mods_trace;
static char s_mods_dir[512];
static mod_open_t s_mods_open[MODS_OPEN];
static uint32_t mods_net_vehicles(uint8_t *d, uint32_t n, int content);  /* mods_net.c: mod cars, Donor lines */
static recomp_func_t mods_net_lookup(uint32_t va);           /* mods_net.c: System Link car records */

static void mods_key(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    while (*src == '/' || *src == '\\' || (src[0] == '.' && (src[1] == '/' || src[1] == '\\')))
        src += *src == '.' ? 2 : 1;
    for (; *src && i + 1 < size; src++)
        dst[i++] = *src == '\\' ? '/' : (char)tolower((unsigned char)*src);
    dst[i] = 0;
}

static mod_file_t *mods_find(const char *name)
{
    char key[512];
    mods_key(key, name, sizeof key);
    for (int i = 0; i < s_mods_n; i++)
        if (!strcmp(s_mods[i].key, key))
            return &s_mods[i];
    return NULL;
}

static void mods_used(mod_file_t *m)
{
    if (!m->used) {
        m->used = 1;
        fprintf(stderr, "[MOD] %s from mods/\n", m->key);
    }
}

static void mods_scan(const char *rel)
{
    char dir[1024], path[1024], key[512];
    DIR *d;
    struct dirent *e;
    struct stat st;

    snprintf(dir, sizeof dir, "%s%s%s", s_mods_dir, *rel ? "/" : "", rel);
    if (!(d = opendir(dir)))
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st))
            continue;
        snprintf(path, sizeof path, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        if (S_ISDIR(st.st_mode)) {
            mods_scan(path);
            continue;
        }
        mods_key(key, path, sizeof key);
        if (mods_find(key))
            continue;                   /* the same name in another case */
        if (s_mods_n == s_mods_cap) {
            s_mods_cap = s_mods_cap ? 2 * s_mods_cap : 256;
            s_mods = realloc(s_mods, (size_t)s_mods_cap * sizeof *s_mods);
        }
        s_mods[s_mods_n].path = strdup(path);
        s_mods[s_mods_n].key = strdup(key);
        s_mods[s_mods_n++].used = 0;
    }
    closedir(d);
}

/* Cut every top-level Vehicle block after the 127th; the rest (Manufacturer
 * blocks, comments) stays. Returns the new length. */
static uint32_t mods_cap_vehicles(uint8_t *d, uint32_t n)
{
    uint32_t r = 0, w = 0, cars = 0, dropped = 0;
    int depth = 0, skip = 0, opened = 0;

    while (r < n) {
        uint32_t e = r, a, b;
        while (e < n && d[e] != '\n')
            e++;
        if (e < n)
            e++;
        for (a = r; a < e && isspace(d[a]); a++) {}
        for (b = e; b > a && isspace(d[b - 1]); b--) {}
        if (!skip && depth == 0 && b - a == 7 && !memcmp(d + a, "Vehicle", 7) && ++cars > MODS_CARS) {
            skip = 1;
            dropped++;
        }
        for (uint32_t i = a; i < b; i++)
            depth += d[i] == '{' ? 1 : d[i] == '}' ? -1 : 0;
        if (!skip) {
            memmove(d + w, d + r, e - r);
            w += e - r;
        } else if (depth > 0) {
            opened = 1;
        } else if (opened) {
            skip = opened = 0;
        }
        r = e;
    }
    if (dropped)
        fprintf(stderr, "[MOD] vehicle.lst: %u cars, the last %u dropped (the showroom holds %u)\n",
                cars, dropped, MODS_CARS);
    return w;
}

/* Kernel opens of loose disc files: <game>/<rel> -> <game>/mods/<rel>. */
static void mods_host_path(char *host_path, size_t size, size_t base_len)
{
    char out[1024];
    mod_file_t *m;

    if (host_path[base_len] != '/' || !(m = mods_find(host_path + base_len + 1)))
        return;
    snprintf(out, sizeof out, "%.*s/mods/%s", (int)base_len, host_path, m->path);
    if (strlen(out) < size) {
        strcpy(host_path, out);
        mods_used(m);
    }
}

/* Boot (from the main heap's creation, before any asset is opened). */
static void mods_init(void)
{
    const char *game = getenv("MC3_GAME_DIR");

    snprintf(s_mods_dir, sizeof s_mods_dir, "%s/mods", game && *game ? game : "game");
    mods_scan("");
    if (!s_mods_n)
        return;
    s_mods_trace = getenv("MC3_MOD_TRACE") && atoi(getenv("MC3_MOD_TRACE"));
    xbox_host_path_hook = mods_host_path;
    fprintf(stderr, "[MOD] %d files in %s: read before the archives and the disc\n",
            s_mods_n, s_mods_dir);
}

static mod_open_t *mods_handle(uint32_t h)
{
    return h - MODS_H0 < MODS_OPEN && s_mods_open[h - MODS_H0].data ? &s_mods_open[h - MODS_H0] : NULL;
}

/* +0x00 Open(name, readonly) */
static void mods_dev_open(void)
{
    const char *name = (const char *)XBOX_PTR(MEM32(g_esp + 4));
    mod_file_t *m;
    char path[1024];
    FILE *f;
    long len;
    int slot;

    if (s_mods_trace)
        fprintf(stderr, "[MOD] open %s%s\n", name, MEM32(g_esp + 8) & 0xFF ? "" : " (write)");
    if (!(MEM32(g_esp + 8) & 0xFF) || !(m = mods_find(name))) {
        sub_002B3B80();
        return;
    }
    for (slot = 0; slot < MODS_OPEN && s_mods_open[slot].data; slot++) {}
    snprintf(path, sizeof path, "%s/%s", s_mods_dir, m->path);
    if (slot == MODS_OPEN || !(f = fopen(path, "rb"))) {
        fprintf(stderr, "[MOD] %s: %s, the archive's copy is used\n", m->key,
                slot == MODS_OPEN ? "too many mod files open" : "cannot open");
        sub_002B3B80();
        return;
    }
    /* shortcut: the whole file in memory (car parts are <= ~2 MB); stream it if mods get big */
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_mods_open[slot].data = malloc(len > 0 ? (size_t)len : 1);
    s_mods_open[slot].size = len > 0 && fread(s_mods_open[slot].data, 1, (size_t)len, f) == (size_t)len
                             ? (uint32_t)len : 0;
    s_mods_open[slot].pos = 0;
    fclose(f);
    if (!strcmp(m->key, "tune/vehicle/vehicle.lst"))
        s_mods_open[slot].size = mods_cap_vehicles(s_mods_open[slot].data,
                                                   mods_net_vehicles(s_mods_open[slot].data, s_mods_open[slot].size, 0));
    else if (!strcmp(m->key, "content.lst"))
        s_mods_open[slot].size = mods_net_vehicles(s_mods_open[slot].data, s_mods_open[slot].size, 1);
    mods_used(m);
    g_eax = MODS_H0 + (uint32_t)slot;
    g_esp += 4;
}

/* +0x08 Read(h, buf, n) */
static void mods_dev_read(void)
{
    mod_open_t *o = mods_handle(MEM32(g_esp + 4));
    uint32_t n;

    if (!o) {
        sub_002B3C40();
        return;
    }
    n = MEM32(g_esp + 12);
    if (n > o->size - o->pos)
        n = o->size - o->pos;
    memcpy((void *)XBOX_PTR(MEM32(g_esp + 8)), o->data + o->pos, n);
    o->pos += n;
    g_eax = n;
    g_esp += 4;
}

/* +0x10 Seek(h, off, whence): clamped to [0, size] as the archive's (sub_002B3A70) */
static void mods_dev_seek(void)
{
    mod_open_t *o = mods_handle(MEM32(g_esp + 4));
    uint32_t off, whence, to;

    if (!o) {
        sub_002B3C60();
        return;
    }
    off = MEM32(g_esp + 8);
    whence = MEM32(g_esp + 12);
    if (whence > 2) {
        g_eax = 0xFFFFFFFFu;
    } else {
        to = off + (whence == 1 ? o->pos : whence == 2 ? o->size : 0);
        o->pos = to > o->size ? o->size : to;
        g_eax = o->pos;
    }
    g_esp += 4;
}

/* +0x14 Close(h) */
static void mods_dev_close(void)
{
    mod_open_t *o = mods_handle(MEM32(g_esp + 4));

    if (!o) {
        sub_002B2BD0();
        return;
    }
    free(o->data);
    o->data = NULL;
    g_eax = 0;
    g_esp += 4;
}

/* +0x1C Size(h) */
static void mods_dev_size(void)
{
    mod_open_t *o = mods_handle(MEM32(g_esp + 4));

    if (!o) {
        sub_002B2C00();
        return;
    }
    g_eax = o->size;
    g_esp += 4;
}

/* +0x24 RawInfo(name, ...): "not in an archive" for a mod's name, so the
 * streamer opens it through Open */
static void mods_dev_rawinfo(void)
{
    if (!mods_find((const char *)XBOX_PTR(MEM32(g_esp + 4)))) {
        sub_002B3BD0();
        return;
    }
    g_eax = 0;
    g_esp += 4;
}

static recomp_func_t mods_lookup(uint32_t va)
{
    switch (va) {
    case 0x002B3B80u: return mods_dev_open;
    case 0x002B3C40u: return mods_dev_read;
    case 0x002B3C60u: return mods_dev_seek;
    case 0x002B2BD0u: return mods_dev_close;
    case 0x002B2C00u: return mods_dev_size;
    case 0x002B3BD0u: return mods_dev_rawinfo;
    }
    return (recomp_func_t)0;
}
