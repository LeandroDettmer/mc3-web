/*
 * difftest.c -- snapshot the guest around one lifted function call, so
 * tools/difftest.py can run the *original* x86 code from the same state in
 * an emulator (Unicorn) and report where the lifted C disagrees.
 *
 * A capture file holds the guest CPU registers and two memory windows:
 *   0x00000000 + RAM size     main RAM: code, data, heap, stacks
 *   0x80000000 + 64 MB        the contiguous window (MmAllocateContiguous...:
 *                             Bink's frame buffers, D3D's resources). Not an
 *                             alias of RAM in this runtime, so captured too.
 *
 * Format (little-endian, no padding), "MC3DIFF1":
 *   char magic[8]; u32 version=1; u32 func; u32 call_index;
 *   u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
 *   u32 fp_top; u32 fp_control_word; f64 fp_stack[8];
 *   u64 mm[8]; u8 xmm[8][16];
 *   u32 region_count; then per region: u32 va, u32 size, size bytes.
 *
 * Used by wrappers in src/recomp_manual.c, armed with MC3_DIFFTEST_CALL=<n>
 * (capture the n-th call). Off by default.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, mode) _mkdir(p)          /* Windows: no mode argument */
#endif

#include "recomp_types.h"

extern size_t xbox_GetMappedSize(void);   /* RAM bytes (64 MB retail) */

/* recomp_types.h declares the MMX registers only for generated code; they are
 * 8-byte thread-local unions in the runtime, read here as their 64 bits. */
extern RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;

#define CONTIG_VA   0x80000000u
extern uint32_t g_xbox_contig_size;      /* 64 MB, 128 with RECOMP_RAM_MB=128 */
#define CONTIG_SIZE g_xbox_contig_size

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static int write_region(FILE *f, uint32_t va, uint32_t size)
{
    put32(f, va);
    put32(f, size);
    return fwrite((const void *)XBOX_PTR(va), 1, size, f) == size ? 0 : -1;
}

/* Run one wrapped call; on the armed one, capture before/after and end.
 * Armed by MC3_DIFFTEST_CALL=<n> and MC3_DIFFTEST_FUNC=<hex address> (the
 * function to capture; several can be wrapped at once). */
static uint32_t difftest_func(void)
{
    static long func = -1;
    if (func < 0) {
        const char *e = getenv("MC3_DIFFTEST_FUNC");
        func = e ? strtol(e, NULL, 16) : 0;
    }
    return (uint32_t)func;
}

long mc3_difftest_wanted(void);
void mc3_difftest_capture(const char *phase, uint32_t func, long call_index);

/* MC3_DIFFTEST_WITHIN=<hex func>:<n>: count the armed function's calls only
 * while call n of that outer function runs ("call 59 of the IDCT within
 * frame 600"). The outer function must be wrapped too. */
static uint32_t s_within_func;
static long s_within_call;
static int s_within_active;

static void within_init(void)
{
    static int done;
    const char *e;
    if (done) return;
    done = 1;
    e = getenv("MC3_DIFFTEST_WITHIN");
    if (e && *e) {
        char *colon;
        s_within_func = (uint32_t)strtoul(e, &colon, 16);
        s_within_call = (*colon == ':') ? atol(colon + 1) : 0;
    }
}

/* MC3_DIFFTEST_CALL=key: capture on demand, from a run played by hand.
 * After F10, the next call of each wrapped function (MC3_DIFFTEST_FUNC=all,
 * or addresses: 2BDEC0,2C1520) is captured once; the run ends 2-3 s later or after 16
 * captures. Desktop builds only (the SDL keyboard). */
#if !defined(__SWITCH__)
int xbox_sdl_keyboard_down(int scancode);
#define DIFFTEST_KEY 67                     /* SDL_SCANCODE_F10 */
#endif

static int key_mode(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_DIFFTEST_CALL");
        on = e && strcmp(e, "key") == 0;
        if (on)
            fprintf(stderr, "[DIFFTEST] armed by key: press F10 to capture the next call of %s\n",
                    getenv("MC3_DIFFTEST_FUNC") ? getenv("MC3_DIFFTEST_FUNC") : "every wrapped function");
    }
    return on;
}

