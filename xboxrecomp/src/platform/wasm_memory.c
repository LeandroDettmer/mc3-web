/*
 * Guest memory for WebAssembly (Emscripten) builds.
 *
 * Native hosts reserve 4 GB of host address space and map the guest into it at
 * one offset; the RAM mirrors and the tiled aperture are extra views of the
 * same pages (CreateFileMapping/MapViewOfFileEx, memfd on POSIX). wasm32 has
 * no aliasing mmap and only 4 GB in all, so here guest memory is a table:
 * guest granule g (1 MB) lives at host g_xbox_region_off[g] + VA. Real storage
 * is only what exists on the console -- RAM, the contiguous window, the device
 * apertures -- and an alias is granules pointing at storage that is already
 * someone else's. xbox_memory_layout.c builds the layout through the three
 * calls below; XBOX_PTR (recomp_types.h) and xbox_ptr (kernel/xbox_guest_ptr.h)
 * read the table.
 *
 * Granules nothing backs point at one shared scratch megabyte: a stray access
 * lands there rather than on whatever wasm memory sits at the guest VA (a
 * native host would fault; nothing can fault here).
 */
#if defined(__EMSCRIPTEN__)

#include "../kernel/xbox_guest_ptr.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRANULE    (1u << XBOX_GRANULE_SHIFT)
#define MAX_BLOCKS 1024

uint32_t g_xbox_region_off[XBOX_GRANULE_COUNT];

/* Storage handed out, for host -> guest lookups. */
typedef struct { uint8_t *host; uint32_t va; uint32_t size; } wasm_block;
static wasm_block s_blocks[MAX_BLOCKS];
static int s_nblocks;

enum { G_UNBACKED = 0, G_BACKED, G_ALIAS };
static uint8_t s_state[XBOX_GRANULE_COUNT];
static uint8_t *s_sink;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_granule(uint32_t g, const uint8_t *host)
{
    g_xbox_region_off[g] = (uint32_t)(uintptr_t)host - (g << XBOX_GRANULE_SHIFT);
}

static int table_init_locked(void)
{
    uint32_t g;

    if (s_sink)
        return 1;
    /* Slack past the megabyte: an access straddling the sink's end. */
    s_sink = aligned_alloc(4096, GRANULE + 4096);
    if (!s_sink)
        return 0;
    memset(s_sink, 0, GRANULE + 4096);
    for (g = 0; g < XBOX_GRANULE_COUNT; g++)
        set_granule(g, s_sink);
    return 1;
}

/* Granule range [*g0, *g0 + *n) covering [va, va + size); 0 if it runs past
 * the top of the 32-bit guest space or is empty. */
static int granules(uint32_t va, uint32_t size, uint32_t *g0, uint32_t *n)
{
    uint64_t end = (uint64_t)va + size;

    if (!size || end > 0x100000000ULL)
        return 0;
    *g0 = va >> XBOX_GRANULE_SHIFT;
    *n = (uint32_t)(((end + GRANULE - 1) >> XBOX_GRANULE_SHIFT) - *g0);
    return 1;
}

static int range_unbacked(uint32_t g0, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (s_state[g0 + i] != G_UNBACKED)
            return 0;
    return 1;
}

/* Granules below this live at wasm address == guest VA: lifted code reaches
 * them without the table (recomp_types.h, XBOX_RAM_IDENT). The link keeps
 * Emscripten's data, stacks and heap above (-sGLOBAL_BASE, CMakeLists.txt). */
#define IDENT_GRANULES (0x04000000u >> XBOX_GRANULE_SHIFT)

static void check_ident_free(void)
{
    static int done;
    int local;
    if (done++)
        return;
    if ((uintptr_t)&s_lock < 0x04000000u || (uintptr_t)&local < 0x04000000u) {
        fprintf(stderr, "  [WASM] runtime data below 64 MB (link without -sGLOBAL_BASE=0x04000000?):"
                        " guest RAM cannot sit at wasm 0\n");
        abort();
    }
}

/* New zeroed storage for n granules from g0. Caller holds the lock. 1 on
 * success: RAM's host address is 0 (wasm address == guest VA), so not a
 * pointer. */
