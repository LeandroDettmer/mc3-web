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
 * The overlay: packs, <disc>/mods/<id>/<name> (web: /game/mods/, i.e. a
 * mods/ folder next to the disc's files on the server; native:
 * game-remix-<ed>/mods/), each with mods/<id>/mod.json {id, name, author,
 * credit, description, version, kind} and listed in mods/index.json (both
 * for the page; the game reads neither). Only the packs named in MC3_MODS
 * (comma-separated ids, the page's Mods tab) are scanned, once at boot, in
 * that order: a name two packs give comes from the later one ('[MOD] <name>:
 * pack B over pack A'). <name> is the disc path inside the pack
 * (content.lst, strings.txt, flash/..., tune/...). A readonly Open of a name
 * found there is served from that file (whole file in memory; handles
 * MODS_H0 + slot); any other Open goes to the archives. A loose disc path
 * found there is redirected. A file there replaces an entry or adds one.
 * '[MOD] <name> from mods/<id>/' once per file at its first use. MC3_MODS
 * unset or empty, or no file in its packs: no hook is set and the game runs
 * as before.
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

typedef struct { char *path, *key; int used; } mod_file_t;   /* path: <id>/<name> as on disc, under mods/; key: <name> lowercase, '/' */
typedef struct { uint8_t *data; uint32_t size, pos; } mod_open_t;

static mod_file_t *s_mods;
static int s_mods_n, s_mods_cap, s_mods_trace;
static char s_mods_dir[512];
static mod_open_t s_mods_open[MODS_OPEN];
static uint32_t mods_net_vehicles(uint8_t *d, uint32_t n, int content);  /* mods_net.c: mod cars, Donor lines */
static uint8_t *mods_str_strtbl(uint32_t *size);              /* strings.txt as mcdlstrings_x.strtbl */
static int mods_str_n;                                        /* strings.txt lines (defined below) */
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
        fprintf(stderr, "[MOD] %s from mods/%.*s/\n", m->key, (int)strcspn(m->path, "/"), m->path);
    }
}

/* Pack <id>'s files under <rel> (path inside the pack); returns how many. */
static int mods_scan(const char *id, const char *rel)
{
    char dir[1024], path[1024], key[512];
    size_t idn = strlen(id);
    DIR *d;
    struct dirent *e;
    struct stat st;
    mod_file_t *m;
    int n = 0;

    snprintf(dir, sizeof dir, "%s/%s%s%s", s_mods_dir, id, *rel ? "/" : "", rel);
    if (!(d = opendir(dir)))
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || (!*rel && !strcasecmp(e->d_name, "mod.json")))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st))
            continue;
        snprintf(path, sizeof path, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        if (S_ISDIR(st.st_mode)) {
            n += mods_scan(id, path);
            continue;
        }
        mods_key(key, path, sizeof key);
        snprintf(path, sizeof path, "%s/%s%s%s", id, rel, *rel ? "/" : "", e->d_name);
        n++;
        if ((m = mods_find(key)) != NULL) {
            if (strncmp(m->path, id, idn) || m->path[idn] != '/') {   /* another pack's: the later one wins */
                fprintf(stderr, "[MOD] %s: pack %s over pack %.*s\n", key, id, (int)strcspn(m->path, "/"), m->path);
                free(m->path);
                m->path = strdup(path);
            }
            continue;                   /* else the same name in another case */
        }
        if (s_mods_n == s_mods_cap) {
            s_mods_cap = s_mods_cap ? 2 * s_mods_cap : 256;
            s_mods = realloc(s_mods, (size_t)s_mods_cap * sizeof *s_mods);
        }
        s_mods[s_mods_n].path = strdup(path);
        s_mods[s_mods_n].key = strdup(key);
        s_mods[s_mods_n++].used = 0;
    }
    closedir(d);
    return n;
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