static int key_call(uint32_t func, void (*lifted)(void), long *calls)
{
#if defined(__SWITCH__)
    (void)func; (void)calls;
    lifted();
    return 1;
#else
    static uint32_t done[16];
    static int ndone;
    static time_t armed_at;
    const char *want = getenv("MC3_DIFFTEST_FUNC");
    int i, all = !want || !*want || strcmp(want, "all") == 0;
    int listed = all;

    (*calls)++;
    if (!armed_at && xbox_sdl_keyboard_down(DIFFTEST_KEY)) {
        armed_at = time(NULL);
        fprintf(stderr, "[DIFFTEST] F10: capturing\n");
    }
    if (!all) {                             /* "2BDEC0,2C1520": a list */
        const char *p = want;
        while (*p && !listed) {
            char *end;
            if ((uint32_t)strtoul(p, &end, 16) == func)
                listed = 1;
            p = *end == ',' ? end + 1 : end + (*end != 0);
        }
    }
    if (!armed_at || !listed) {
        lifted();
        return 1;
    }
    for (i = 0; i < ndone; i++)
        if (done[i] == func)
            break;
    if (i < ndone || ndone == 16) {
        lifted();
    } else {
        done[ndone++] = func;
        mc3_difftest_capture("before", func, *calls);
        lifted();
        mc3_difftest_capture("after", func, *calls);
    }
    if (ndone == 16 || time(NULL) - armed_at >= 3) {
        fprintf(stderr, "[DIFFTEST] %d function(s) captured; ending the run\n", ndone);
        fflush(stderr);
        exit(0);
    }
    return 1;
#endif
}

void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls)
{
    long want;

    if (key_mode()) {
        key_call(func, lifted, calls);
        return;
    }
    want = mc3_difftest_wanted();
    within_init();
    if (s_within_func && func == s_within_func) {
        (*calls)++;
        if (*calls == s_within_call) {
            s_within_active = 1;
            lifted();
            s_within_active = 0;
            return;
        }
        lifted();
        return;
    }
    if (s_within_func && !s_within_active) {
        lifted();
        return;
    }
    (*calls)++;
    if (want && *calls == want && difftest_func() == func) {
        mc3_difftest_capture("before", func, *calls);
        lifted();
        mc3_difftest_capture("after", func, *calls);
        fprintf(stderr, "[DIFFTEST] captured; ending the run\n");
        fflush(stderr);
        exit(0);
    }
    lifted();
}

/* Which call to capture: MC3_DIFFTEST_CALL (1-based), 0 = never. */
long mc3_difftest_wanted(void)
{
    static long want = -1;
    if (want < 0) {
        const char *e = getenv("MC3_DIFFTEST_CALL");
        want = e && strcmp(e, "key") != 0 ? atol(e) : 0;
    }
    return want;
}

void mc3_difftest_capture(const char *phase, uint32_t func, long call_index)
{
    const char *dir = getenv("MC3_DIFFTEST_DIR");
    char path[512];
    FILE *f;
    uint32_t ram = (uint32_t)xbox_GetMappedSize();
    int i;

    if (!dir || !*dir)
        dir = "difftest";
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/%08X_%ld_%s.bin", dir, func, call_index, phase);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[DIFFTEST] cannot write %s\n", path);
        return;
    }
    fwrite("MC3DIFF1", 1, 8, f);
    put32(f, 1);
    put32(f, func);
    put32(f, (uint32_t)call_index);
    put32(f, g_eax); put32(f, g_ecx); put32(f, g_edx); put32(f, g_ebx);
    put32(f, g_esp); put32(f, g_ebp); put32(f, g_esi); put32(f, g_edi);
    put32(f, (uint32_t)g_fp_top);
    put32(f, (uint32_t)g_fp_control_word);
    fwrite(g_fp_stack, sizeof(double), 8, f);
    {
        uint64_t mm[8] = { g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7 };
        fwrite(mm, 8, 8, f);
    }
    {
        RecompXmm x[8] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 };
        for (i = 0; i < 8; i++) fwrite(&x[i], 16, 1, f);
    }
    put32(f, 2);
    if (write_region(f, 0, ram) || write_region(f, CONTIG_VA, CONTIG_SIZE))
        fprintf(stderr, "[DIFFTEST] short write in %s\n", path);
    fclose(f);
    fprintf(stderr, "[DIFFTEST] %s: sub_%08X call #%ld, esp=%08X -> %s\n",
            phase, func, call_index, g_esp, path);
}