static int back_locked(uint32_t g0, uint32_t n)
{
    size_t bytes = (size_t)n << XBOX_GRANULE_SHIFT;
    uint8_t *host;
    uint32_t i;

    if (s_nblocks >= MAX_BLOCKS)
        return 0;
    if (g0 < IDENT_GRANULES && g0 + n > IDENT_GRANULES) {
        /* One block must be contiguous on the host (g_memory_base + x):
         * 128 MB of devkit RAM cannot be, so it is not offered here. */
        fprintf(stderr, "  [WASM] guest storage straddles 64 MB (devkit RAM?): not supported\n");
        return 0;
    }
    if (g0 < IDENT_GRANULES) {
        check_ident_free();
        /* Below -sGLOBAL_BASE nothing has written: fresh wasm memory is
         * zero. No memset either: host may be address 0, and this file is
         * not built with -fno-delete-null-pointer-checks. */
        host = (uint8_t *)(uintptr_t)(g0 << XBOX_GRANULE_SHIFT);
    } else if (!(host = aligned_alloc(4096, bytes))) {
        return 0;
    } else {
        memset(host, 0, bytes);
    }
    for (i = 0; i < n; i++) {
        set_granule(g0 + i, host + ((size_t)i << XBOX_GRANULE_SHIFT));
        s_state[g0 + i] = G_BACKED;
    }
    s_blocks[s_nblocks].host = host;
    s_blocks[s_nblocks].va = g0 << XBOX_GRANULE_SHIFT;
    s_blocks[s_nblocks].size = n << XBOX_GRANULE_SHIFT;
    s_nblocks++;
    return 1;
}

void *xbox_wasm_back(uint32_t va, uint32_t size)
{
    uint32_t g0, n;
    void *ret = NULL;

    if (!granules(va, size, &g0, &n))
        return NULL;
    pthread_mutex_lock(&s_lock);
    if (table_init_locked() && range_unbacked(g0, n) && back_locked(g0, n))
        ret = (void *)XBOX_GUEST_ADDR(va);
    pthread_mutex_unlock(&s_lock);
    if (!ret)
        fprintf(stderr, "  [WASM] no storage for 0x%08X+0x%X\n", va, size);
    return ret;
}

void *xbox_wasm_alias(uint32_t va, uint32_t size, uint32_t target_va)
{
    uint32_t g0, n, t0, tn, i;
    void *ret = NULL;

    /* Granule-aligned both ends: an alias is whole granules of storage. */
    if (((va | target_va) & (GRANULE - 1)) || !granules(va, size, &g0, &n)
            || !granules(target_va, size, &t0, &tn))
        return NULL;
    pthread_mutex_lock(&s_lock);
    if (table_init_locked() && range_unbacked(g0, n)) {
        for (i = 0; i < n && s_state[t0 + i] != G_UNBACKED; i++)
            ;
        if (i == n) {
            for (i = 0; i < n; i++) {
                uint32_t tva = (t0 + i) << XBOX_GRANULE_SHIFT;
                set_granule(g0 + i, (const uint8_t *)XBOX_GUEST_ADDR(tva));
                s_state[g0 + i] = G_ALIAS;
            }
            ret = (void *)XBOX_GUEST_ADDR(va);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int xbox_wasm_commit(uint32_t va, uint32_t size)
{
    uint32_t g0, n, i, run;
    int ok = 1;

    if (!granules(va, size, &g0, &n))
        return 0;
    pthread_mutex_lock(&s_lock);
    if (!table_init_locked())
        ok = 0;
    /* One block per run of unbacked granules; backed ones are already live. */
    for (i = 0; ok && i < n; i += run) {
        run = 1;
        if (s_state[g0 + i] != G_UNBACKED)
            continue;
        while (i + run < n && s_state[g0 + i + run] == G_UNBACKED)
            run++;
        if (!back_locked(g0 + i, run))
            ok = 0;
    }
    pthread_mutex_unlock(&s_lock);
    if (!ok)
        fprintf(stderr, "  [WASM] commit of 0x%08X+0x%X failed\n", va, size);
    return ok;
}

uint32_t xbox_native_to_va(const void *p)
{
    uintptr_t h = (uintptr_t)p;
    uint32_t va = 0;
    int i;

    pthread_mutex_lock(&s_lock);
    for (i = 0; i < s_nblocks; i++) {
        uintptr_t lo = (uintptr_t)s_blocks[i].host;
        if (h >= lo && h - lo < s_blocks[i].size) {
            va = s_blocks[i].va + (uint32_t)(h - lo);
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return va;
}

#endif /* __EMSCRIPTEN__ */