/* Kernel opens of loose disc files: <game>/<rel> -> <game>/mods/<id>/<rel>. */
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
    const char *game = getenv("MC3_GAME_DIR"), *list = getenv("MC3_MODS"), *p;
    char id[64];

    if (!list || !*list)
        return;                         /* no pack picked: the game as it is */
    snprintf(s_mods_dir, sizeof s_mods_dir, "%s/mods", game && *game ? game : "game");
    for (p = list; *p; p += *p == ',') {
        size_t k = strcspn(p, ","), i;
        if (k == 0 || k >= sizeof id) {
            p += k;
            continue;
        }
        memcpy(id, p, k);
        id[k] = 0;
        p += k;
        for (i = 0; i < k && (isalnum((unsigned char)id[i]) || id[i] == '_' || id[i] == '-'); i++) {}
        if (i < k) {                    /* a folder name only: no '/', '..' */
            fprintf(stderr, "[MOD] pack '%s': not a pack id, skipped\n", id);
            continue;
        }
        fprintf(stderr, "[MOD] pack %s: %d files\n", id, mods_scan(id, ""));
    }
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

    if (s_mods_trace) {                 /* every archive open; " <- mods/<id>" when a pack serves it */
        mod_file_t *t = (MEM32(g_esp + 8) & 0xFF) ? mods_find(name) : NULL;
        fprintf(stderr, "[MOD] open %s%s%s%.*s\n", name, MEM32(g_esp + 8) & 0xFF ? "" : " (write)",
                t ? " <- mods/" : "", t ? (int)strcspn(t->path, "/") : 0, t ? t->path : "");
    }
    for (slot = 0; slot < MODS_OPEN && s_mods_open[slot].data; slot++) {}
    if ((MEM32(g_esp + 8) & 0xFF) && !mods_find(name) && slot < MODS_OPEN) {
        const char *base = name + strlen(name);
        while (base > name && base[-1] != '/' && base[-1] != '\\')
            base--;
        if (!strcasecmp(base, "mcdlstrings_x.strtbl")
            && (s_mods_open[slot].data = mods_str_strtbl(&s_mods_open[slot].size)) != NULL) {
            static int said;
            if (!said++)
                fprintf(stderr, "[MOD] strings.txt as %s: %d texts for the DLC string table, "
                        "merged at every string reload\n", name, mods_str_n);
            s_mods_open[slot].pos = 0;
            g_eax = MODS_H0 + (uint32_t)slot;
            g_esp += 4;
            return;
        }
    }
    if (!(MEM32(g_esp + 8) & 0xFF) || !(m = mods_find(name))) {
        sub_002B3B80();
        return;
    }
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

/* ── Late fixes, from the main menu's build and the per-frame pass ──
 *
 * (1) DLC cars: the loader (sub_0026A720, reached by direct calls, so not
 * hookable without a relift) fills record +0x18 (mask) and appends name and
 * price to the DLC lists, never writing record +0x1C, the DLC slot the
 * showroom (sub_0014AE60) looks for: -1 = skipped, $0 and class Z in the
 * Flash. Here every record with +0x18 != 0 gets +0x1C = the index of its
 * name in the DLC name list [0x557EA8] (count [0x557EAC]); the name is the
 * record's +0 (the list holds that same value; a char* compared as text
 * too). Records [0x557EB0], count [0x557EB4], stride 0x28.
 * (2) mods/strings.txt, lines 'key=UTF-8 text': each key missing from a
 * loaded language table is added with the game's own sub_000EA750 (this =
 * 0x507E08, the table picked by its +0x28; the new entry's text is the key,
 * replaced by a buffer of the real text from the game's heap, the old one
 * left; existing keys are never touched). Every language whose table is
 * loaded; the pass waits for the current language's. Checked every pass,
 * not once per table: a lookup of a key before it was added (or in a table
 * reloaded at the same address) makes sub_000EA750 add it itself with the
 * text "!!key!!" (or the bare key), and such an entry gets our text too.
 * That alone did not reach the showroom: every string reload drops these
 * entries and nothing runs this pass in the garage, so the showroom still
 * showed "!!vp_sandero_rs!!" (2026-11-13). The texts now also reach the
 * tables the game's own way, as its DLC string table (mods_str_strtbl). */
