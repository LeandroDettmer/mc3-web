/**
 * web_fs.c -- the browser's file system (Emscripten WASMFS), set up before
 * the game opens anything (main.c, game_main).
 *
 *   /game   the extracted disc, read on demand over HTTP in 1 MiB blocks
 *           (WASMFS fetch backend, its JS half web/wasmfs_fetch.js: a memory
 *           LRU, then the OPFS disc cache, web/disc_cache.js). Read-only.
 *           <disc>.list names its files, one relative path per line
 *           (tools/web_serve.py makes it; for another server:
 *           `cd game-remix-usa && find . -type f | cut -c3- > ../disc.list`).
 *   /opfs   the Origin Private File System: what the game writes.
 *           /game/UDATA and /game/TDATA (saves and settings: the toolkit maps
 *           Harddisk0\Partition1 onto the game folder, kernel_path.c) link
 *           into it, and MC3_SAVE_DIR (T:, U:, Z:) is /opfs/save. Profiles
 *           live in /opfs/UDATA; their rotating copies in /opfs/backup
 *           (save safety, below).
 *
 * The disc URL is MC3_DISC_URL (the page sets it, web/index.html), else
 * /disc. A MC3_GAME_DIR given (Node.js, NODERAWFS: the host's own files)
 * keeps everything as it is.
 */
#include <emscripten/em_asm.h>
#include <emscripten/em_macros.h>
#include <emscripten/wasmfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WEB_GAME_DIR "/game"
#define WEB_OPFS_DIR "/opfs"

