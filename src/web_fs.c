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
 *           into it, and MC3_SAVE_DIR (T:, U:, Z:) is /opfs/save.
 *
 * The disc URL is MC3_DISC_URL (the page sets it, web/index.html), else
 * /disc. A MC3_GAME_DIR given (Node.js, NODERAWFS: the host's own files)
 * keeps everything as it is.
 */
#include <emscripten/em_asm.h>
#include <emscripten/wasmfs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
        || wasmfs_create_directory(WEB_OPFS_DIR, 0777, wasmfs_create_opfs_backend()) != 0) {
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
}