#define MODS_STR_MAX 128
#define MODS_STR_LANGS 10
static uint32_t mods_dlc_done_list, mods_dlc_done_n;
static uint32_t mods_str_buf[MODS_STR_LANGS][MODS_STR_MAX];   /* our text per language and key */
static char *mods_str_key[MODS_STR_MAX], *mods_str_val[MODS_STR_MAX];
static int mods_str_n, mods_str_loaded;
extern void sub_000EA750(void), sub_001B3B90(void);
static int online_ram(uint32_t va);
static uint32_t str_table(uint32_t *n, uint32_t *buckets);
static uint32_t str_text(uint32_t n, uint32_t buckets, const char *key);
static uint32_t str_hash(const char *k);
static uint32_t roam_call(recomp_func_t fn, uint32_t ecx, uint32_t top, int n, const uint32_t *args);

static void mods_dlc_fix(void)
{
    uint32_t names = MEM32(0x00557EA8u), cnt = MEM16(0x00557EACu);
    uint32_t recs = MEM32(0x00557EB0u), n = MEM16(0x00557EB4u), i, j;

    if (!cnt || !n || !online_ram(names) || !online_ram(recs)
        || (names == mods_dlc_done_list && cnt == mods_dlc_done_n))
        return;
    if (n > MODS_CARS)
        n = MODS_CARS;
    for (i = 0; i < n; i++) {
        uint32_t rec = recs + i * 0x28u, nm = MEM32(rec);
        if (!MEM32(rec + 0x18u))
            continue;
        for (j = 0; j < cnt; j++) {
            uint32_t c = MEM32(names + 4u * j);
            if (c == nm || (online_ram(c) && online_ram(nm)
                            && !strcmp((const char *)XBOX_PTR(c), (const char *)XBOX_PTR(nm))))
                break;
        }
        if (j == cnt || MEM32(rec + 0x1Cu) == j)
            continue;
        MEM32(rec + 0x1Cu) = j;
        fprintf(stderr, "[MOD] DLC car %s: slot %u\n",
                online_ram(nm) ? (const char *)XBOX_PTR(nm) : "?", j);
    }
    mods_dlc_done_list = names;
    mods_dlc_done_n = cnt;
}

static void mods_str_load(void)
{
    mod_file_t *m = mods_find("strings.txt");
    char path[1024], line[1024];
    FILE *f;

    mods_str_loaded = 1;
    if (!m)
        return;
    snprintf(path, sizeof path, "%s/%s", s_mods_dir, m->path);
    if (!(f = fopen(path, "rb")))
        return;
    while (mods_str_n < MODS_STR_MAX && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *e;
        if (!eq || line[0] == '#' || eq == line)
            continue;
        *eq = 0;
        for (e = eq + strlen(eq + 1); e > eq && isspace((unsigned char)*e); e--)
            *e = 0;
        mods_str_key[mods_str_n] = strdup(line);
        mods_str_val[mods_str_n++] = strdup(eq + 1);
    }
    fclose(f);
    mods_used(m);
}

/* UTF-8 -> UTF-16 LE bytes at out (NULL = count only), 0-ended; returns the units, without the 0. */
static uint32_t mods_utf16(uint8_t *out, const char *s)
{
    uint32_t k = 0, cp;
    const uint8_t *u = (const uint8_t *)s;
    while (*u) {
        int extra = *u >= 0xF0 ? 3 : *u >= 0xE0 ? 2 : *u >= 0xC0 ? 1 : 0;
        cp = extra ? *u & (0x3Fu >> extra) : *u;
        for (u++; extra-- > 0 && (*u & 0xC0) == 0x80; u++)
            cp = (cp << 6) | (*u & 0x3Fu);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            if (out) {
                out[2 * k] = (uint8_t)(cp >> 10);
                out[2 * k + 1] = (uint8_t)(0xD8u | (cp >> 18));
            }
            k++;
            cp = 0xDC00u | (cp & 0x3FFu);
        }
        if (out) {
            out[2 * k] = (uint8_t)cp;
            out[2 * k + 1] = (uint8_t)(cp >> 8);
        }
        k++;
    }
    if (out)
        out[2 * k] = out[2 * k + 1] = 0;
    return k;
}