/* mkdir -p of the directories above `path` (inside /game: fetch directories). */
static void make_parents(char *path)
{
    for (char *s = strchr(path + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(path, 0777);   /* writable: files are added to it below */
        *s = '/';
    }
}

/* One file of the disc, named; its bytes come on the first read. */
static int add_disc_file(const char *rel)
{
    char path[1024];
    int fd;
    if (!strncasecmp(rel, "UDATA/", 6) || !strncasecmp(rel, "TDATA/", 6))
        return 0;                   /* a native run's saves: OPFS holds them here */
    snprintf(path, sizeof path, WEB_GAME_DIR "/%s", rel);
    make_parents(path);
    fd = open(path, O_CREAT | O_RDONLY, 0444);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int mount_disc(const char *url)
{
    char list_url[1024], line[1024];
    backend_t list_be, disc_be;
    FILE *f;
    int n = 0, fd;

    /* The file list is fetched like any file: a fetch backend whose one
     * file is the list's URL. */
    snprintf(list_url, sizeof list_url, "%s.list", url);
    list_be = wasmfs_create_fetch_backend(list_url, 0);
    if ((fd = wasmfs_create_file("/disc.list", 0444, list_be)) < 0)
        return -1;
    close(fd);
    f = fopen("/disc.list", "r");
    if (!f)
        return -1;
    /* File URLs come out as <url>//<path> (WASMFS joins with a '/' after a
     * path that starts with one); servers fold the double slash. */
    disc_be = wasmfs_create_fetch_backend(url, 0);   /* blocks: web/wasmfs_fetch.js */
    if (wasmfs_create_directory(WEB_GAME_DIR, 0777, disc_be) != 0) {
        fclose(f);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *rel = line;
        line[strcspn(line, "\r\n")] = 0;
        while (*rel == '.' || *rel == '/')
            rel++;                  /* "./x" from find */
        if (*rel)
            n += add_disc_file(rel);
    }
    fclose(f);
    return n;
}

/* ── Save safety (2026-11-12): no release may damage or lose a player's save ──
 *
 * The game writes a profile in place (NtCreateFile with an overwrite
 * disposition truncates it, kernel_file.c, then streams the bytes): a quota
 * error or a closed tab mid-write leaves a short file. So before each career
 * write (the Remix's sub_00022040 override) every profile file that changed
 * since its last copy is kept, the last 2, under /opfs/backup/<same path>
 * .bak1 (newest) / .bak2 (outside UDATA: the game never lists or deletes
 * them), and at boot and after each write a main file that fails its checks
 * gets the newest good copy back ('[SAVE] restored ...'; the bad one kept as
 * .bad beside the copies). A missing main file is never restored: deleted in
 * the game or on the page means deleted. The Online profile
 * (recomp_manual.c) uses the same helpers with its own checks. Files are
 * replaced through <file>.tmp + rename in one folder, never written in place. */
#define WEB_SAVE_ROOT   WEB_OPFS_DIR "/UDATA"
#define WEB_BACKUP_ROOT WEB_OPFS_DIR "/backup/UDATA"

/* A whole file, malloc'd (NULL: none or unreadable; *n its size). */
uint8_t *mc3_file_read(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d = NULL;
    long sz;
    *n = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (d = malloc(sz ? (size_t)sz : 1)) && fread(d, 1, (size_t)sz, f) == (size_t)sz)
        *n = (size_t)sz;
    else {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

/* `path` becomes d[0..n): written to <path>.tmp, then renamed over it, so a
 * failed write leaves the old file whole. 1 = done. */
int mc3_file_write(const char *path, const void *d, size_t n)
{
    char tmp[1024];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    make_parents(tmp);
    f = fopen(tmp, "wb");
    ok = f && fwrite(d, 1, n, f) == n;
    if (f)
        ok = (fclose(f) == 0) && ok;
    ok = ok && rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    return ok;
}

/* d[0..n) (the current good file) becomes <base>.bak1, the old .bak1 .bak2;
 * nothing moves when .bak1 already holds those bytes. 1 = a copy was made. */
int mc3_backup_rotate(const char *base, const uint8_t *d, size_t n)
{
    char b1[1024], b2[1024];
    size_t n1;
    uint8_t *old;
    int same;
    snprintf(b1, sizeof b1, "%s.bak1", base);
    snprintf(b2, sizeof b2, "%s.bak2", base);
    old = mc3_file_read(b1, &n1);
    same = old && n1 == n && !memcmp(old, d, n);
    if (old && !same)
        mc3_file_write(b2, old, n1);
    free(old);
    if (same)
        return 0;
    if (!mc3_file_write(b1, d, n)) {
        fprintf(stderr, "[SAVE] backup of %s NOT written (storage full?)\n", base);
        return 0;
    }
    return 1;
}

/* kernel_crypto.c (VOID __stdcall, UCHAR/ULONG = uint8_t/uint32_t; __stdcall is empty here) */
void xbox_XcHMAC(const uint8_t *Key, uint32_t KeyLength, const uint8_t *Data1, uint32_t Data1Length,
                 const uint8_t *Data2, uint32_t Data2Length, uint8_t *Digest);

/* Last 20 bytes = HMAC-SHA1 of the rest under the port's all-zero key (the
 * DUB Edition's .mc3, tools/save_import.py; the Remix's is not proven, so a
 * file is held to it only when its backup passes it). */
static int sig_ok(const uint8_t *d, size_t n)
{
    static const uint8_t zero[16];
    uint8_t h[20];
    if (n <= 20)
        return 0;
    xbox_XcHMAC(zero, sizeof zero, d, (uint32_t)(n - 20), NULL, 0, h);
    return !memcmp(h, d + n - 20, 20);
}

/* A career file judged against its newest copy `r` (NULL: none): not empty,
 * not shorter (the formats are fixed-size), the same 4-byte stamp, and
 * signed when the copy is. */
static int career_good(const uint8_t *d, size_t n, const uint8_t *r, size_t rn)
{
    if (!d || !n)
        return 0;
    if (!r || !rn)
        return 1;
    return n >= rn && (n < 4 || rn < 4 || !memcmp(d, r, 4)) && (!sig_ok(r, rn) || sig_ok(d, n));
}

static void snap_copy(const char *path, const uint8_t *d, size_t n);

/* backup 1: keep a copy of each good, changed file; 2: a good file goes into
 * the dated snapshot (snap_copy); 0: restore each bad one */
static void career_file(const char *path, const char *base, int backup)
{
    char b1[1024], b2[1024], bad[1024];
    size_t n, n1, n2;
    uint8_t *d = mc3_file_read(path, &n), *c1, *c2;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(d);                                /* missing: deleted on purpose, never brought back */
        return;
    }
    snprintf(b1, sizeof b1, "%s.bak1", base);
    snprintf(b2, sizeof b2, "%s.bak2", base);
    c1 = mc3_file_read(b1, &n1);
    c2 = mc3_file_read(b2, &n2);
    if (career_good(d, n, c1 ? c1 : c2, c1 ? n1 : n2)) {
        if (backup == 2)
            snap_copy(path, d, n);
        else if (backup)
            mc3_backup_rotate(base, d, n);
    } else if (backup == 2) {
        fprintf(stderr, "[SAVE] snapshot: %s fails its checks, left out\n", path);
    } else if (!backup) {
        const uint8_t *use = career_good(c1, n1, c2, n2) ? c1 : career_good(c2, n2, NULL, 0) ? c2 : NULL;
        size_t un = use == c1 ? n1 : n2;
        if (!use)
            fprintf(stderr, "[SAVE] %s fails its checks (%u bytes) and has no good backup: left as it is\n", path,
                    (unsigned)n);
        else {
            snprintf(bad, sizeof bad, "%s.bad", base);
            if (d && n)
                mc3_file_write(bad, d, n);      /* kept, never deleted */
            if (mc3_file_write(path, use, un))
                fprintf(stderr, "[SAVE] restored %s from backup (%s, %u bytes; the bad one, %u bytes, kept as %s)\n",
                        path, use == c1 ? ".bak1" : ".bak2", (unsigned)un, (unsigned)n, bad);
            else
                fprintf(stderr, "[SAVE] %s fails its checks; restoring its backup failed (storage full?)\n", path);
        }
    }
    free(d);
    free(c1);
    free(c2);
}

/* Every profile file: UDATA/<title>/<profile>/<file>. */
static void career_walk(int backup)
{
    char t[512], p[768], path[1024], base[1024];
    DIR *dt, *dp, *df;
    struct dirent *et, *ep, *ef;
    if (!(dt = opendir(WEB_SAVE_ROOT)))
        return;
    while ((et = readdir(dt))) {
        if (et->d_name[0] == '.')
            continue;
        snprintf(t, sizeof t, "%s", et->d_name);
        snprintf(p, sizeof p, WEB_SAVE_ROOT "/%s", t);
        if (!(dp = opendir(p)))
            continue;
        while ((ep = readdir(dp))) {
            char pd[1024];
            if (ep->d_name[0] == '.')
                continue;
            snprintf(pd, sizeof pd, "%s/%s", p, ep->d_name);
            if (!(df = opendir(pd)))
                continue;
            while ((ef = readdir(df))) {
                size_t l = strlen(ef->d_name);
                if (ef->d_name[0] == '.' || (l > 4 && !strcmp(ef->d_name + l - 4, ".tmp")))
                    continue;
                snprintf(path, sizeof path, "%s/%s", pd, ef->d_name);
                snprintf(base, sizeof base, WEB_BACKUP_ROOT "/%s/%s/%s", t, ep->d_name, ef->d_name);
                career_file(path, base, backup);
            }
            closedir(df);
        }
        closedir(dp);
    }
    closedir(dt);
}

/* Before the game writes a profile: copies of what changed. */
void mc3_web_saves_backup(void) { career_walk(1); }
/* At boot and after a write: bad files get their newest good copy back. */
void mc3_web_saves_check(void) { career_walk(0); }

/* ── Dated snapshots (2026-11-13): on top of the rotating copies above ──
 *
 * At boot, after the check above, when the newest snapshot is 2 days old or
 * more (or there is none), every save that passes its checks is copied into
 * /opfs/backup/snap/<YYYY-MM-DD>/ (UDATA/<title>/<profile>/<file> and
 * online/profile.bin, the same paths as in /opfs), then snapshot.txt is
 * written last: a folder without it was cut short and is removed. A save
 * that fails its checks is left out, never copied. Then snapshots dated
 * more than 7 days before today are removed, but never the newest one (a
 * clock set wrong, forward or back, never empties the folder). Dates are the
 * browser's local clock; folders dated after today do not count for the 2
 * days. The page's Saves tab lists, downloads and restores them (its restore
 * first keeps the current saves as a snapshot "<date>-N"). MC3_SNAP_TEST=1
 * takes one at every boot. Only backup/snap is ever written or removed here. */
#define WEB_SNAP_ROOT WEB_OPFS_DIR "/backup/snap"
#define SNAP_EVERY_DAYS 2
#define SNAP_KEEP_DAYS  7
uint32_t rc_crc32(const uint8_t *p, uint32_t n);    /* xboxrecomp/src/net/roam/roam_core.c */

static char s_snap_dir[256];
static int s_snap_files, s_snap_failed;

/* one good save file -> the snapshot, at its path under /opfs */
static void snap_copy(const char *path, const uint8_t *d, size_t n)
{
    char dst[1024];
    if (s_snap_failed)
        return;
    snprintf(dst, sizeof dst, "%s%s", s_snap_dir, path + strlen(WEB_OPFS_DIR));
    if (mc3_file_write(dst, d, n))
        s_snap_files++;
    else
        s_snap_failed = 1;
}

/* rm -r, for backup/snap only */
static void rm_tree(const char *path)
{
    char p[1024];
    struct dirent *e;
    struct stat st;
    DIR *dir = opendir(path);
    if (dir) {
        while ((e = readdir(dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
                rm_tree(p);
            else
                unlink(p);
        }
        closedir(dir);
        rmdir(path);
    }
}

/* "YYYY-MM-DD..." -> days since 1970-01-01, -1 if not a date */
static long snap_day(const char *name)
{
    int y, m, d;
    long era, yoe, doy;
    if (sscanf(name, "%4d-%2d-%2d", &y, &m, &d) != 3 || y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
        return -1;
    y -= m <= 2;                                /* days_from_civil (H. Hinnant) */
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static void mc3_web_saves_snapshot(void)
{
    char name[64], p[1024];
    long today, newest = -1, newest_past = -1, day;
    int have_today = 0, k, force;
    const char *test = getenv("MC3_SNAP_TEST");
    time_t now = time(NULL);                    /* the browser's clock (Date.now) */
    struct dirent *e;
    struct stat st;
    DIR *dir;

    force = test && test[0] && strcmp(test, "0");
    strftime(name, sizeof name, "%Y-%m-%d", localtime(&now));
    today = snap_day(name);
    mkdir(WEB_OPFS_DIR "/backup", 0777);
    mkdir(WEB_SNAP_ROOT, 0777);
    if ((dir = opendir(WEB_SNAP_ROOT))) {       /* the snapshots there; half-written ones go */
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, WEB_SNAP_ROOT "/%s/snapshot.txt", e->d_name);
            if (stat(p, &st) != 0) {
                snprintf(p, sizeof p, WEB_SNAP_ROOT "/%s", e->d_name);
                rm_tree(p);
                fprintf(stderr, "[SAVE] snapshot %s was cut short: removed\n", e->d_name);
                continue;
            }
            if ((day = snap_day(e->d_name)) < 0)
                continue;
            if (day > newest)
                newest = day;
            if (day <= today && day > newest_past)
                newest_past = day;
            have_today |= day == today;
        }
        closedir(dir);
    }

    if (force || (!have_today && (newest_past < 0 || today - newest_past >= SNAP_EVERY_DAYS))) {
        char base[64];
        uint8_t *d;
        size_t n;
        uint32_t h[4] = { 0 };
        snprintf(base, sizeof base, "%s", name);
        snprintf(s_snap_dir, sizeof s_snap_dir, WEB_SNAP_ROOT "/%s", name);
        for (k = 2; stat(s_snap_dir, &st) == 0; k++) {  /* a second one today (the test switch) */
            snprintf(name, sizeof name, "%s-%d", base, k);
            snprintf(s_snap_dir, sizeof s_snap_dir, WEB_SNAP_ROOT "/%s", name);
        }
        s_snap_files = s_snap_failed = 0;
        career_walk(2);
        /* the Online profile: 'MC3O', its size, its CRC (recomp_manual.c online_profile_check) */
        if ((d = mc3_file_read(WEB_OPFS_DIR "/online/profile.bin", &n))) {
            if (n >= 20)
                memcpy(h, d, sizeof h);
            if (n >= 20 && h[0] == 0x4F33434Du && n - 20 == h[2] && h[3] == rc_crc32(d + 20, h[2]))
                snap_copy(WEB_OPFS_DIR "/online/profile.bin", d, n);
            else
                fprintf(stderr, "[SAVE] snapshot: " WEB_OPFS_DIR "/online/profile.bin fails its checks, left out\n");
            free(d);
        }
        if (s_snap_files && !s_snap_failed) {
            char txt[64];
            int tn = snprintf(txt, sizeof txt, "files %d\n", s_snap_files);
            snprintf(p, sizeof p, "%s/snapshot.txt", s_snap_dir);
            s_snap_failed = !mc3_file_write(p, txt, (size_t)tn);
        }
        if (!s_snap_files) {
            rm_tree(s_snap_dir);
            fprintf(stderr, "[SAVE] snapshot: no saves yet, none taken\n");
        } else if (s_snap_failed) {
            rm_tree(s_snap_dir);
            fprintf(stderr, "[SAVE] snapshot %s NOT written (storage full?): the saves and their copies are as"
                    " they were\n", name);
        } else {
            fprintf(stderr, "[SAVE] snapshot %s: %d files in " WEB_SNAP_ROOT "\n", name, s_snap_files);
            if (today > newest)
                newest = today;
        }
    }

    if (newest >= 0 && (dir = opendir(WEB_SNAP_ROOT))) {   /* older than 7 days, never the newest */
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.' || (day = snap_day(e->d_name)) < 0 || day >= newest ||
                today - day <= SNAP_KEEP_DAYS)
                continue;
            snprintf(p, sizeof p, WEB_SNAP_ROOT "/%s", e->d_name);
            rm_tree(p);
            fprintf(stderr, "[SAVE] snapshot %s removed (%ld days old)\n", e->d_name, today - day);
        }
        closedir(dir);
    }
}

/* Set from the OPFS backend's thread when its root could not be opened
 * (web/wasmfs_fetch.js, _wasmfs_opfs_init_root_directory). */
static volatile int opfs_failed;
EMSCRIPTEN_KEEPALIVE void mc3_web_opfs_failed(void) { opfs_failed = 1; }

/* /opfs on the OPFS backend; -1 if its root cannot be opened. The root is
 * opened on a probe mount first, so a refusal leaves /opfs free for memory. */
static int mount_opfs(void)
{
    backend_t be = wasmfs_create_opfs_backend();
    if (wasmfs_create_directory("/.opfs-probe", 0777, be) != 0 || opfs_failed)
        return -1;
    return wasmfs_create_directory(WEB_OPFS_DIR, 0777, be);
}

void mc3_web_fs_init(void)
{
    const char *url = getenv("MC3_DISC_URL");
    const char *gd = getenv("MC3_GAME_DIR");
    int n;

    if (gd && gd[0])
        return;
    if (!url || !url[0])
        url = "/disc";
    n = mount_disc(url);
    {   /* a deploy's test password rides in the URL as /k/<password>/: log it masked */
        const char *k = strstr(url, "/k/"), *after = k ? strchr(k + 3, '/') : NULL;
        int pre = after ? (int)(k - url) : (int)strlen(url);
        if (n <= 0) {
            fprintf(stderr, "[WEB] no disc at %.*s%s%s (its file list did not load)\n",
                    pre, url, after ? "/k/***" : "", after ? after : "");
            return;                 /* main.c then says the XBE is missing */
        }
        fprintf(stderr, "[WEB] disc: %d files from %.*s%s%s\n", n, pre, url,
                after ? "/k/***" : "", after ? after : "");
    }
    setenv("MC3_GAME_DIR", WEB_GAME_DIR, 1);

    /* OPFS has one root: every directory made with its backend is that root,
     * so it is mounted once and the game's folders link into it. Only in a
     * secure context (https, localhost): plain http to another host has no
     * navigator.storage, nor has Node.js. */
    if (!EM_ASM_INT({ return !!(typeof navigator == 'object' && navigator.storage
                                && navigator.storage.getDirectory); })
        || mount_opfs() != 0) {
        fprintf(stderr, "[WEB] OPFS unavailable: saves last until the page closes\n");
        wasmfs_create_directory(WEB_OPFS_DIR, 0777, wasmfs_create_memory_backend());
    }
    mkdir(WEB_OPFS_DIR "/UDATA", 0777);
    mkdir(WEB_OPFS_DIR "/TDATA", 0777);
    mkdir(WEB_OPFS_DIR "/save", 0777);
    symlink(WEB_OPFS_DIR "/UDATA", WEB_GAME_DIR "/UDATA");
    symlink(WEB_OPFS_DIR "/TDATA", WEB_GAME_DIR "/TDATA");
    if (!getenv("MC3_SAVE_DIR"))
        setenv("MC3_SAVE_DIR", WEB_OPFS_DIR "/save", 1);
    fprintf(stderr, "[WEB] saves in " WEB_OPFS_DIR "\n");
    mc3_web_saves_check();                      /* a profile cut short last time gets its copy back */
    mc3_web_saves_snapshot();                   /* every 2 days, kept 7 */
}