/* mods/strings.txt as the game's DLC string table: every string reload (boot,
 * profile, each race or cruise load: sub_00022850, sub_00029240) loads
 * mcstrings_x into the language slots, then sub_00022780 opens
 * fonts/mcdlstrings_x.strtbl in every archive and merges it into each slot
 * (sub_000EA480 with merge 1), as Xbox Live content shipped its car names. A
 * reload drops whatever was added to the tables before, so the texts set at
 * boot by mods_strings were gone when the garage opened: the showroom's
 * lookup (sub_000EA750, this 0x507E08) then added "!!vp_sandero_rs!!"
 * (2026-11-13). Format (sub_000EA480, entries sub_000EA320): u32 languages,
 * u32 offset per language (all ours point at one block), u32 0x200, u32 keys
 * + (u32 len, chars, 0) each (read only with keys on; DLC loads skip them);
 * the block: u32 count, then per entry u32 hash (sub_000EA710), u16 kind
 * (0 = skipped; car names 15), u32 n + font name (n chars), u32 n +
 * UTF-16 text (n units with its 0), f32 1.0, f32 1.0, u8 0, u8 0. */
static void mods_put(uint8_t **b, uint32_t *n, const void *p, uint32_t k)
{
    *b = realloc(*b, *n + k);
    memcpy(*b + *n, p, k);
    *n += k;
}

static void mods_put32(uint8_t **b, uint32_t *n, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    mods_put(b, n, t, 4);
}

static uint8_t *mods_str_strtbl(uint32_t *size)
{
    static const uint8_t kind[2] = { 15, 0 }, tail[10] = { 0, 0, 0x80, 0x3F, 0, 0, 0x80, 0x3F, 0, 0 };
    uint8_t *b = NULL;
    uint32_t n = 0, at, i, k;

    if (!mods_str_loaded)
        mods_str_load();
    if (!mods_str_n)
        return NULL;
    mods_put32(&b, &n, 10);
    for (i = 0; i < 10; i++)
        mods_put32(&b, &n, 0);                  /* offsets, below */
    mods_put32(&b, &n, 0x200);
    mods_put32(&b, &n, (uint32_t)mods_str_n);
    for (i = 0; i < (uint32_t)mods_str_n; i++) {
        k = (uint32_t)strlen(mods_str_key[i]);
        mods_put32(&b, &n, k);
        mods_put(&b, &n, mods_str_key[i], k + 1);
    }
    for (at = n, i = 0; i < 10; i++)
        for (k = 0; k < 4; k++)
            b[4 + 4 * i + k] = (uint8_t)(at >> (8 * k));
    mods_put32(&b, &n, (uint32_t)mods_str_n);
    for (i = 0; i < (uint32_t)mods_str_n; i++) {
        mods_put32(&b, &n, str_hash(mods_str_key[i]));
        mods_put(&b, &n, kind, 2);
        mods_put32(&b, &n, 10);
        mods_put(&b, &n, "smallspace", 10);
        k = mods_utf16(NULL, mods_str_val[i]) + 1;
        mods_put32(&b, &n, k);
        b = realloc(b, n + 2 * k);
        mods_utf16(b + n, mods_str_val[i]);
        n += 2 * k;
        mods_put(&b, &n, tail, 10);
    }
    *size = n;
    return b;
}

/* The garage opens (state 5): what the string table holds now for each strings.txt key. */
static void mods_garage_strings(void)
{
    uint32_t n, buckets, t, i, k, have = 0;
    char first[96] = "";

    if (!s_mods_n || !mods_str_n || !str_table(&n, &buckets))
        return;
    for (i = 0; i < (uint32_t)mods_str_n; i++) {
        if (!(t = str_text(n, buckets, mods_str_key[i])))
            continue;
        if (!have++) {
            int w = snprintf(first, sizeof first, "%s = \"", mods_str_key[i]);
            for (k = 0; k < 48u && MEM16(t + 2u * k) && w + 2 < (int)sizeof first; k++)
                first[w++] = MEM16(t + 2u * k) < 0x80u ? (char)MEM16(t + 2u * k) : '?';
            first[w++] = '"';
            first[w] = 0;
        }
    }
    fprintf(stderr, "[MOD] garage: strings.txt keys in the string table (language %u): %u of %d%s%s\n",
            MEM32(0x00507E08u + 0x28u), have, mods_str_n, have ? ", " : "", first);
}

/* The entry of key in a table (str_text's walk), 0 if none. */
static uint32_t mods_str_entry(uint32_t n, uint32_t buckets, const char *key)
{
    uint32_t h = str_hash(key), node = MEM32(buckets + 4u * (h % n));
    unsigned guard = 0;
    for (; online_ram(node) && guard < 100000u; node = MEM32(node + 8u), guard++)
        if (MEM32(node) == h)
            return online_ram(MEM32(node + 4u)) ? MEM32(node + 4u) : 0;
    return 0;
}

/* 1 if the UTF-16 text at t is sub_000EA750's own for a missing key: "!!key!!" or key. */
static int mods_str_stub(uint32_t t, const char *key)
{
    size_t k = strlen(key), i, at = 0;
    if (!online_ram(t))
        return 1;
    if (MEM16(t) == '!' && MEM16(t + 2u) == '!')
        at = 2;
    for (i = 0; i < k; i++)
        if (MEM16(t + 2u * (at + i)) != (uint8_t)key[i])
            return 0;
    return at ? MEM16(t + 2u * (at + k)) == '!' && MEM16(t + 2u * (at + k + 1)) == '!'
                && !MEM16(t + 2u * (at + k + 2)) : !MEM16(t + 2u * k);
}

static void mods_strings(void)
{
    uint32_t n, buckets, lang, cur = MEM32(0x00507E08u + 0x28u), top, keybuf;
    int i;

    if (!mods_str_loaded)
        mods_str_load();
    if (!mods_str_n || !str_table(&n, &buckets) || !str_text(n, buckets, "CM_Networking"))
        return;                                 /* none, or the current language is not loaded yet */
    keybuf = (g_esp - 0x400u) & ~0xFu;
    top = keybuf - 0x40u;
    for (lang = 0; lang < MODS_STR_LANGS; lang++) {
        uint32_t t = MEM32(0x00507E08u + 4u * lang), added = 0, fixed = 0;
        if (!online_ram(t))
            continue;
        MEM32(0x00507E08u + 0x28u) = lang;
        if (str_table(&n, &buckets) != t)
            continue;
        for (i = 0; i < mods_str_n; i++) {
            uint32_t entry, units, buf, a;
            if (strlen(mods_str_key[i]) >= 0x100u)
                continue;
            entry = mods_str_entry(n, buckets, mods_str_key[i]);
            if (entry && (MEM32(entry + 8u) == mods_str_buf[lang][i]
                          || !mods_str_stub(MEM32(entry + 8u), mods_str_key[i])))
                continue;                       /* ours, or the game's own text */
            if (!entry) {
                memcpy((char *)XBOX_PTR(keybuf), mods_str_key[i], strlen(mods_str_key[i]) + 1);
                a = keybuf;
                entry = roam_call(sub_000EA750, 0x00507E08u, top, 1, &a);
            } else
                fixed++;
            units = mods_utf16(NULL, mods_str_val[i]);
            a = 2 * (units + 1);
            buf = online_ram(entry) ? roam_call(sub_001B3B90, 0, top, 1, &a) : 0;
            if (!online_ram(buf))
                continue;
            mods_utf16((uint8_t *)XBOX_PTR(buf), mods_str_val[i]);
            MEM32(entry + 8u) = buf;
            mods_str_buf[lang][i] = buf;
            added++;
        }
        if (added)
            fprintf(stderr, "[MOD] strings.txt: %u texts set in language %u (%u replaced a missing-key text)\n",
                    added, lang, fixed);
    }
    MEM32(0x00507E08u + 0x28u) = cur;
}

static void mods_late(void)
{
    if (!s_mods_n)
        return;
    mods_dlc_fix();
    mods_strings();
}
