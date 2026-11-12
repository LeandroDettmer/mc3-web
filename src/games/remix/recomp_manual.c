/* Ported by tools/port_overrides.py from src/games/dub/recomp_manual.c: every guest address
 * rewritten for this build. Places marked PORT? could not be mapped;
 * each needs a look before its override is trusted. */

/**
 * Midnight Club 3: DUB Edition -- hand-written overrides of lifted functions
 * and ICALL diagnostics. Started from xboxrecomp's templates/new-game (MIT).
 *
 * tools/regen.sh passes this file to the recompiler (--exclude-manual): any
 * sub_XXXXXXXX defined here is not generated, so the hand-written body links
 * instead. After editing, LIFT_ONLY=1 tools/regen.sh.
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* The generated register model (gen/recomp_types.h): the guest registers,
 * MEM32 and friends, and the ICALL trace ring the runtime owns. Include it
 * rather than re-declaring registers by hand: they are thread-local
 * (RECOMP_TLS), and a plain `extern uint32_t g_eax` is a TLS mismatch that
 * GCC refuses to link on Linux and the Switch. */
#include "recomp_types.h"

/* End of the contiguous window (MmAllocateContiguousMemory, the game's heap):
 * 0x84000000, or 0x88000000 with RECOMP_RAM_MB=128 (toolkit kernel.h). */
extern uint32_t g_xbox_contig_size;
#define MC3_CONTIG_END (0x80000000u + g_xbox_contig_size)

/* ── The game's main heap ──────────────────────────────────────
 *
 * Boot (sub_00012440 @0x12503) makes it with sub_002698C0(name 0x447E24,
 * 0x2F = 47 MB, 0, 0, 0), cdecl: sub_003670F0 shifts the size left by 20 and
 * sub_00367310 takes it from MmAllocateContiguousMemoryEx; the heap object
 * becomes the current heap [0x52B488] (+0xC size, +0x10 bump top). Its
 * sub-heaps (LoadHeap, VehHeap, the network heap) are carved out of it, so a
 * larger one only adds headroom. Free roam's remote cars (~0.8-0.9 MB each,
 * docs/roam-many-cars.md) live in a heap of their own with a 128 MB window
 * (roam_cheap, roam_game.c: MC3_ROAM_HEAP_MB, 48 MB), else in this one.
 *
 * MC3_HEAP_MB=<n> sets it; unset, it is 47 with a 64 MB window and 63 with
 * RECOMP_RAM_MB=128 (95 with MC3_ROAM_HEAP_MB=0: no cars heap). At most the
 * window less 17 MB (the image and what D3D, DSOUND and the rest take
 * outside the heap: 64 - 47 at retail), so 47 / 111: 63 + the cars heap's 48.
 * Logs '[HEAP] main heap N KB'. */
extern void sub_002698C0_gen(void);
void sub_002698C0(void)
{
    uint32_t window_mb = g_xbox_contig_size >> 20, max_mb = window_mb - 17u;
    uint32_t game_mb = MEM32(g_esp + 8), mb = game_mb, heap;
    const char *env = getenv("MC3_HEAP_MB"), *cars = getenv("MC3_ROAM_HEAP_MB");

    if (env && *env)
        mb = (uint32_t)strtoul(env, NULL, 10);
    else if (window_mb >= 128u)
        mb = game_mb + (cars && *cars && !strtoul(cars, NULL, 10) ? 48u : 16u);
    if (mb < 16u)
        mb = 16u;
    if (mb > max_mb) {
        fprintf(stderr, "[HEAP] main heap %u MB does not fit a %u MB window: %u MB\n",
                mb, window_mb, max_mb);
        mb = max_mb;
    }
    MEM32(g_esp + 8) = mb;
    sub_002698C0_gen();
    heap = MEM32(0x0052B488u);
    /* heap +8 base, +0xC size (sub_001B3DF0) */
    fprintf(stderr, "[HEAP] main heap %u KB at 0x%08X (heap 0x%08X; game asked %u MB; window %u MB)\n",
            heap ? MEM32(heap + 0xCu) >> 10 : 0u, heap ? MEM32(heap + 8u) : 0u, heap,
            game_mb, window_mb);
}

/* ── Native replacements ───────────────────────────────────── */

/* memmove (CRT), 0x002DB4C0 -- func_id: memmove, confidence 0.9.
 *
 * The lifted body dispatches its head and tail copies through two jump tables
 * (0x002AB670 (PORT?), 0x002AB76C (PORT?)) whose targets are labels inside the function. The
 * vendored lifter emits those as indirect calls to other functions, which
 * resolve to nothing ([ICALL] Failed to resolve VA 0x002AB8B4 (PORT?), first run): the
 * copy silently stops short. Upstream fixed this class of jump table after our
 * base; until it is merged, the host's memmove does the job (faster, too).
 *
 * cdecl: dest, src, count on the guest stack above the return address;
 * returns dest in eax; the caller pops the arguments, we pop the return
 * address as `ret` would. */
void sub_002DB4C0(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t len = MEM32(g_esp + 12);

    if (len)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), len);
    g_eax = dst;
    g_esp += 4;
}

/* Bink's MMX detection, 0x002E9AC0 (called by the blit set-up sub_003B3D80).
 *
 * The classic test: flip EFLAGS.ID with pushfd/popfd to see whether CPUID
 * exists, then CPUID leaf 1, EDX bit 23 = MMX. The vendored lifter emits
 * pushfd, popfd and cpuid as silent no-ops, so the ID bit never "sticks",
 * Bink decides the CPU has no MMX, never installs its pixel routines
 * (0x55FD70.. stay 0), and the blit loop -- whose stdcall calls through those
 * null pointers are skipped without popping their argument -- drifts its
 * stack and never ends (2026-10-14 runs, [BINKLOOP] trace).
 *
 * The Xbox CPU is a Pentium III: CPUID and MMX are always there. Same
 * results and side effects as the original:
 *   [0x4BA740] = cached "has MMX" (-1 until detected)
 *   [0x5697DC] = "use MMX", unless the caller's flags carry 0x40000
 * cdecl: flags at [esp+4]; returns the detection result in eax. */
void sub_002E9AC0(void)
{
    uint32_t flags = MEM32(g_esp + 4);
    uint32_t has_mmx = MEM32(0x4BA740);

    if ((int32_t)has_mmx < 0) {
        has_mmx = 1;
        MEM32(0x4BA740) = has_mmx;
    }
    MEM32(0x5697DC) = (has_mmx && !(flags & 0x40000u)) ? 1u : 0u;
    g_eax = has_mmx;
    g_esp += 4;
}

/* D3D fence wait, 0x004165A0 (D3D section): waits until the GPU-written
 * semaphore at *(device + 0x30) reaches a fence. The NV2A executor writes
 * BACK_END_WRITE_SEMAPHORE_RELEASE values there only once the project names
 * the address (nv2a_pb_set_semaphore_target, xboxrecomp nv2a_backend.h);
 * until then every release is dropped and the wait never ends (2026-10-14
 * run 15: main thread here with PUT == GET). MC3's D3D keeps its device
 * pointer at 0x00422D78 (mov esi, [0x422D78] at the top of this function).
 * Name the semaphore on the first wait, then run the original: its kick-off
 * queues new releases, which now land. */
/*
 * The wait's last loop (0x004166D7) reads the time handed out once
 * (+0x2C, into a register) and spins while
 *     handed - target < handed - gpu      (unsigned)
 * i.e. while the GPU is behind the fence inside the window [.., handed].
 * Its poll-loop yield lets the other guest threads run -- MC3's loading
 * screen keeps drawing from its own thread -- and when they push and the
 * GPU runs past `handed` before this thread looks again, handed - gpu
 * wraps to ~4 G and the loop never ends: the career loading-screen hang
 * (2026-10-21; snapshot: fence 000F6AEF, GPU at 000F90B5, main thread
 * spinning there with the loading screen still animating). On the Xbox the
 * window is microseconds and nothing pushes in it. So while the original
 * runs, a spin hook checks at each yield: once the GPU has passed the fence
 * it stores the fence itself in the semaphore, which ends the loop on its
 * next read, and puts the GPU's value back when the wait returns (unless
 * the GPU has written a newer one). Lower is the safe direction: a reader
 * in between waits, never runs ahead. MC3_FENCE_FIX=0 turns it off.
 */
static uint32_t s_fence_sem_va;
static RECOMP_TLS uint32_t t_fence_target, t_fence_saved;
static RECOMP_TLS int t_fence_fixed;
extern void recomp_set_spin_hook(void (*fn)(void));

/* Compare-and-swap on guest memory: the executor writes it from its thread. */
static uint32_t fence_cas(uint32_t va, uint32_t expect, uint32_t want)
{
    uint32_t *p = (uint32_t *)XBOX_PTR(va);
    __atomic_compare_exchange_n(p, &expect, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expect;                            /* the value found */
}

static void fence_spin_hook(void)
{
    uint32_t gpu;
    if (!s_fence_sem_va)
        return;
    if (t_fence_fixed) {
        /* Still spinning after the fix: some other loop -- give it the
         * real value back. */
        fence_cas(s_fence_sem_va, t_fence_target, t_fence_saved);
        t_fence_fixed = 0;
        return;
    }
    gpu = __atomic_load_n((uint32_t *)XBOX_PTR(s_fence_sem_va), __ATOMIC_SEQ_CST);
    if (gpu == t_fence_target || (int32_t)(gpu - t_fence_target) < 0)
        return;                                /* not there yet, or exact */
    if (fence_cas(s_fence_sem_va, gpu, t_fence_target) == gpu) {
        static int logged;
        t_fence_saved = gpu;
        t_fence_fixed = 1;
        if (logged++ < 5)
            fprintf(stderr, "[D3D] fence wait: GPU at %08X ran past fence %08X and the "
                            "window -- released\n", gpu, t_fence_target);
    }
}

extern void sub_004165A0_gen(void);
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
void sub_004165A0(void)
{
    static uint32_t sem_va;
    static int fix = -1;
    uint32_t save_target = t_fence_target, save_saved = t_fence_saved;
    int save_fixed = t_fence_fixed;

    if (!sem_va) {
        uint32_t device = MEM32(0x00422D78u);
        uint32_t sem = device ? MEM32(device + 0x30u) : 0;
        if (sem) {
            sem_va = sem;
            nv2a_pb_set_semaphore_target(sem_va);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (device 0x%08X)\n",
                    sem_va, device);
            s_fence_sem_va = sem_va;
        }
    }
    if (fix < 0) {
        const char *e = getenv("MC3_FENCE_FIX");
        fix = !(e && e[0] == '0');
    }
    if (!fix || !s_fence_sem_va) {
        sub_004165A0_gen();
        return;
    }
    t_fence_target = MEM32(g_esp + 4);         /* (time, flags), stdcall */
    t_fence_fixed = 0;
    recomp_set_spin_hook(fence_spin_hook);
    sub_004165A0_gen();
    if (t_fence_fixed)
        fence_cas(s_fence_sem_va, t_fence_target, t_fence_saved);
    t_fence_target = save_target;
    t_fence_saved = save_saved;
    t_fence_fixed = save_fixed;
    recomp_set_spin_hook(save_target ? fence_spin_hook : NULL);
}

/* ── Differential tests (src/difftest.c, tools/difftest.py) ── */

/* Wrapped so one call can be captured and replayed on real x86 (make difftest
 * FUNC=<addr> CALL=<n>). Unarmed, a wrapper costs one counter and a compare.
 * Movie frames and movie audio decode wrongly (2026-10-15); BinkDoFrame
 * (0x003AD2C0) itself waits on Bink's I/O thread, so capture below it:
 *   0x003B2F40  Bink video: decodes the frame's planes (4 x sub_003B1180,
 *               block decoders 0x003AFB30.., transforms 0x003B7060..);
 *               called from BinkDoFrame at 0x003AD82A
 *   0x003AEDB0  Bink audio: decodes the frame's audio (sub_003AE8C0 and the
 *               .text maths at 0x002E9930 / 0x002E97F0); at 0x003AD5AD */
extern void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls);
extern long mc3_difftest_wanted(void);

/* ── Music: the WMA decoder XACT drives (MC3_WMA_TRACE=1) ──
 *
 * The music (audio/XStreams/mx.xwb, 110 tracks) is WMA, stereo 44.1 kHz;
 * XACT decodes it on the CPU through the WMADEC section's media object and
 * plays the PCM from a looping buffer voice. On 2026-10-16 that voice ran
 * but every sample it read was zero, so the menu song was silent. This logs
 * what the decoder is asked and answers: XACT creates it with
 * sub_004260AD (0x0040CAE8, when the wave's format tag is 2 = WMA); its
 * vtable is at 0x004A5594 (constructor sub_00426003). Per method: the first
 * calls in full, then every 500th, with stdcall arguments (this, a, b), the
 * HRESULT, and for any argument that looks like an XMEDIAPACKET
 * {pvBuffer, dwMaxSize, pdwCompletedSize, ...} the completed size and the
 * peak 16-bit sample in it. Each is difftest-capable: Process is slot 4,
 * `make difftest FUNC=003EFCE7 CALL=<n>`. */
static int wma_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_WMA_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

/* An XMEDIAPACKET's completed size and 16-bit peak, or 0 if `va` is not one. */
static int wma_packet(uint32_t va, uint32_t *done, uint32_t *max, int *peak)
{
    uint32_t buf, size, done_va, n, i;
    if (va < 0x10000u || va >= MC3_CONTIG_END)
        return 0;
    buf = MEM32(va); size = MEM32(va + 4); done_va = MEM32(va + 8);
    if (buf < 0x10000u || buf >= MC3_CONTIG_END || !size || size > 0x400000u)
        return 0;
    n = done_va && done_va < MC3_CONTIG_END ? MEM32(done_va) : size;
    if (n > size) n = size;
    *done = n; *max = size; *peak = 0;
    for (i = 0; i + 1 < n; i += 2) {
        int v = (int16_t)MEM16(buf + i);
        if (v < 0) v = -v;
        if (v > *peak) *peak = v;
    }
    return 1;
}

static void wma_call(uint32_t addr, int method, void (*fn)(void))
{
    static long calls[11], dcalls[11];
    static int after_create;                /* method calls still to log after a create */
    uint32_t a0, a1, a2;
    long n;
    int follow = 0;
    /* Creation is rare (one per music track) and the question is often
     * whether it happens at all: always logged, with or without the trace.
     * So are the first 3 method calls after each, entered and returned
     * (2026-11-11: the game froze right after a track's create #3; an
     * "entered" line with no return names a hang inside the decoder). */
#ifdef __EMSCRIPTEN__
    enum { web = 1 };
#else
    enum { web = 0 };                       /* native: first 4 creates, no follow-up */
#endif
    if (web && method < 10 && after_create > 0) {
        after_create--;
        follow = 1;
    }
    if (!wma_trace_on() && (method != 10 || (!web && calls[10] >= 4)) && !follow) {
        mc3_difftest_call(addr, fn, &dcalls[method]);
        return;
    }
    a0 = MEM32(g_esp + 4); a1 = MEM32(g_esp + 8); a2 = MEM32(g_esp + 12);
    n = ++calls[method];
    if (follow)
        fprintf(stderr, "[WMA] method #%ld [vtable +0x%02X] entered (%08X, %08X, %08X)\n",
                n, method * 4, a0, a1, a2);
    mc3_difftest_call(addr, fn, &dcalls[method]);
    if (web && method == 10)
        after_create = 3;
    if (n <= 12 || n % 500 == 0 || (web && method == 10) || follow) {
        uint32_t d, m;
        int pk;
        fprintf(stderr, "[WMA] %s #%ld (%08X, %08X, %08X) -> %08X",
                method == 10 ? "create" : "method", n, a0, a1, a2, g_eax);
        if (method < 10)
            fprintf(stderr, " [vtable +0x%02X]", method * 4);
        if (wma_packet(a1, &d, &m, &pk))
            fprintf(stderr, " | a: %u/%u bytes peak %d", d, m, pk);
        if (wma_packet(a2, &d, &m, &pk))
            fprintf(stderr, " | b: %u/%u bytes peak %d", d, m, pk);
        fprintf(stderr, "\n");
    }
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
extern void sub_004260AD_gen(void);
void sub_004260AD(void) { wma_call(0x004260ADu, 10, sub_004260AD_gen); }   /* create: (out object, ...) */
extern void sub_00425C5F_gen(void);
void sub_00425C5F(void) { wma_call(0x00425C5Fu, 2, sub_00425C5F_gen); }
extern void sub_00425CA1_gen(void);
void sub_00425CA1(void) { wma_call(0x00425CA1u, 3, sub_00425CA1_gen); }
extern void sub_00425F87_gen(void);
void sub_00425F87(void) { wma_call(0x00425F87u, 4, sub_00425F87_gen); }
extern void sub_00425C9C_gen(void);
void sub_00425C9C(void) { wma_call(0x00425C9Cu, 5, sub_00425C9C_gen); }
extern void sub_00425F23_gen(void);
void sub_00425F23(void) { wma_call(0x00425F23u, 6, sub_00425F23_gen); }
extern void sub_00425D5A_gen(void);
void sub_00425D5A(void) { wma_call(0x00425D5Au, 7, sub_00425D5A_gen); }
extern void sub_00425DC0_gen(void);
void sub_00425DC0(void) { wma_call(0x00425DC0u, 8, sub_00425DC0_gen); }
extern void sub_00425B8F_gen(void);
void sub_00425B8F(void) { wma_call(0x00425B8Fu, 9, sub_00425B8F_gen); }

/* ── Music: XACT's API as the game calls it (MC3_XACT_TRACE=1) ──
 *
 * With MC3_WMA_TRACE on, XACT never created a WMA decoder (2026-10-16):
 * the songs fail before decoding, and the in-game music player shuffles
 * through them forever. These are the 37 XACT functions the game calls
 * directly (0x004073D9..0x00407A14; the last ten are jmp thunks). Each
 * logs its first 3 calls, and every call returning a failure HRESULT (bit
 * 31 set; a few may return counts or pointers instead), with ecx and four
 * stack arguments. */
extern void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls);

static int xact_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_XACT_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void xact_call(uint32_t addr, int slot, void (*fn)(void))
{
    static long calls[64], failures, dcalls[64];
    uint32_t c, a0, a1, a2, a3;
    long n;
    /* Every XACT wrapper can also be captured: make difftest FUNC=<addr>. */
    if (!xact_trace_on()) { mc3_difftest_call(addr, fn, &dcalls[slot]); return; }
    c = g_ecx;
    a0 = MEM32(g_esp + 4); a1 = MEM32(g_esp + 8);
    a2 = MEM32(g_esp + 12); a3 = MEM32(g_esp + 16);
    uint32_t desc[4] = { 0, 0, 0, 0 };
    n = ++calls[slot];
    /* sub_004078AD / sub_0040EDB3's second argument is a descriptor:
     * flags (bit 2: streamed), then three words (+0xC: the stream's
     * source). Read before the call, while it is the caller's. */
    if ((addr == 0x004078ADu || addr == 0x0040EDB3u) && a1 >= 0x10000u && a1 < MC3_CONTIG_END) {
        desc[0] = MEM32(a1); desc[1] = MEM32(a1 + 4);
        desc[2] = MEM32(a1 + 8); desc[3] = MEM32(a1 + 12);
    }
    mc3_difftest_call(addr, fn, &dcalls[slot]);
    if (n <= 3 || ((g_eax & 0x80000000u) && failures++ < 400)) {
        fprintf(stderr, "[XACT] sub_%08X #%ld (ecx %08X; %08X %08X %08X %08X) -> %08X%s",
                addr, n, c, a0, a1, a2, a3, g_eax,
                (g_eax & 0x80000000u) ? "  FAILED" : "");
        if (desc[0] | desc[1] | desc[2] | desc[3])
            fprintf(stderr, " desc {%08X %08X %08X %08X}", desc[0], desc[1], desc[2], desc[3]);
        fprintf(stderr, "\n");
    }
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
/* XACT's variation choice, 0x0040DBEF: thiscall (table offset, flags,
 * float), returns the chosen entry or 0. sub_0040E0F6 turns 0 into wave
 * index 0xFFFF and sub_0040EC76 into E_FAIL (0x0040ED25) -- every song in
 * the music player. Pure computation (mode (flags >> 13) & 15, an RNG at
 * sub_00408459 / sub_00408462): wrapped for difftest, and logged with
 * MC3_XACT_TRACE. The first call is at boot (the menu song). */
extern void sub_0040DBEF_gen(void);
void sub_0040DBEF(void)
{
    static long calls, logged;
    uint32_t c = g_ecx, a0 = MEM32(g_esp + 4), a1 = MEM32(g_esp + 8), a2 = MEM32(g_esp + 12);
    uint32_t entry = 0;
    if (c >= 0x10000u && c < MC3_CONTIG_END)
        entry = MEM32(MEM32(c + 4) + a0);
    mc3_difftest_call(0x0040DBEFu, sub_0040DBEF_gen, &calls);
    if (xact_trace_on() && logged++ < 40)
        fprintf(stderr, "[XACT] variation sub_0040DBEF #%ld (ecx %08X; %08X %08X %08X)"
                " table word %08X (mode %u, count %u) -> %08X\n", logged, c, a0, a1, a2,
                entry, (entry >> 13) & 15, entry & 0x1FFF, g_eax);
}

/* XACT sound states (2026-10-17). Starting a sound moves it to state 5
 * (sub_0040D9D0 -> sub_0040B71F(5)); the only refused transition is from
 * state 7, "stopped", and that refusal is the music player's E_FAIL: every
 * song is stopped before it starts. sub_0040BE8F is XACT's stop (state 7 +
 * cleanup), called from nine places. With MC3_XACT_TRACE these log who stops
 * a sound (the return address names the caller) and every move to state 7 or
 * refused transition, with the sound object and its state before. */
extern void sub_0040BE8F_gen(void);
void sub_0040BE8F(void)
{
    static long n;
    uint32_t snd = g_ecx, ret = MEM32(g_esp), flags = MEM32(g_esp + 4);
    uint32_t state = snd >= 0x10000u && snd < MC3_CONTIG_END ? MEM16(snd + 0x30) : 0xFFFF;
    if (xact_trace_on() && n++ < 200)
        fprintf(stderr, "[XACT] stop sound %08X (state %u, flags %08X) called from %08X\n",
                snd, state, flags, ret);
    sub_0040BE8F_gen();
}

extern void sub_0040B71F_gen(void);
void sub_0040B71F(void)
{
    static long n;
    uint32_t snd = g_ecx, ret = MEM32(g_esp), want = MEM32(g_esp + 4);
    uint32_t before = snd >= 0x10000u && snd < MC3_CONTIG_END ? MEM16(snd + 0x30) : 0xFFFF;
    sub_0040B71F_gen();
    if (xact_trace_on() && (want == 7 || !g_eax) && n++ < 300)
        fprintf(stderr, "[XACT] sound %08X state %u -> %u %s (from %08X)\n",
                snd, before, want, g_eax ? "ok" : "REFUSED", ret);
}

/* One level down, inside sub_004078AD -> sub_0040EDB3 (2026-10-16: E_FAIL
 * 403x while the music player shuffled): which step fails. */
extern void sub_0040EDB3_gen(void);
void sub_0040EDB3(void) { xact_call(0x0040EDB3u, 40, sub_0040EDB3_gen); }
extern void sub_00407084_gen(void);
void sub_00407084(void) { xact_call(0x00407084u, 41, sub_00407084_gen); }   /* streamed (flag 4) */
extern void sub_0040EC76_gen(void);
void sub_0040EC76(void) { xact_call(0x0040EC76u, 42, sub_0040EC76_gen); }
extern void sub_0040DEB0_gen(void);
void sub_0040DEB0(void) { xact_call(0x0040DEB0u, 43, sub_0040DEB0_gen); }
extern void sub_00410DDB_gen(void);
void sub_00410DDB(void) { xact_call(0x00410DDBu, 44, sub_00410DDB_gen); }
extern void sub_004073D9_gen(void);
void sub_004073D9(void) { xact_call(0x004073D9u, 0, sub_004073D9_gen); }
extern void sub_0040740B_gen(void);
void sub_0040740B(void) { xact_call(0x0040740Bu, 1, sub_0040740B_gen); }
extern void sub_004074B0_gen(void);
void sub_004074B0(void) { xact_call(0x004074B0u, 2, sub_004074B0_gen); }
extern void sub_004074C8_gen(void);
void sub_004074C8(void) { xact_call(0x004074C8u, 3, sub_004074C8_gen); }
extern void sub_0040750E_gen(void);
void sub_0040750E(void) { xact_call(0x0040750Eu, 4, sub_0040750E_gen); }
extern void sub_0040754D_gen(void);
void sub_0040754D(void) { xact_call(0x0040754Du, 5, sub_0040754D_gen); }
extern void sub_00407590_gen(void);
void sub_00407590(void) { xact_call(0x00407590u, 6, sub_00407590_gen); }
extern void sub_004075D3_gen(void);
void sub_004075D3(void) { xact_call(0x004075D3u, 7, sub_004075D3_gen); }
extern void sub_00407612_gen(void);
void sub_00407612(void) { xact_call(0x00407612u, 8, sub_00407612_gen); }
extern void sub_0040764D_gen(void);
void sub_0040764D(void) { xact_call(0x0040764Du, 9, sub_0040764D_gen); }
extern void sub_0040768C_gen(void);
void sub_0040768C(void) { xact_call(0x0040768Cu, 10, sub_0040768C_gen); }
extern void sub_004076AC_gen(void);
void sub_004076AC(void) { xact_call(0x004076ACu, 11, sub_004076AC_gen); }
extern void sub_004076EB_gen(void);
void sub_004076EB(void) { xact_call(0x004076EBu, 12, sub_004076EB_gen); }
extern void sub_00407726_gen(void);
void sub_00407726(void) { xact_call(0x00407726u, 13, sub_00407726_gen); }
extern void sub_00407761_gen(void);
void sub_00407761(void) { xact_call(0x00407761u, 14, sub_00407761_gen); }
extern void sub_004077A0_gen(void);
void sub_004077A0(void) { xact_call(0x004077A0u, 15, sub_004077A0_gen); }
extern void sub_004077E3_gen(void);
void sub_004077E3(void) { xact_call(0x004077E3u, 16, sub_004077E3_gen); }
extern void sub_0040781B_gen(void);
void sub_0040781B(void) { xact_call(0x0040781Bu, 17, sub_0040781B_gen); }
extern void sub_00407833_gen(void);
void sub_00407833(void) { xact_call(0x00407833u, 18, sub_00407833_gen); }
extern void sub_0040784F_gen(void);
void sub_0040784F(void) { xact_call(0x0040784Fu, 19, sub_0040784F_gen); }
extern void sub_0040787A_gen(void);
void sub_0040787A(void) { xact_call(0x0040787Au, 20, sub_0040787A_gen); }
extern void sub_004078AD_gen(void);
void sub_004078AD(void) { xact_call(0x004078ADu, 21, sub_004078AD_gen); }
extern void sub_004078E0_gen(void);
void sub_004078E0(void) { xact_call(0x004078E0u, 22, sub_004078E0_gen); }
extern void sub_00407917_gen(void);
void sub_00407917(void) { xact_call(0x00407917u, 23, sub_00407917_gen); }
extern void sub_0040794E_gen(void);
void sub_0040794E(void) { xact_call(0x0040794Eu, 24, sub_0040794E_gen); }
extern void sub_00407981_gen(void);
void sub_00407981(void) { xact_call(0x00407981u, 25, sub_00407981_gen); }
extern void sub_004079B4_gen(void);
void sub_004079B4(void) { xact_call(0x004079B4u, 26, sub_004079B4_gen); }
extern void sub_004079E7_gen(void);
void sub_004079E7(void) { xact_call(0x004079E7u, 27, sub_004079E7_gen); }
extern void sub_004079EC_gen(void);
void sub_004079EC(void) { xact_call(0x004079ECu, 28, sub_004079EC_gen); }
extern void sub_004079F1_gen(void);
void sub_004079F1(void) { xact_call(0x004079F1u, 29, sub_004079F1_gen); }
extern void sub_004079F6_gen(void);
void sub_004079F6(void) { xact_call(0x004079F6u, 30, sub_004079F6_gen); }
extern void sub_004079FB_gen(void);
void sub_004079FB(void) { xact_call(0x004079FBu, 31, sub_004079FB_gen); }
extern void sub_00407A00_gen(void);
void sub_00407A00(void) { xact_call(0x00407A00u, 32, sub_00407A00_gen); }
extern void sub_00407A05_gen(void);
void sub_00407A05(void) { xact_call(0x00407A05u, 33, sub_00407A05_gen); }
extern void sub_00407A0A_gen(void);
void sub_00407A0A(void) { xact_call(0x00407A0Au, 34, sub_00407A0A_gen); }
extern void sub_00407A0F_gen(void);
void sub_00407A0F(void) { xact_call(0x00407A0Fu, 35, sub_00407A0F_gen); }
extern void sub_00407A14_gen(void);
void sub_00407A14(void) { xact_call(0x00407A14u, 36, sub_00407A14_gen); }

/* ── Movies: where a frame's time goes (MC3_BINK_TRACE=1) ──
 *
 * The movie loop (sub_003638A0) per frame: BinkDoFrame, lock the texture,
 * BinkCopyToBuffer, BinkNextFrame, then spin on BinkWait until the next frame
 * is due; its caller draws and presents. Once a second this prints the
 * average milliseconds per frame in each phase -- "rest" is everything from
 * the end of the wait to the next BinkDoFrame (draw, Present, game). */

enum { BT_DO, BT_VIDEO, BT_AUDIO, BT_LOCK, BT_COPY, BT_NEXT, BT_WAIT, BT_REST, BT_N };
static const char *const s_bt_name[BT_N] =
    { "doframe", "video", "audio", "lock", "copy", "next", "wait", "rest" };
static double s_bt_sum[BT_N], s_bt_mark[BT_N], s_bt_report;
static long s_bt_frames, s_bt_polls;
static double s_bt_wait_start;       /* 0 when not in the wait loop */

static double bt_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int bt_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_BINK_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void bt_report(double now)
{
    char line[256];
    int n = 0, i;
    if (!s_bt_report) { s_bt_report = now; return; }
    if (now - s_bt_report < 1000.0 || !s_bt_frames) return;
    n += snprintf(line + n, sizeof line - n, "[BINKT] %.1f fps, ms/frame:",
                  s_bt_frames * 1000.0 / (now - s_bt_report));
    for (i = 0; i < BT_N; i++)
        n += snprintf(line + n, sizeof line - n, " %s %.1f", s_bt_name[i], s_bt_sum[i] / s_bt_frames);
    snprintf(line + n, sizeof line - n, " (%ld polls/frame)", s_bt_polls / s_bt_frames);
    fprintf(stderr, "%s\n", line);
    memset(s_bt_sum, 0, sizeof s_bt_sum);
    s_bt_frames = s_bt_polls = 0;
    s_bt_report = now;
}

/* Time one lifted call into phase `p`. */
static void bt_time(int p, void (*fn)(void))
{
    double t0;
    if (!bt_on()) { fn(); return; }
    t0 = bt_now();
    fn();
    s_bt_sum[p] += bt_now() - t0;
}

/* BinkDoFrame, 0x003AD2C0: stdcall (hbink). Opens a frame. */

/* Remix bring-up: does a stdcall wrapper hand back the stack it was given?
 * (MC3_ABI_CHECK=1; the movie function's pops came back 0, 2026-10-24) */
static void esp_check(const char *name, uint32_t esp0, uint32_t want)
{
    static int on = -1, said;
    if (on < 0) {
        const char *e = getenv("MC3_ABI_CHECK");
        on = e && e[0] == '1';
    }
    if (on && g_esp - esp0 != want && said++ < 20)
        fprintf(stderr, "[ABI] %s returned esp %+d, want %+u\n", name, (int)(g_esp - esp0), want);
}
/* MC3_SKIP_MOVIES=hard (see movie_skipped below): the movie being skipped.
 * Its BinkDoFrame, BinkCopyToBuffer, BinkNextFrame and BinkWait return at
 * once (nothing decoded, the game's cleared texture stays black) with the
 * frame number at the last frame, so the movie loop (sub_003638A0) reports
 * the end (3) and the game closes it and goes on as after a whole movie. */
static uint32_t s_hard_hbink;
static int hard_skip(unsigned args)
{
    uint32_t h = MEM32(g_esp + 4);
    if (!s_hard_hbink || h != s_hard_hbink)
        return 0;
    MEM32(h + 0x0C) = MEM32(h + 0x08);          /* FrameNum = Frames */
    g_eax = 0;
    g_esp += 4 + 4 * args;                      /* stdcall: return address + arguments */
    return 1;
}

extern void sub_003AD2C0_gen(void);
void sub_003AD2C0(void)
{
    double t0;
    uint32_t e0 = g_esp;
    if (hard_skip(1)) return;
    if (!bt_on()) { sub_003AD2C0_gen(); esp_check("BinkDoFrame", e0, 8); return; }
    t0 = bt_now();
    if (s_bt_mark[BT_REST]) s_bt_sum[BT_REST] += t0 - s_bt_mark[BT_REST];
    bt_report(t0);
    s_bt_frames++;
    sub_003AD2C0_gen();
    s_bt_mark[BT_LOCK] = bt_now();
    s_bt_sum[BT_DO] += s_bt_mark[BT_LOCK] - t0;
}

/* BinkCopyToBuffer, 0x003AD280: the YUV -> RGB copy into the locked texture.
 * stdcall (hbink, dest, pitch, height, x, y, flags), returns 0 when copied.
 * With the renderer upscaling the frame (bink_native.c mc3_bink_native_copy)
 * the copy is skipped: nothing but the drawn texture reads it. */
extern int mc3_bink_native_copy(uint32_t hbink, uint32_t dest_va, const uint8_t *dest,
                                uint32_t pitch, uint32_t height, uint32_t x, uint32_t y, int after);
extern void sub_003AD280_gen(void);
void sub_003AD280(void)
{
    uint32_t hbink = MEM32(g_esp + 4), dest = MEM32(g_esp + 8), pitch = MEM32(g_esp + 12);
    uint32_t height = MEM32(g_esp + 16), x = MEM32(g_esp + 20), y = MEM32(g_esp + 24);
    if (hard_skip(7)) return;
    if (bt_on() && s_bt_mark[BT_LOCK]) {
        s_bt_sum[BT_LOCK] += bt_now() - s_bt_mark[BT_LOCK];
        s_bt_mark[BT_LOCK] = 0;
    }
    if (dest && mc3_bink_native_copy(hbink, dest, (const uint8_t *)XBOX_PTR(dest),
                                     pitch, height, x, y, 0)) {
        g_eax = 0;
        g_esp += 4 + 28;                     /* return address + 7 arguments */
        return;
    }
    {
        uint32_t e0 = g_esp;
        bt_time(BT_COPY, sub_003AD280_gen);
        esp_check("BinkCopyToBuffer", e0, 32);
    }
    if (dest)
        mc3_bink_native_copy(hbink, dest, (const uint8_t *)XBOX_PTR(dest), pitch, height, x, y, 1);
}

/* BinkNextFrame, 0x003AD8D0. */
extern void sub_003AD8D0_gen(void);
void sub_003AD8D0(void)
{
    uint32_t e0 = g_esp;
    if (hard_skip(1)) return;
    bt_time(BT_NEXT, sub_003AD8D0_gen);
    esp_check("BinkNextFrame", e0, 8);
}

/* BinkWait, 0x003ACDF0: stdcall (hbink), nonzero = too early, call again. */
extern void sub_003ACDF0_gen(void);
void sub_003ACDF0(void)
{
    double now;
    uint32_t e0 = g_esp;
    if (hard_skip(1)) return;
    if (!bt_on()) { sub_003ACDF0_gen(); esp_check("BinkWait", e0, 8); return; }
    now = bt_now();
    if (!s_bt_wait_start) s_bt_wait_start = now;
    s_bt_polls++;
    sub_003ACDF0_gen();
    if (!g_eax) {
        now = bt_now();
        s_bt_sum[BT_WAIT] += now - s_bt_wait_start;
        s_bt_wait_start = 0;
        s_bt_mark[BT_REST] = now;
    }
}

/* ── Movies: Bink video decoded by FFmpeg (src/bink_native.c) ── */

/* Bink's HBINK, as far as the native path needs it. */
#define BINK_WIDTH      0x00u
#define BINK_HEIGHT     0x04u
#define BINK_FRAMENUM   0x0Cu       /* 1-based frame being decoded */
#define BINK_DIRTY_MAP  0x3A0u      /* changed-block map, 1 byte per 16x16 block;
                                     * HBINK+0xDC points here, the decoder's 5th
                                     * argument */

extern int  mc3_bink_native_enabled(void);
extern void mc3_bink_native_open(uint32_t hbink, const char *xbox_name);
extern int  mc3_bink_native_frame(uint32_t hbink, int frame, uint8_t *dest, int w, int h);
extern void mc3_bink_native_close(uint32_t hbink);
extern void mc3_bink_native_audio(uint32_t hbink, int frame);
extern int  mc3_bink_native_audio_active(void);

/* MC3_SKIP_MOVIES: for unattended runs (headless, recordings) where nobody
 * is there to press Enter. Off by default: in a window, press it yourself.
 * A comma list of file names without the folder ("mc3intro.bik"), or 1 =
 * the boot intro (rockstar, sdlogo, mc3intro). While such a movie is open,
 * pad 1 pulses A + Start (3 frames held, 3 released: fresh presses) -- input
 * like a player's, through the title's own handling, so only what the game
 * lets you skip is skipped: the Rockstar and Rockstar San Diego logos play
 * in full (as on an Xbox), mc3intro is cut short.
 * MC3_SKIP_MOVIES=hard (the web page's "Skip intro"): the boot intro is not
 * played at all, logos included -- each movie opens and ends at once
 * (hard_skip above). Returns 1 to press through, 2 to skip hard. */
extern void usb_gamepad_set_held(int dev, uint8_t digital, uint8_t analog);
#define PAD_START 0x10u                         /* digital mask */
#define PAD_A     0x01u                         /* analog mask */
static uint32_t s_skip_hbink;                   /* the movie being pressed through */
static unsigned s_skip_frames;
extern int  mc3_autostart_on(void);             /* src/autostart.c */
extern void mc3_autostart_frame(int in_game, int downs_to_arcade);

static int movie_skipped(const char *path)
{
    const char *list = getenv("MC3_SKIP_MOVIES"), *base = path, *p;
    size_t n;
    int hard = 0;

    if ((!list || !*list) && mc3_autostart_on())
        list = "1";                             /* MC3_AUTOSTART skips the intro too */
    if (!list || !*list || !strcmp(list, "0"))
        return 0;
    if (!strcmp(list, "hard"))
        hard = 1, list = "1";
    if (!strcmp(list, "1"))
        list = "rockstar.bik,sdlogo.bik,mc3intro.bik";
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    n = strlen(base);
    for (p = list; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncasecmp(p, base, n))
            return 1 + hard;
        p += len + (e ? 1 : 0);
    }
    return 0;
}

/* BinkOpen, 0x003AB590: stdcall (name, flags), returns the HBINK. */
extern void sub_003AB590_gen(void);
void sub_003AB590(void)
{
    char name[260] = "";
    uint32_t name_va = MEM32(g_esp + 4);
    int skip;

    if (name_va)
        strncpy(name, (const char *)XBOX_PTR(name_va), sizeof name - 1);
    skip = name[0] ? movie_skipped(name) : 0;
    sub_003AB590_gen();
    if (skip == 2 && g_eax) {
        s_hard_hbink = g_eax;
        fprintf(stderr, "[BINK] skipping %s (MC3_SKIP_MOVIES=hard)\n", name);
        return;
    }
    if (skip) {
        s_skip_hbink = g_eax;
        s_skip_frames = 0;
        fprintf(stderr, "[BINK] pressing A+Start through %s (MC3_SKIP_MOVIES)\n", name);
    }
    if (name[0] && g_eax)
        mc3_bink_native_open(g_eax, name);
}

/* BinkClose, 0x003ADA70: stdcall (hbink). */
extern void sub_003ADA70_gen(void);
void sub_003ADA70(void)
{
    if (s_hard_hbink && MEM32(g_esp + 4) == s_hard_hbink) {
        fprintf(stderr, "[BINK] skipped, no frame played\n");
        s_hard_hbink = 0;
    }
    if (s_skip_hbink && MEM32(g_esp + 4) == s_skip_hbink) {
        usb_gamepad_set_held(0, 0, 0);
        fprintf(stderr, "[BINK] skipped after %u frames\n", s_skip_frames);
        s_skip_hbink = 0;
    }
    mc3_bink_native_close(MEM32(g_esp + 4));
    sub_003ADA70_gen();
}

/* Bink video decode, 0x003B2F40: stdcall, 14 arguments -- (dest frame,
 * previous frame, 0, 0, changed-block map = HBINK+0x3A0, w, h, padded w, padded h,
 * packet, ...),
 * returns 1. Called from BinkDoFrame at 0x003AD82A. The lifted decoder runs
 * when FFmpeg has no frame for this movie, and for difftest captures. */
extern void sub_003B2F40_gen(void);
static void bink_video_decode(void)
{
    static long calls;
    uint32_t map = MEM32(g_esp + 20);
    uint32_t hbink = map - BINK_DIRTY_MAP;

    if (s_skip_hbink && hbink == s_skip_hbink) {
        int down = (s_skip_frames++ % 6) < 3;
        usb_gamepad_set_held(0, down ? PAD_START : 0, down ? PAD_A : 0);
    }

    if (!mc3_difftest_wanted() && mc3_bink_native_enabled()) {
        int w = (int)MEM32(hbink + BINK_WIDTH), h = (int)MEM32(hbink + BINK_HEIGHT);
        int frame = (int)MEM32(hbink + BINK_FRAMENUM) - 1;
        mc3_bink_native_audio(hbink, frame);
        if (mc3_bink_native_frame(hbink, frame, (uint8_t *)XBOX_PTR(MEM32(g_esp + 4)), w, h)) {
            /* Every block changed, so BinkCopyToBuffer copies the whole frame:
             * it counts the map's leading non-zero bytes against the block
             * total. Same size as BinkDoFrame's own clear (0x003AD72F): the
             * 8th and 9th arguments (padded width, height) over 16. */
            uint32_t blocks = (MEM32(g_esp + 32) >> 4) * (MEM32(g_esp + 36) >> 4);
            memset((void *)XBOX_PTR(map), 1, blocks);
            g_eax = 1;
            g_esp += 4 + 0x38;
            return;
        }
    }
    mc3_difftest_call(0x003B2F40u, sub_003B2F40_gen, &calls);
}

void sub_003B2F40(void)
{
    bt_time(BT_VIDEO, bink_video_decode);
}

/* Bink inverse DCT for one 8x8 block, 0x003B5880: (dest, pitch, coeffs) on
 * the stack. In frame 600 its call #59 writes the one wrong block the
 * video difftest found (dest 0x80F3D400, coefficients at 0x00F7FADC). */
extern void sub_003B5880_gen(void);
void sub_003B5880(void)
{
    static long calls;
    mc3_difftest_call(0x003B5880u, sub_003B5880_gen, &calls);
}

extern void sub_003AEDB0_gen(void);
/* Bink audio decode, 0x003AEDB0: stdcall (state, &out_ptr, &out_bytes,
 * packet, &packet_pos), called from BinkDoFrame at 0x003AD5AD, which then
 * copies *out_bytes of PCM from *out_ptr into its DirectSound ring. While
 * FFmpeg plays the movie's sound (src/bink_native.c), that PCM is zeroed:
 * Bink's streaming and clock carry on, on silence. */
static void bink_audio_decode(void)
{
    static long calls;
    uint32_t out_ptr_va = MEM32(g_esp + 8), out_bytes_va = MEM32(g_esp + 12);

    mc3_difftest_call(0x003AEDB0u, sub_003AEDB0_gen, &calls);
    if (mc3_bink_native_audio_active() && !mc3_difftest_wanted()) {
        uint32_t out = MEM32(out_ptr_va), bytes = MEM32(out_bytes_va);
        if (out && bytes && bytes <= 0x100000u /* PORT? */)
            memset((void *)XBOX_PTR(out), 0, bytes);
    }
}

void sub_003AEDB0(void)
{
    bt_time(BT_AUDIO, bink_audio_decode);
}

/* ── Game timing: the clocks MC3 reads (MC3_TIME_TRACE=1) ──
 *
 * 2026-10-17: gameplay ran ~10x fast at a steady 60 fps while music and
 * movies kept time, and every host clock checked out at the source. These
 * wrap the title's own timer entry points and print once a second, per
 * clock, how often it was read and how far it moved (in its own units):
 *   sub_001A1C00  ms since boot: rdtsc / 733333        (1000 per second)
 *   sub_0032633B  QueryPerformanceCounter (rdtsc)      (733.33 M per second)
 *   sub_0032634C  QueryPerformanceFrequency            (733333333)
 *   sub_0020E140  rdtsc, low 32 bits                   (733.33 M per second) */
static int time_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_TIME_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static struct { const char *name; long calls; uint64_t first, last; } s_clk[4] = {
    { "ms clock (0x1A1C00)" }, { "QPC (0x32633B)" }, { "QPF (0x32634C)" }, { "rdtsc lo (0x20E140)" },
};

static void qpc_callers_print(void);

static void time_note(int k, uint64_t v)
{
    static long long next;
    struct timespec ts;
    long long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!s_clk[k].calls++) s_clk[k].first = v;
    s_clk[k].last = v;
    if (!next) next = now + 1000;
    if (now < next) return;
    next = now + 1000;
    for (int i = 0; i < 4; i++) {
        if (!s_clk[i].calls) continue;
        fprintf(stderr, "[TIME] %-20s %6ld reads/s, moved %llu (last %llu)\n", s_clk[i].name,
                s_clk[i].calls, (unsigned long long)(s_clk[i].last - s_clk[i].first),
                (unsigned long long)s_clk[i].last);
        s_clk[i].calls = 0;
    }
    qpc_callers_print();
}

extern void sub_001A1C00_gen(void);
void sub_001A1C00(void)
{
    sub_001A1C00_gen();
    if (time_trace_on()) time_note(0, ((uint64_t)g_edx << 32) | g_eax);
}
/* QPC's callers: the return addresses of the last second, top 4 printed
 * with the [TIME] lines (2026-10-17: 600k reads/s during the intro
 * flythrough, none in the garage -- who polls it). */
typedef struct { uint32_t ret; long n; } CallerCount;
static CallerCount s_qpc_by[16], s_bink_by[16];

static void caller_note(CallerCount *t, uint32_t ret)
{
    int i, low = 0;
    for (i = 0; i < 16; i++) {
        if (t[i].ret == ret) { t[i].n++; return; }
        if (t[i].n < t[low].n) low = i;
    }
    t[low].ret = ret;
    t[low].n = 1;
}
static void qpc_caller(uint32_t ret) { caller_note(s_qpc_by, ret); }

static void callers_print(CallerCount *t, const char *what)
{
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int i = 0; i < 16; i++)
            if (t[i].n > 0 && (best < 0 || t[i].n > t[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, "[TIME]   %s called from %08X: %ld\n", what, t[best].ret, t[best].n);
        t[best].n = 0;                              /* printed */
    }
    memset(t, 0, 16 * sizeof *t);
}

static void qpc_callers_print(void)
{
    callers_print(s_qpc_by, "QPC");
    callers_print(s_bink_by, "Bink timer");
}

/* Bink's millisecond timer (RADTimerRead): its callers, for the loop that
 * reads it ~500k times a second during the intro flythrough. */
extern void sub_003AE3E0_gen(void);
void sub_003AE3E0(void)
{
    if (time_trace_on()) caller_note(s_bink_by, MEM32(g_esp));
    sub_003AE3E0_gen();
}

extern void sub_0032633B_gen(void);
void sub_0032633B(void)
{
    uint32_t out = MEM32(g_esp + 4), ret = MEM32(g_esp);
    sub_0032633B_gen();
    if (time_trace_on()) {
        qpc_caller(ret);
        time_note(1, ((uint64_t)MEM32(out + 4) << 32) | MEM32(out));
    }
}
extern void sub_0032634C_gen(void);
void sub_0032634C(void)
{
    uint32_t out = MEM32(g_esp + 4);
    sub_0032634C_gen();
    if (time_trace_on()) time_note(2, ((uint64_t)MEM32(out + 4) << 32) | MEM32(out));
}
extern void sub_0020E140_gen(void);
void sub_0020E140(void)
{
    sub_0020E140_gen();
    if (time_trace_on()) time_note(3, g_eax);
}

/* ── D3D's vblank handling, counted (MC3_PERF reports it) ──
 *
 * sub_0041B250 is D3D's vblank work: it advances the device's vblank count
 * (+0x1C0) and retires flips. Its callers run it when PMC_INTR_0's display
 * bit (0x01000000) is set -- on any interrupt, so a pushbuffer trap raised
 * while that bit is still up is counted as one more vblank. Should be 60 a
 * second; the report prints the real rate (2026-10-17: the intro flythrough
 * runs fast on Linux and reads no clock, so its time base is suspect). */
volatile long g_mc3_d3d_vblanks;
volatile uint32_t g_mc3_d3d_device;       /* ecx of the vblank handler: D3D's device */
extern void sub_0041B250_gen(void);
#ifdef __EMSCRIPTEN__
/* The handler acks PCRTC and spins at 0x0041B300 until PMC_INTR bit 24
 * drops; with the bits held for the DPC's 300 us grace and the flag thread
 * sleeping up to 1 ms, that was ~0.3-1.3 ms per vblank under the guest lock
 * (10% of the browser's game thread). Its caller has read the bits: clear
 * them now, as nfsu2-sw does. */
void xbox_Nv2aVblankTaken(void);
#endif
void sub_0041B250(void)
{
    g_mc3_d3d_vblanks++;
    g_mc3_d3d_device = g_ecx;
#ifdef __EMSCRIPTEN__
    xbox_Nv2aVblankTaken();
#endif
    sub_0041B250_gen();
}

/* ── The main loop's passes, counted (MC3_PERF reports them) ──
 *
 * sub_0001FA11 is the game's main loop; it calls sub_002D5400 once per
 * pass, before the update. On the console a pass is paced by the display
 * (60 a second); the intro flythrough runs ~10x fast on Linux while the
 * presented frame rate, the frame time and every clock read are right
 * (2026-10-18) -- so: passes per second against frames presented. */
volatile long g_mc3_loop_passes;
extern void sub_002D5400_gen(void);
void sub_002D5400(void)
{
    g_mc3_loop_passes++;
    sub_002D5400_gen();
}

/* ── D3D's PGRAPH handler: the trap protocol with an acknowledgement ──
 *
 * sub_0041B7C0 is D3D's PGRAPH interrupt handler: it reads PGRAPH_INTR and
 * NSOURCE (0xFD400100/0108), and for a software method (NOP) hands the
 * trapped parameter to the pushbuffer callback sub_0041B550. It acknowledges
 * by writing the status back -- write-one-to-clear on the chip, a no-op on
 * the plain memory here -- so the trap stayed visible, and D3D's interrupt
 * routine ran the handler again and again on it before the executor saw it
 * taken: 412,082 flips queued in one run (2026-10-18), D3D's flip targets
 * millions of vblanks ahead and its frame bookkeeping far ahead of the
 * screen. The toolkit's documented cure for that (NFSU2 hit the same): wrap
 * the handler under the executor's trap lock and report a trap it found as
 * taken, which clears what the hardware would (nv2a_pb_exec.c). */
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);
extern void sub_0041B7C0_gen(void);
void sub_0041B7C0(void)
{
    static int on = -1;
    uint32_t nsource;
    if (on < 0) {                      /* MC3_TRAP_HOOK=0: the old protocol, to compare */
        const char *e = getenv("MC3_TRAP_HOOK");
        on = !(e && e[0] == '0');
    }
    if (!on) {
        sub_0041B7C0_gen();
        return;
    }
    nv2a_pb_trap_lock();
    nsource = MEM32(0xFD400108u);
    sub_0041B7C0_gen();
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
}

/* ── Frame pacing: D3D's flip retirement (vblank) ──
 *
 * D3D queues each flip with a target vblank: previous target + the
 * presentation interval the title asked for (the low bits of the callback's
 * parameter; MC3's menus use 2, i.e. 30 fps). sub_0041B170, called from the
 * vblank handler, retires the flips whose target has come and returns how
 * many. The executor's FLIP_STALL waits for those retirements once the
 * project reports them (nv2a_pb_flip_retired); unreported, it waited one
 * vblank whatever the interval, and the 30 fps menus, flythrough and garage
 * played at 60 -- the game advances them per frame. A first attempt
 * (2026-10-17) stuck at 4 fps: the trap storm above had wrecked D3D's flip
 * queue (targets millions of vblanks ahead). With the trap acknowledged,
 * the queue is sane and retirement can pace the flips. MC3_FLIP_PACE=0
 * falls back to one flip per vblank. */
extern void nv2a_pb_flip_retired(unsigned n);
extern void sub_0041B170_gen(void);
void sub_0041B170(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_FLIP_PACE");
        on = !(e && e[0] == '0');
    }
    sub_0041B170_gen();
    if (on)
        nv2a_pb_flip_retired(g_eax);
}

/* ── D3D's pushbuffer callback, logged (MC3_FLIP_TRACE=1) ──
 *
 * sub_0041B550(param) runs when the GPU reaches one of D3D's software
 * methods (the executor's NOP trap). param & 0x1F is the command; for a
 * flip, (param >> 5) & 7 is the presentation interval and the frame's
 * target vblank becomes max(previous target +0x1C8 + interval, vblank
 * count +0x1C0). It showed the trap storm (targets millions of vblanks
 * ahead) and the menus' interval 2 (2026-10-18). First 40 calls, then
 * every 120th.
 * MC3_60FPS=1 (fps60_step, below): while a race or cruise steps 1/60 s, a
 * flip asking for interval 2 is rewritten to interval 1 before D3D sees it. */
static int s_fps60;                             /* this frame steps 1/60 (MC3_60FPS) */
extern void sub_0041B550_gen(void);
void sub_0041B550(void)
{
    static int on = -1;
    static long n;
    uint32_t dev = g_ecx, param = MEM32(g_esp + 4), t_before = 0, vb = 0;
    if (on < 0) {
        const char *e = getenv("MC3_FLIP_TRACE");
        on = e && e[0] == '1';
    }
    if (s_fps60 && (param & 0x1F) == 1 && ((param >> 5) & 7) == 2) {
        param = (param & ~(7u << 5)) | (1u << 5);
        MEM32(g_esp + 4) = param;
    }
    if (on && dev) { t_before = MEM32(dev + 0x1C8); vb = MEM32(dev + 0x1C0); }
    sub_0041B550_gen();
    if (on && dev && (++n <= 40 || n % 120 == 0))
        fprintf(stderr, "[FLIP] #%ld param %08X cmd %u data %X | target %u -> %u, next %u, vblank %u (dev %08X)\n",
                n, param, param & 0x1F, param >> 5, t_before, MEM32(dev + 0x1C8),
                MEM32(dev + 0x1C4), vb, dev);
}

/* ── The loading screen starts: compile the remembered shaders now ──
 *
 * sub_0001BC10 starts the loading-screen thread (0x0001BBF0, priority 14):
 * first behind the legal screen, while the game loads for seconds. Without
 * program binaries (the Switch's Mesa 20.1) the renderer compiled its whole
 * program list before the first logo: 4.8 s of black (2026-10-18). Now it
 * waits for this and compiles a program a frame behind a static screen
 * (nv2a_gl_prewarm_start; RECOMP_PROG_PREWARM=boot for the old way). */
extern void nv2a_gl_prewarm_start(void);
extern void sub_0001BC10_gen(void);
void sub_0001BC10(void)
{
    nv2a_gl_prewarm_start();
    sub_0001BC10_gen();
}

/* ── The loading screen keeps its 4:3 shape ──
 *
 * sub_0001B9D0 is the loading-screen thread's loop (thread 0x0001BBF0,
 * started only from the boot sequence at 0x00020387 (PORT?)): the legal screen.
 * With "legal" in MC3_FILL_SKIP it is shown at its own 4:3 shape, the fill
 * held while the loop runs. Not the default: the loop also runs behind
 * other loading screens and ends before some of them do, so they started
 * at 4:3 and switched to 16:9 halfway (2026-10-20). */
extern void nv2a_gl_fill_hold(int delta) __attribute__((weak));
extern void nv2a_gl_fill_freeze(int delta) __attribute__((weak));   /* RECOMP_GL_FILL_STEADY */
extern void sub_0001B9D0_gen(void);
static RECOMP_TLS int t_ls_thread;          /* this is the loading-screen thread */

#ifdef __EMSCRIPTEN__
/* ── "Skip intro" hides the legal screen too (browser, MC3_SKIP_MOVIES=hard) ──
 *
 * The legal screen (flash/legal_x.xbck: red, scrolling copyright and
 * trademark text) is this loop's first run, behind the boot load. Its
 * length is that load, not a timer: the main thread sets the stop flag
 * (+0x4004, sub_0001AD50) when it has loaded and waits for this thread
 * (WaitForSingleObject, INFINITE); the loop then plays the movie's
 * transition-out (StartOfTransitionOut -> EndOfTransitionOut) and leaves.
 * Players' logs (2026-11-11): 78 turns (~2.6 s) on a warm disc cache;
 * cold, the disc waits under it alone sum to 5-25 s (one log: 758 turns,
 * 25.3 s of waits). Each turn advanced and drew the movie (600-900 draws)
 * under the guest lock the loader needs. With hard skip the first run
 * draws nothing: each turn is the loop's own input poll (sub_00084C00),
 * BeginScene + Present (as at its exit: the black of the skipped movies
 * stays) and Sleep(24), so frames keep coming (the page's freeze watch).
 * At the stop the loop runs once with the movie hidden (+0x400C = 0: no
 * transition-out, nothing drawn), then the movie goes back for the main
 * thread's delete (sub_0001BB70), which is still waiting on this thread.
 * Opt-in since 2026-11-12 (MC3_SKIP_LEGAL=1): players' profiles went
 * missing at the title after it shipped; no file I/O is skipped here (the
 * per-turn calls left out, sub_002D3620 and sub_001B4E00, set D3D state),
 * but it stays off until a run shows profiles found with it on. */
extern void sub_00084C00(void);             /* the loop's per-turn input poll, cdecl () */
extern void sub_002D1830(void);             /* BeginScene, cdecl () */
void sub_002D2990(void);                    /* Present, cdecl () (override below) */
void sub_0001735C(void);                    /* Sleep, stdcall (ms) (override below) */
static double ls_now(void);
static int legal_hidden(uint32_t obj)
{
    static int runs;
    const char *s = getenv("MC3_SKIP_MOVIES"), *l = getenv("MC3_SKIP_LEGAL");
    uint32_t movie = MEM32(obj + 0x400C), r[8], turns = 0;
    double t0;

    if (runs++ || !s || strcmp(s, "hard") || !(l && l[0] == '1') || !movie || MEM8(obj + 0x4004))
        return 0;                           /* not the legal screen, or not skipping */
    t0 = ls_now();
    fprintf(stderr, "[LS] legal screen hidden: boot load running\n");   /* the page shows its Loading box (index.html) */
    r[0] = g_eax; r[1] = g_ecx; r[2] = g_edx; r[3] = g_ebx;
    r[4] = g_esp; r[5] = g_ebp; r[6] = g_esi; r[7] = g_edi;
    while (!MEM8(obj + 0x4004)) {
        g_esp -= 4; MEM32(g_esp) = 0;       /* return address */
        sub_00084C00();
        g_esp -= 4; MEM32(g_esp) = 0;
        sub_002D1830();
        g_esp -= 4; MEM32(g_esp) = 0;
        sub_002D2990();
        g_esp -= 4; MEM32(g_esp) = 24;      /* as the loop: Sleep(24), gives the lock away */
        g_esp -= 4; MEM32(g_esp) = 0;
        sub_0001735C();
        g_esp = r[4];
        turns++;
    }
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
    MEM32(obj + 0x400C) = 0;                /* the loop: no movie, done at once */
    sub_0001B9D0_gen();
    MEM32(obj + 0x400C) = movie;
    fprintf(stderr, "[LS] legal screen hidden (MC3_SKIP_MOVIES=hard): %u turns, %.1f s of boot load behind it\n",
            turns, (ls_now() - t0) / 1e3);
    return 1;
}
#endif

void sub_0001B9D0(void)
{
    const char *e = getenv("MC3_FILL_SKIP");
    int hold = nv2a_gl_fill_hold && e && strstr(e, "legal");   /* opt-in: see above */
    if (hold)
        nv2a_gl_fill_hold(1);
    t_ls_thread = 1;
    if (nv2a_gl_fill_freeze)
        nv2a_gl_fill_freeze(1);             /* the circle's in and out: no change of size */
#ifdef __EMSCRIPTEN__
    if (!legal_hidden(g_ecx))
#endif
    sub_0001B9D0_gen();
    if (nv2a_gl_fill_freeze)
        nv2a_gl_fill_freeze(-1);
    t_ls_thread = 0;
    if (hold)
        nv2a_gl_fill_hold(-1);
}

/* ── The loading screen's loop, timed (MC3_LS_TRACE=1) ──
 *
 * Each turn of sub_0001B9D0: the Flash movie advances a fixed 1/30 s
 * (sub_0013A0F0, 0x3D08882F), is drawn and presented (sub_002D2990), then
 * Sleep(24) (sub_0001735C). Logs every call on the loading thread with
 * its time, and presents from any other thread. It showed the loop turning
 * evenly every 33 ms while the circles stuttered (2026-10-24): the cause
 * was the renderer, which ignored the 2D blit that carries each frame into
 * the next back buffer (xboxrecomp, nv2a_pb_exec.c). */
static int ls_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_LS_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}
static double ls_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
const char *xbox_gil_my_label(void);
static void ls_log(const char *what, double t0)
{
    double t1 = ls_now();
    const char *l = xbox_gil_my_label();
    fprintf(stderr, "[LS] @%.3f %s %s %.1f ms\n", t0 / 1e3, t_ls_thread ? "ls" : l ? l : "?",
            what, t1 - t0);
}
static void ls_call(void (*fn)(void), const char *what, int any_thread)
{
    double t0;
    if (!ls_trace_on() || !(t_ls_thread || any_thread)) {
        fn();
        return;
    }
    t0 = ls_now();
    fn();
    ls_log(what, t0);
}
extern void sub_0013A0F0_gen(void);
void sub_0013A0F0(void) { ls_call(sub_0013A0F0_gen, "advance", 0); }
extern void sub_002D2990_gen(void);
void sub_002D2990(void) { ls_call(sub_002D2990_gen, "present", 1); }
/* ── The heartbeat thread sleeps instead of spinning (MC3_HEARTBEAT_MS) ──
 *
 * sub_001B15D0 (its own thread) is a one-second watchdog: enter a critical
 * section, compare the CPU clock with the last beat (ticks / 733 MHz >=
 * 1.0 s: sub_00047CE0), leave, Sleep(0) at 0x001B164D, again. On the
 * Switch each turn took the guest lock from the main thread: 4,200 turns
 * a second, the main thread waiting behind it 3,400 times a second at
 * ~50 us a hand-over -- most of its 30% waiting in the menus (19.5 fps,
 * 2026-10-21 log). A beat checked every few milliseconds is as good.
 * This turns that one Sleep(0) into Sleep(MC3_HEARTBEAT_MS), default 8;
 * 0 restores the game's own. */
static uint32_t heartbeat_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("MC3_HEARTBEAT_MS");
        ms = e ? atoi(e) : 8;
        if (ms < 0) ms = 0;
    }
    return (uint32_t)ms;
}

extern void sub_0001735C_gen(void);
void sub_0001735C(void)
{
    if (MEM32(g_esp) == 0x001B1654u && MEM32(g_esp + 4) == 0) {   /* the heartbeat's Sleep(0) */
        static int said;
        MEM32(g_esp + 4) = heartbeat_ms();
        if (!said++) {
            const char *l = xbox_gil_my_label();
            fprintf(stderr, "[MC3] heartbeat thread (%s): Sleep(0) -> Sleep(%u) (MC3_HEARTBEAT_MS)\n",
                    l ? l : "?", heartbeat_ms());
        }
    }
    ls_call(sub_0001735C_gen, "sleep", 0);
}

/* The way out: once the game sets the stop flag (+0x2004 of the loop's
 * object), each turn sends the Flash movie to "StartOfTransitionOut"
 * (sub_00139520, this = movie, label, 1) and leaves when sub_00139690
 * says it has reached "EndOfTransitionOut" (returns 1). Logged with their
 * arguments and results on the loading thread. A load that ends while the
 * intro still plays (fast storage: 0.5 s) gets a transition-out with its
 * shapes still off stage, so the screen cuts away with the spinner up; at
 * a DVD's pace (RECOMP_IO_MBPS=8 RECOMP_IO_SEEK_MS=80) it plays out. */
static void ls_label(void (*fn)(void), const char *what)
{
    uint32_t movie = g_ecx, label = MEM32(g_esp + 4), arg2 = MEM32(g_esp + 8);
    char name[40];
    uint32_t i;
    fn();
    if (!ls_trace_on() || !t_ls_thread)
        return;
    for (i = 0; i + 1 < sizeof name && label && MEM8(label + i); i++)
        name[i] = (char)MEM8(label + i);
    name[i] = 0;
    fprintf(stderr, "[LS] @%.3f ls %s(movie %08X, \"%s\", %u) -> %u\n", ls_now() / 1e3,
            what, movie, name, arg2, g_eax);
}
extern void sub_00139520_gen(void);
void sub_00139520(void) { ls_label(sub_00139520_gen, "goto"); }
extern void sub_00139690_gen(void);
void sub_00139690(void) { ls_label(sub_00139690_gen, "reached"); }

/* ── Save-game deletion, traced (MC3_SAVE_TRACE=1) ──
 *
 * Deleting a profile says "deleted" but leaves it on disk, and no file call
 * reaches the kernel (2026-10-18). sub_0002A950(name) is the game's delete:
 * it calls XDeleteSaveGame (sub_00324C8E) with "<letter>:\\" only when its
 * device's drive letter ([this+8]) is set. This logs which way it goes and
 * what XDeleteSaveGame returned. */
static int save_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_SAVE_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}

static void save_trace_name(char *out, size_t cap, uint32_t va)
{
    size_t i = 0;
    for (; va && i + 1 < cap; i++) {        /* wide string, ASCII shown */
        uint16_t c = MEM16(va + 2 * i);
        if (!c) break;
        out[i] = c < 0x80 ? (char)c : '?';
    }
    out[i] = 0;
}

extern void sub_0002A950_gen(void);
void sub_0002A950(void)
{
    uint32_t self = g_ecx, name = MEM32(g_esp + 4), ret = MEM32(g_esp);
    char n[64];
    if (save_trace_on()) {
        save_trace_name(n, sizeof n, name);
        fprintf(stderr, "[SAVE] delete \"%s\" device %08X letter '%c' (0x%02X), from %08X\n",
                n, self, MEM8(self + 8) >= 0x20 ? MEM8(self + 8) : '?', MEM8(self + 8), ret);
    }
    sub_0002A950_gen();
    if (save_trace_on())
        fprintf(stderr, "[SAVE] delete -> %u\n", g_eax);
}

extern void sub_00324C8E_gen(void);
void sub_00324C8E(void)
{
    uint32_t root = MEM32(g_esp + 4), name = MEM32(g_esp + 8);
    char n[64], r[8] = {0};
    int i;
    if (save_trace_on()) {
        save_trace_name(n, sizeof n, name);
        for (i = 0; i < 7 && root && MEM8(root + i); i++)
            r[i] = (char)MEM8(root + i);
        fprintf(stderr, "[SAVE] XDeleteSaveGame(\"%s\", \"%s\")\n", r, n);
    }
    sub_00324C8E_gen();
    if (save_trace_on())
        fprintf(stderr, "[SAVE] XDeleteSaveGame -> %u\n", g_eax);
}

/* ── "Is pad n a steering wheel?", asked once per controller ──
 *
 * sub_00084BD0 (thiscall, the game's pad object) returns
 * XInputGetCapabilities(handle).SubType == 0x10 (XINPUT_DEVSUBTYPE_GC_WHEEL),
 * and the game asks every frame. XInputGetCapabilities is a synchronous USB
 * control transfer (XPP 0x00441DAF, waiting at 0x0044535B with a 50 ms
 * timeout): ~1-2 ms on the Xbox, ~19 ms on the emulated OHCI on the Switch,
 * where the transfer waits for the controller thread's 4 ms pass and the
 * completion interrupt for the guest lock. Two per frame took 450 ms of
 * every second of the main thread in a race (10-12 fps, 2026-10-20). The
 * answer cannot change while the same handle is open, so it is kept per
 * handle ([pad+0x34]; [pad+0x1C] == 4 is "open"). MC3_PAD_CAPS_CACHE=0
 * asks every time, as the game does. */
extern void sub_00084BD0_gen(void);
void sub_00084BD0(void)
{
    static uint32_t handle_of[8], answer_of[8];
    static unsigned next;
    static int on = -1;
    uint32_t pad = g_ecx, handle = MEM32(pad + 0x34);
    int open = MEM32(pad + 0x1C) == 4 && handle;
    unsigned i;
    if (on < 0) {
        const char *e = getenv("MC3_PAD_CAPS_CACHE");
        on = !(e && e[0] == '0');
    }
    if (on && open)
        for (i = 0; i < 8; i++)
            if (handle_of[i] == handle) {
                g_eax = answer_of[i];
                g_esp += 4;                      /* ret: thiscall, no arguments */
                return;
            }
    sub_00084BD0_gen();
    if (on && open) {
        handle_of[next % 8] = handle;
        answer_of[next % 8] = g_eax & 0xFFu;
        next++;
    }
}

/* ── Stick shaping (sub_000847B0), traced ──
 *
 * The pad object's read (sub_000848F0) turns each stick into two bytes
 * (0x80 = centred) and hands them, as esi/edi pointers, to sub_000847B0,
 * which shapes them in SSE + x87 (the DUB Edition's twin is x87 only).
 * MC3_PAD_TRACE=1 logs the bytes going in and out, once a second while
 * they hold still and at every change. */
extern void sub_000847B0_gen(void);
void sub_000847B0(void)
{
    static int on = -1;
    static uint32_t last_key[2];
    static unsigned n[2];
    uint32_t px = g_esi, py = g_edi;
    uint8_t x0, y0;
    if (on < 0) {
        const char *e = getenv("MC3_PAD_TRACE");
        const char *t = getenv("MC3_PAD_SELFTEST");
        on = e && e[0] == '1';
        if (t && t[0] == '1') {
            /* Once: the lifted shaping over every x byte (y centred) with
             * this call's weight, for tools/difftest-style comparison with
             * the original in Unicorn. Guest stack below esp as scratch. */
            uint32_t sx = g_esp - 0x200, sy = g_esp - 0x1FC, arg = MEM32(g_esp + 4);
            uint32_t esi0 = g_esi, edi0 = g_edi, esp0 = g_esp, b;
            char line[1100];
            size_t q = 0;
            for (b = 0; b < 256; b++) {
                MEM8(sx) = (uint8_t)b;
                MEM8(sy) = 0x80;
                g_esi = sx; g_edi = sy;
                g_esp = esp0 - 0x100;
                g_esp -= 4; MEM32(g_esp) = arg;
                g_esp -= 4; MEM32(g_esp) = 0;          /* return address */
                sub_000847B0_gen();                    /* ret: pops it */
                q += (size_t)snprintf(line + q, sizeof line - q, "%02X", MEM8(sx));
            }
            g_esi = esi0; g_edi = edi0; g_esp = esp0;
            fprintf(stderr, "[PAD] selftest weight %08X: x out for x in 00..FF: %s\n", arg, line);
        }
    }
    if (!on) {
        sub_000847B0_gen();
        return;
    }
    x0 = MEM8(px); y0 = MEM8(py);
    sub_000847B0_gen();
    {
        int which = (px & 3) == 2;         /* +0x28/0x29 left, +0x2A/0x2B right */
        uint32_t key = (uint32_t)x0 << 24 | (uint32_t)y0 << 16 | MEM8(px) << 8 | MEM8(py);
        if (key != last_key[which] || ++n[which] % 60 == 0) {
            fprintf(stderr, "[PAD] %s stick in %02X,%02X -> out %02X,%02X (pad 0x%08X)\n",
                    which ? "right" : "left", x0, y0, MEM8(px), MEM8(py), px & ~3u);
            last_key[which] = key;
        }
    }
}

/* ── Race cameras, traced and capturable ──
 *
 * The camera classes (camTrackCS, camPolarCS, camPovCS, camPointCS; their
 * vtables in .rdata at 0x00476500-0x00476E40 and 0x004A4200-0x004A4E60)
 * share a 15-slot layout; wrapped here are their own methods (slots 3, 4,
 * 8-10, 13, 14) and the camera base's (sub_001383F0 tune/camera,
 * sub_00138470 CameraFOV, sub_00138510). The DUB Edition's are x87, the
 * Remix's SSE.
 *
 * MC3_CAM_TRACE=1: once a second, each method called that second, how
 * often, and its object and vtable (which class is active).
 * MC3_DIFFTEST_CALL=key (src/difftest.c): F10 captures the next call of
 * each, for tools/difftest.py. */
extern void mc3_difftest_call(uint32_t func, void (*lifted)(void), long *calls);

#define CAM_N 54
static uint32_t cam_addr[CAM_N], cam_this[CAM_N];
static long cam_calls[CAM_N], cam_count[CAM_N];

/* ── The keyboard while driving ──
 *
 * The keyboard's up arrow is the gas (right trigger) and, for menus, left
 * stick up. In a race the Remix reads stick up/down for its camera (it
 * rises and moves in, 2026-10-25; the DUB Edition ignores it), so with
 * the gas held the camera stayed pulled in. While the player's race camera
 * control (sub_002C1520) runs every frame and the game is not paused, the
 * keyboard leaves the stick alone (xbox_keyboard_driving, sdl_keyboard.c).
 * Paused / menu: the state machine's flag at +9 (sub_002C48C0 turns the
 * look-around off on it) or state 3. MC3_KEYS_TRACE=1 logs each change. */
extern int (*xbox_keyboard_driving)(void);
static volatile uint32_t s_race_cam_ms;

static uint32_t host_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static int guest_ptr_ok(uint32_t p)
{
    return (p >= 0x10000u && p < 0x08000000u) || (p >= 0x80000000u && p < MC3_CONTIG_END);
}

/* An FE menu is up over the race: the FE screen manager M =
 * [[[0x50F148]+0xC]+0xC] (built as "RacePauseMenu", sub_00156C90) keeps the
 * screen asked for at +0xE0 (Go sub_00163830, sub_001610D0) and the one
 * shown at +0xD8. 1 is none (ctor sub_001625D0; the pause menu's resume,
 * sub_00161230, sets both back to 1); 0x25 is the race HUD's own overlay,
 * opened and closed by the race view (sub_002C5C80 -> sub_00154AB0 /
 * sub_00154AE0), no menu. Anything else is a screen: 2 / 0x3A pause
 * (sub_00154640), 5 MenuRaceOver (sub_00154980), 0x12 host left, ... A
 * System Link race does not pause, so under the race-over menu the race
 * camera keeps running and (likely the host's case; sub_00154980 opens it
 * one way in state 3, another otherwise) the state is not 3: only this tells
 * the menu. Not M+0x44 (sub_001546E0's byte): set while driving too
 * (2026-11-09). */
static int fe_menu_up(void)
{
    uint32_t fe = MEM32(0x0050F148u), s, m, shown, asked;
    if (!guest_ptr_ok(fe) || !guest_ptr_ok(s = MEM32(fe + 0xCu)) || !guest_ptr_ok(m = MEM32(s + 0xCu)))
        return 0;
    shown = MEM32(m + 0xD8u);
    asked = MEM32(m + 0xE0u);
    return (shown != 1u && shown != 0x25u) || (asked != 1u && asked != 0x25u);
}

static int remix_driving(void)
{
    static int last = -1;
    uint32_t sm = MEM32(0x00557ED0u);
    int on = s_race_cam_ms && host_ms() - s_race_cam_ms < 250u
             && sm && MEM32(sm + 4) != 3 && MEM8(sm + 9) == 0 && !fe_menu_up();
    if (on != last) {
        const char *e = getenv("MC3_KEYS_TRACE");
        if (e && e[0] == '1')
            fprintf(stderr, "[KEYS] %s: up/down %s\n", on ? "driving" : "not driving",
                    on ? "are the triggers only" : "move the stick too");
        last = on;
    }
    return on;
}

/* Installed at load, so menus (garage) are known as "not driving" before the
 * first race: up/down are then the stick alone (sdl_keyboard.c). */
__attribute__((constructor)) static void remix_driving_install(void)
{
    xbox_keyboard_driving = remix_driving;
}

static void race_camera_ran(void)
{
    s_race_cam_ms = host_ms();
    xbox_keyboard_driving = remix_driving;
}

static void cam_call(int slot, uint32_t addr, void (*lifted)(void))
{
    static int on = -1;
    static time_t last;
    time_t now;
    int i;
    if (on < 0) {
        const char *e = getenv("MC3_CAM_TRACE");
        on = e && e[0] == '1';
    }
    if (on) {
        cam_addr[slot] = addr;
        cam_this[slot] = g_ecx;
        cam_count[slot]++;
        now = time(NULL);
        if (now != last) {
            for (i = 0; i < CAM_N; i++)
                if (cam_count[i]) {
                    uint32_t t = cam_this[i];
                    uint32_t vt = (t >= 0x10000u && t < 0x08000000u) || (t >= 0x80000000u && t < MC3_CONTIG_END) ? MEM32(t) : 0;
                    fprintf(stderr, "[CAM] sub_%08X x%ld this=%08X vtable=%08X\n",
                            cam_addr[i], cam_count[i], cam_this[i], vt);
                    cam_count[i] = 0;
                }
            fprintf(stderr, "[CAM] --\n");
            last = now;
        }
    }
    if (addr == 0x002C1520u)
        race_camera_ran();
    mc3_difftest_call(addr, lifted, &cam_calls[slot]);
}

extern void sub_000AF930_gen(void);
void sub_000AF930(void) { cam_call(0, 0x000AF930u, sub_000AF930_gen); }
extern void sub_001383F0_gen(void);
void sub_001383F0(void) { cam_call(1, 0x001383F0u, sub_001383F0_gen); }
extern void sub_00138470_gen(void);
void sub_00138470(void) { cam_call(2, 0x00138470u, sub_00138470_gen); }
extern void sub_00138510_gen(void);
void sub_00138510(void) { cam_call(3, 0x00138510u, sub_00138510_gen); }
extern void sub_001E28A0_gen(void);
void sub_001E28A0(void) { cam_call(4, 0x001E28A0u, sub_001E28A0_gen); }
extern void sub_002CDDE0_gen(void);
void sub_002CDDE0(void) { cam_call(5, 0x002CDDE0u, sub_002CDDE0_gen); }
extern void sub_002CE2E0_gen(void);
void sub_002CE2E0(void) { cam_call(6, 0x002CE2E0u, sub_002CE2E0_gen); }
extern void sub_002CE300_gen(void);
void sub_002CE300(void) { cam_call(7, 0x002CE300u, sub_002CE300_gen); }
extern void sub_002CE650_gen(void);
void sub_002CE650(void) { cam_call(8, 0x002CE650u, sub_002CE650_gen); }
extern void sub_002CF080_gen(void);
void sub_002CF080(void) { cam_call(9, 0x002CF080u, sub_002CF080_gen); }
extern void sub_002CF4A0_gen(void);
void sub_002CF4A0(void) { cam_call(10, 0x002CF4A0u, sub_002CF4A0_gen); }
extern void sub_002CF6C0_gen(void);
void sub_002CF6C0(void) { cam_call(11, 0x002CF6C0u, sub_002CF6C0_gen); }
extern void sub_002CF710_gen(void);
void sub_002CF710(void) { cam_call(12, 0x002CF710u, sub_002CF710_gen); }
extern void sub_002CF760_gen(void);
void sub_002CF760(void) { cam_call(13, 0x002CF760u, sub_002CF760_gen); }
extern void sub_002CF970_gen(void);
void sub_002CF970(void) { cam_call(14, 0x002CF970u, sub_002CF970_gen); }
extern void sub_002CFED0_gen(void);
void sub_002CFED0(void) { cam_call(15, 0x002CFED0u, sub_002CFED0_gen); }
extern void sub_002CFF50_gen(void);
void sub_002CFF50(void) { cam_call(16, 0x002CFF50u, sub_002CFF50_gen); }
extern void sub_002CFF60_gen(void);
void sub_002CFF60(void) { cam_call(17, 0x002CFF60u, sub_002CFF60_gen); }
extern void sub_002D01C0_gen(void);
void sub_002D01C0(void) { cam_call(18, 0x002D01C0u, sub_002D01C0_gen); }
extern void sub_00354C90_gen(void);
void sub_00354C90(void) { cam_call(19, 0x00354C90u, sub_00354C90_gen); }
extern void sub_00355140_gen(void);
void sub_00355140(void) { cam_call(20, 0x00355140u, sub_00355140_gen); }
extern void sub_00355170_gen(void);
void sub_00355170(void) { cam_call(21, 0x00355170u, sub_00355170_gen); }
extern void sub_00357410_gen(void);
void sub_00357410(void) { cam_call(22, 0x00357410u, sub_00357410_gen); }
extern void sub_00357960_gen(void);
void sub_00357960(void) { cam_call(23, 0x00357960u, sub_00357960_gen); }
extern void sub_00357D10_gen(void);
void sub_00357D10(void) { cam_call(24, 0x00357D10u, sub_00357D10_gen); }
extern void sub_00357FF0_gen(void);
void sub_00357FF0(void) { cam_call(25, 0x00357FF0u, sub_00357FF0_gen); }
extern void sub_003580C0_gen(void);
void sub_003580C0(void) { cam_call(26, 0x003580C0u, sub_003580C0_gen); }
extern void sub_00358110_gen(void);
void sub_00358110(void) { cam_call(27, 0x00358110u, sub_00358110_gen); }
extern void sub_00362200_gen(void);
void sub_00362200(void) { cam_call(28, 0x00362200u, sub_00362200_gen); }
extern void sub_00363030_gen(void);
void sub_00363030(void) { cam_call(29, 0x00363030u, sub_00363030_gen); }
extern void sub_003635D0_gen(void);
void sub_003635D0(void) { cam_call(30, 0x003635D0u, sub_003635D0_gen); }
extern void sub_003636A0_gen(void);
void sub_003636A0(void) { cam_call(31, 0x003636A0u, sub_003636A0_gen); }

/* The race's per-frame path down to the cameras (from the stack of a
 * capture in the spin, 2026-10-25): sub_00198E90 (world update) ->
 * sub_002BDEC0 (player) -> sub_002C1520 (the player's camera control)
 * -> sub_002C5B30 -> the camera's update. Wrapped for the census and for
 * MC3_DIFFTEST_FUNC=198E90,2BDEC0,2C1520,2C5B30. */
extern void sub_00198E90_gen(void);
void sub_00198E90(void) { cam_call(32, 0x00198E90u, sub_00198E90_gen); }
extern void sub_002BDEC0_gen(void);
static void roam_pool_frame(void);              /* free roam: remote cars after the step (roam_pool.c) */
void sub_002BDEC0(void) { cam_call(33, 0x002BDEC0u, sub_002BDEC0_gen); roam_pool_frame(); }
extern void sub_002C1520_gen(void);
static void roam_player(uint32_t p);            /* free roam (O3), roam_game.c */
static void rra_player(uint32_t p);             /* free roam: AI race prompt, start, pose back (roam_race_ai.c) */
static int rch_player(uint32_t p);              /* free roam: street challenge; 1 owns prompt + White (roam_challenge.c) */
void sub_002C1520(void)
{
    if (!rch_player(g_ecx))
        rra_player(g_ecx);
    roam_player(g_ecx);
    cam_call(34, 0x002C1520u, sub_002C1520_gen);
}
extern void sub_002C5B30_gen(void);
void sub_002C5B30(void) { cam_call(35, 0x002C5B30u, sub_002C5B30_gen); }

/* The drawing side: the projection builder sub_002D4AE0 (thiscall on a
 * viewport; fov, aspect, near, far on the stack) and the code that calls
 * it while a frame is drawn. The chase camera computes its FOV right
 * (dolly zoom: 40 deg standing, up to 60 at speed, while it moves in to
 * 0.8 of its distance), yet on screen only the move-in shows. MC3_CAM_TRACE
 * logs what the builder is handed, once a second. */
extern void sub_002D4F10_gen(void);
void sub_002D4F10(void) { cam_call(36, 0x002D4F10u, sub_002D4F10_gen); }
extern void sub_00331840_gen(void);
void sub_00331840(void) { cam_call(37, 0x00331840u, sub_00331840_gen); }
extern void sub_00331A30_gen(void);
void sub_00331A30(void) { cam_call(38, 0x00331A30u, sub_00331A30_gen); }
extern void sub_00332BD0_gen(void);
void sub_00332BD0(void) { cam_call(39, 0x00332BD0u, sub_00332BD0_gen); }
extern void sub_00138640_gen(void);
void sub_00138640(void) { cam_call(40, 0x00138640u, sub_00138640_gen); }
extern void sub_0020ED50_gen(void);
void sub_0020ED50(void) { cam_call(41, 0x0020ED50u, sub_0020ED50_gen); }
extern void sub_002D4AE0_gen(void);
static void roam_tags_view(uint32_t vp, uint32_t ret); /* free roam: name tags from this view (roam_tags.c) */
void sub_002D4AE0(void)
{
    static uint32_t vp[8];
    static time_t last[8];
    const char *e = getenv("MC3_CAM_TRACE");
    uint32_t tag_vp = g_ecx, tag_ret = MEM32(g_esp);
    int i;
    for (i = 0; i < 7 && vp[i] && vp[i] != g_ecx; i++)
        ;                                  /* one line per viewport (main view, mirror, map) */
    vp[i] = g_ecx;
    if (e && e[0] == '1' && time(NULL) != last[i]) {
        last[i] = time(NULL);
        fprintf(stderr, "[CAM] projection: viewport %08X fov %.3f aspect %.3f near %.3f far %.1f (from %08X)\n",
                g_ecx, MEMF(g_esp + 4), MEMF(g_esp + 8), MEMF(g_esp + 12), MEMF(g_esp + 16), MEM32(g_esp));
    }
#ifdef __EMSCRIPTEN__
    {
        /* Hor+ (WebGPU, RECOMP_ASPECT): the renderer shows the sides of a
         * wider screen, so the frustum's side planes (+0x140..+0x14C) must
         * cull k times wider; everything else stays, +0x138 (tan * aspect)
         * too: the minimap scales its map by it (widened, the map zoomed in
         * k times). Aspect 0 means the viewport's own (+0x128). The builder hands the
         * projection (+4) to D3D itself when its viewport is the current one
         * ([0x558BB4], sub_00411D80): patching m00 afterwards left that
         * upload k times too narrow (the minimap and the car reflection
         * passes, built while current, drew squeezed). So it runs twice:
         * widened for the side planes, then as the title called it, and the
         * wide planes go back over the narrow ones. Its arguments (return
         * address, fov, aspect, near, far) are its scratch: kept for the
         * second call. */
        extern float nv2a_wgpu_wide(void);
        float k = nv2a_wgpu_wide();
        if (k > 1.0f) {
            uint32_t vp = g_ecx, sp = g_esp, args[5], wide[4], i;
            float a = MEMF(sp + 8);
            for (i = 0; i < 5; i++)
                args[i] = MEM32(sp + 4 * i);
            MEMF(sp + 8) = (a != 0.0f ? a : MEMF(vp + 0x128)) * k;
            sub_002D4AE0_gen();
            for (i = 0; i < 4; i++)
                wide[i] = MEM32(vp + 0x140 + 4 * i);
            for (i = 0; i < 5; i++)
                MEM32(sp + 4 * i) = args[i];
            g_esp = sp;
            g_ecx = vp;
            cam_call(42, 0x002D4AE0u, sub_002D4AE0_gen);
            for (i = 0; i < 4; i++)
                MEM32(vp + 0x140 + 4 * i) = wide[i];
            roam_tags_view(tag_vp, tag_ret);
            return;
        }
    }
#endif
    cam_call(42, 0x002D4AE0u, sub_002D4AE0_gen);
    roam_tags_view(tag_vp, tag_ret);
}

/* The rest of the race's frame update (the dispatcher at 0x0001EEDE calls
 * them in turn after sub_00198E90): wrapped so a capture can replay the
 * car's physics too. The camera and the drawing replay exactly at speed
 * (2026-10-25), so the zoom's wrong input comes from here. */
extern void sub_00034A10_gen(void);
void sub_00034A10(void) { cam_call(44, 0x00034A10u, sub_00034A10_gen); }
extern void sub_000398B0_gen(void);
void sub_000398B0(void) { cam_call(45, 0x000398B0u, sub_000398B0_gen); }
extern void sub_00036710_gen(void);
void sub_00036710(void) { cam_call(46, 0x00036710u, sub_00036710_gen); }
extern void sub_001C1FC0_gen(void);
void sub_001C1FC0(void) { cam_call(47, 0x001C1FC0u, sub_001C1FC0_gen); }
extern void sub_0009B270_gen(void);
void sub_0009B270(void) { cam_call(48, 0x0009B270u, sub_0009B270_gen); }
extern void sub_00034340_gen(void);
void sub_00034340(void) { cam_call(49, 0x00034340u, sub_00034340_gen); }
extern void sub_00032150_gen(void);
void sub_00032150(void) { cam_call(50, 0x00032150u, sub_00032150_gen); }
extern void sub_00049FC0_gen(void);
void sub_00049FC0(void) { cam_call(51, 0x00049FC0u, sub_00049FC0_gen); }
extern void sub_0006CC90_gen(void);
void sub_0006CC90(void) { cam_call(52, 0x0006CC90u, sub_0006CC90_gen); }
extern void sub_000EC910_gen(void);
void sub_000EC910(void) { cam_call(53, 0x000EC910u, sub_000EC910_gen); }

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345 (PORT?)(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 (PORT?) entered, eax=0x%08X\n", g_eax);
 *       sub_00012345 (PORT?)();
 *       fprintf(stderr, "[TRACE] sub_00012345 (PORT?) returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345 (PORT?)) return traced_sub_00012345;
     * if (xbox_va == 0x0006D0D0) return stub_00067890;
     * if (xbox_va == 0x000ABCDE (PORT?)) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
/* Where an indirect call came from: the call site's return address (pushed
 * just before RECOMP_ICALL, at g_esp) and the next code addresses up the
 * stack, the callers' return addresses. */
static void icall_where(void)
{
    int found = 0;

    fprintf(stderr, "  called from 0x%08X; up the stack:", MEM32(g_esp));
    for (uint32_t i = 1; i < 256 && found < 8; i++) {
        uint32_t v = MEM32(g_esp + i * 4);
        if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
            fprintf(stderr, " %08X", v);
            found++;
        }
    }
    fprintf(stderr, "\n");
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    icall_where();

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    icall_where();
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── Career race results: listeners checked before they are called ──
 *
 * mcPgCareer::ReportRaceResult() (sub_00277EE0, named by its own messages)
 * hands the result to sub_002793C0, which calls every listener in a list
 * of 12-byte entries {u16 id, u32 key, object*} at this+0x78..+0x7C
 * (vtable slot 0x28; the entry at index this+0x94 first, -1 = none);
 * sub_00279430 does the same through slot 0x2C. Starting one career race
 * on the Switch, and after ~8 minutes on the Mac, the list held a stale
 * entry: first a null method (the runtime skipped the call), then an
 * object at garbage (0xCDCDCD08 on the Mac, 0x535F4D7C on the Switch), and
 * the game crashed (2026-10-21; same call chain on both). Which object
 * leaves its entry behind is not known yet. Until it is, entries whose
 * object, vtable or method do not point where they must (object in RAM,
 * vtable in .rdata, method in code) are taken out of the list before the
 * call and logged with what they held. MC3_LISTENER_CHECK=0 turns it off. */
static int listener_ram(uint32_t va)
{
    return !(va & 3) && ((va >= 0x00010000u && va < 0x04000000u)
                         || (va >= 0x80000000u && va < MC3_CONTIG_END));
}

static int listener_valid(uint32_t obj, unsigned slot)
{
    uint32_t vt, fn;
    if (!listener_ram(obj))
        return 0;
    vt = MEM32(obj);
    if (vt < 0x004476E0u || vt >= 0x004A8140u || (vt & 3))   /* .rdata */
        return 0;
    fn = MEM32(vt + slot);
    return fn >= 0x00012000u && fn < 0x004472A0u;               /* code */
}

static void listener_prune(uint32_t self, unsigned slot, const char *who)
{
    static int on = -1, logged;
    uint32_t begin, end, rd, wr, idx, new_idx = 0xFFFFFFFFu, i;
    if (on < 0) {
        const char *e = getenv("MC3_LISTENER_CHECK");
        on = !(e && e[0] == '0');
    }
    if (!on || !listener_ram(self))
        return;
    begin = MEM32(self + 0x78);
    end = MEM32(self + 0x7C);
    idx = MEM32(self + 0x94);
    if (!listener_ram(begin) || end < begin || end - begin > 12u * 4096 || (end - begin) % 12) {
        if (logged++ < 20)
            fprintf(stderr, "[LISTEN] %s: list of %08X is not a list (begin %08X end %08X): left alone\n",
                    who, self, begin, end);
        return;
    }
    for (rd = wr = begin, i = 0; rd < end; rd += 12, i++) {
        uint32_t obj = MEM32(rd + 8);
        if (!listener_valid(obj, slot)) {
            if (logged++ < 20)
                fprintf(stderr, "[LISTEN] %s: entry %u of %08X dropped: id %04X key %08X object %08X "
                                "vtable %08X%s\n", who, i, self, MEM16(rd), MEM32(rd + 4), obj,
                        listener_ram(obj) ? MEM32(obj) : 0, i == idx ? " (the current one)" : "");
            continue;
        }
        if (i == idx)
            new_idx = (wr - begin) / 12;
        if (wr != rd) {
            MEM32(wr) = MEM32(rd);
            MEM32(wr + 4) = MEM32(rd + 4);
            MEM32(wr + 8) = MEM32(rd + 8);
        }
        wr += 12;
    }
    if (wr != end) {
        MEM32(self + 0x7C) = wr;
        if (idx != 0xFFFFFFFFu)
            MEM32(self + 0x94) = new_idx;
    }
}

/* The broadcast itself, done safely. The crash came back with every entry
 * valid before the call (2026-10-21, no [LISTEN] line; the loop pointer
 * esi inside the list, a garbage object read): a listener's callback
 * changes the list while the original loop walks it, so the loop reads an
 * entry that was moved or freed. On the Xbox freed memory keeps its old
 * contents and the game gets away with it. So: the current entry first
 * (this+0x94), as the original, then every other entry from a copy taken
 * before the calls, each called only if it is still in the list and valid
 * at that moment. thiscall, one argument, callee pops it (ret 4). */
static void listener_call(uint32_t obj, unsigned slot, uint32_t arg)
{
    uint32_t fn_va = MEM32(MEM32(obj) + slot);
    recomp_func_t fn = recomp_lookup(fn_va);
    if (!fn)
        fn = recomp_lookup_manual(fn_va);
    if (!fn) {
        fprintf(stderr, "[LISTEN] method %08X of %08X not in dispatch: skipped\n", fn_va, obj);
        return;
    }
    {
        static int trace = -1;
        if (trace < 0) {
            const char *e = getenv("MC3_LISTEN_TRACE");
            trace = e && e[0] == '1';
        }
        if (trace)
            fprintf(stderr, "[LISTEN]   -> object %08X vtable %08X method %08X (arg %u)\n",
                    obj, MEM32(obj), fn_va, arg);
    }
    {
        /* The callee must pop its return address and argument (ret 4) and
         * keep ebx, esi, edi, ebp, as MSVC's thiscall does. A translated
         * method that does not is a lifter bug; the original loop kept its
         * position in esi and crashed on one. Say which method, and put the
         * stack back where the call left it. */
        uint32_t esp0 = g_esp, bx = g_ebx, si = g_esi, di = g_edi, bp = g_ebp;
        static int logged;
        g_esp -= 4; MEM32(g_esp) = arg;
        g_esp -= 4; MEM32(g_esp) = 0;          /* return address */
        g_ecx = obj;
        fn();                                  /* ret 4: pops both */
        if (g_esp != esp0 || g_ebx != bx || g_esi != si || g_edi != di || g_ebp != bp) {
            if (logged++ < 20)
                fprintf(stderr, "[LISTEN] method %08X of object %08X (vtable %08X +%02X) broke the "
                                "calling convention:%s%s%s%s%s esp %+d\n", fn_va, obj, MEM32(obj), slot,
                        g_ebx != bx ? " ebx" : "", g_esi != si ? " esi" : "", g_edi != di ? " edi" : "",
                        g_ebp != bp ? " ebp" : "", "", (int)(g_esp - esp0));
            g_esp = esp0;
        }
    }
}

static int listener_live(uint32_t self, uint32_t obj, uint16_t id, uint32_t key)
{
    uint32_t e, begin = MEM32(self + 0x78), end = MEM32(self + 0x7C);
    if (!listener_ram(begin) || end < begin || end - begin > 12u * 4096)
        return 0;
    for (e = begin; e + 12 <= end; e += 12)
        if (MEM32(e + 8) == obj && MEM16(e) == id && MEM32(e + 4) == key)
            return 1;
    return 0;
}

#define LISTEN_MAX 512
static void listener_broadcast(uint32_t self, unsigned slot, const char *who)
{
    static int changed_logged;
    uint32_t begin, end, idx, n, i, arg = MEM32(g_esp + 4);
    uint32_t cur_key = 0, saved_ebx = g_ebx, saved_esi = g_esi, saved_edi = g_edi, saved_ebp = g_ebp;
    uint16_t cur_id = 0;
    struct { uint16_t id; uint32_t key, obj; } snap[LISTEN_MAX];

    listener_prune(self, slot, who);           /* drop what is invalid already */
    begin = MEM32(self + 0x78);
    end = MEM32(self + 0x7C);
    idx = MEM32(self + 0x94);
    n = (listener_ram(begin) && end >= begin) ? (end - begin) / 12 : 0;
    if (n > LISTEN_MAX)
        n = LISTEN_MAX;
    for (i = 0; i < n; i++) {
        uint32_t e = begin + 12 * i;
        snap[i].id = MEM16(e); snap[i].key = MEM32(e + 4); snap[i].obj = MEM32(e + 8);
    }
    if (idx != 0xFFFFFFFFu) {
        if (idx < n) {
            cur_id = snap[idx].id; cur_key = snap[idx].key;
            if (listener_valid(snap[idx].obj, slot))
                listener_call(snap[idx].obj, slot, arg);
        } else if (changed_logged++ < 20) {
            fprintf(stderr, "[LISTEN] %s: current entry %u of %08X is past the list (%u entries): skipped\n",
                    who, idx, self, n);
        }
    }
    for (i = 0; i < n; i++) {
        if (snap[i].id == cur_id && snap[i].key == cur_key)
            continue;                          /* the current one: done above */
        if (!listener_live(self, snap[i].obj, snap[i].id, snap[i].key)) {
            if (changed_logged++ < 20)
                fprintf(stderr, "[LISTEN] %s: entry %u of %08X (object %08X) left the list during "
                                "the broadcast: not called\n", who, i, self, snap[i].obj);
            continue;
        }
        if (listener_valid(snap[i].obj, slot))
            listener_call(snap[i].obj, slot, arg);
    }
    /* Listeners added during the broadcast hear it too, as the original
     * loop reached them (it runs to the list's live end): starting a race
     * registers the race's own listeners from a callback, and without this
     * the race stopped at "PREPARE TO RACE" (2026-10-21). Rounds until no
     * new entry appears, each called once. */
    {
        int round;
        for (round = 0; round < 8 && n < LISTEN_MAX; round++) {
            uint32_t e, b = MEM32(self + 0x78), en = MEM32(self + 0x7C), added = 0;
            if (!listener_ram(b) || en < b || en - b > 12u * 4096)
                break;
            for (e = b; e + 12 <= en && n < LISTEN_MAX; e += 12) {
                uint16_t id = MEM16(e);
                uint32_t key = MEM32(e + 4), obj = MEM32(e + 8), k;
                int seen = id == cur_id && key == cur_key;
                for (k = 0; k < n && !seen; k++)
                    seen = snap[k].obj == obj && snap[k].id == id && snap[k].key == key;
                if (seen)
                    continue;
                snap[n].id = id; snap[n].key = key; snap[n].obj = obj;
                n++;
                added++;
                if (listener_valid(obj, slot))
                    listener_call(obj, slot, arg);
            }
            if (!added)
                break;
        }
    }
    {
        static int trace = -1;
        static long count;
        if (trace < 0) {
            const char *e = getenv("MC3_LISTEN_TRACE");
            trace = e && e[0] == '1';
        }
        if (trace && count++ < 200)
            fprintf(stderr, "[LISTEN] %s %08X(arg %u): %u listener(s), current %d, list now %u\n",
                    who, self, arg, n, (int)idx,
                    (MEM32(self + 0x7C) - MEM32(self + 0x78)) / 12);
    }
    g_ebx = saved_ebx; g_esi = saved_esi; g_edi = saved_edi; g_ebp = saved_ebp;
    g_esp += 8;                                /* ret 4: return address and the argument */
}

static int listener_fix_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_LISTENER_CHECK");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* Written out, not by macro: tools/recomp/manual_scan.py reads the text. */
extern void sub_002793C0_gen(void);
void sub_002793C0(void)
{
    if (!listener_fix_on()) { sub_002793C0_gen(); return; }
    listener_broadcast(g_ecx, 0x28, "race result");
}

extern void sub_00279430_gen(void);
void sub_00279430(void)
{
    if (!listener_fix_on()) { sub_00279430_gen(); return; }
    listener_broadcast(g_ecx, 0x2C, "race event");
}

/* ── Remix bring-up: the table parser that faults (MC3_TABLE_TRACE=1) ──
 *
 * First boot (2026-10-24) faulted in sub_00293B50 -- the same code as the
 * DUB Edition's sub_00269CD0 -- on a row pointer 0xFA4F0322 while the
 * game read its assets. It looks a table up (sub_00330400) and walks its
 * rows as pairs of string pointers. Logs each call's this and arguments
 * and the bytes they point to, so the input can be compared with the DUB
 * Edition's. */
static void table_dump(const char *what, uint32_t p)
{
    char hex[3 * 32 + 1], txt[33];
    int i;
    if (!((p >= 0x00010000u && p < 0x04000000u) || (p >= 0x80000000u && p < MC3_CONTIG_END))) {
        fprintf(stderr, "        %s %08X (not a pointer)\n", what, p);
        return;
    }
    for (i = 0; i < 32; i++) {
        uint8_t b = MEM8(p + (uint32_t)i);
        snprintf(hex + 3 * i, 4, "%02X ", b);
        txt[i] = (b >= 32 && b < 127) ? (char)b : '.';
    }
    txt[32] = 0;
    fprintf(stderr, "        %s %08X: %s|%s|\n", what, p, hex, txt);
}
extern void sub_00293B50_gen(void);
void sub_00293B50(void)
{
    static int on = -1, n;
    if (on < 0) {
        const char *e = getenv("MC3_TABLE_TRACE");
        on = e && e[0] == '1';
    }
    if (on && n++ < 40) {
        uint32_t i;
        fprintf(stderr, "[TABLE] #%d this %08X, args %08X %08X %08X %08X, from %08X\n", n, g_ecx,
                MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16), MEM32(g_esp));
        table_dump("this", g_ecx);
        for (i = 1; i <= 4; i++) {
            char what[8];
            snprintf(what, sizeof what, "arg%u", i);
            table_dump(what, MEM32(g_esp + 4 * i));
        }
    }
    sub_00293B50_gen();
}

/* The table loader (sub_00330400, the DUB Edition's sub_003000A0): this =
 * the table object; afterwards +0x10 holds the cell pointers (row-major),
 * +0x0C (16 bits) the columns per row and +0x18 the row count, as
 * sub_00293B50 reads them. With MC3_TABLE_TRACE=1: the shape, the first
 * rows, and the first cell that is not a pointer into memory. */
static int table_ptr_ok(uint32_t p)
{
    return (p >= 0x00010000u && p < 0x04000000u) || (p >= 0x80000000u && p < MC3_CONTIG_END);
}
static void table_str(char *out, size_t cap, uint32_t p)
{
    size_t i;
    if (!table_ptr_ok(p)) { snprintf(out, cap, "<%08X>", p); return; }
    for (i = 0; i + 1 < cap && MEM8(p + (uint32_t)i); i++) {
        uint8_t b = MEM8(p + (uint32_t)i);
        out[i] = (b >= 32 && b < 127) ? (char)b : '?';
    }
    out[i] = 0;
}
extern void sub_00330400_gen(void);
void sub_00330400(void)
{
    static int on = -1, n;
    uint32_t obj = g_ecx;
    if (on < 0) {
        const char *e = getenv("MC3_TABLE_TRACE");
        on = e && e[0] == '1';
    }
    sub_00330400_gen();
    if (on && n++ < 40) {
        uint32_t cells = MEM32(obj + 0x10), cols = MEM16(obj + 0x0C), rows = MEM32(obj + 0x18), r, c;
        char a[40], b[40];
        fprintf(stderr, "[TABLE] load #%d -> %u: obj %08X cells %08X, %u columns x %u rows\n",
                n, g_eax & 0xFF, obj, cells, cols, rows);
        if (!table_ptr_ok(cells) || cols == 0 || rows > 100000)
            return;
        for (r = 0; r < rows && r < 3; r++) {
            table_str(a, sizeof a, MEM32(cells + 4 * (r * cols)));
            table_str(b, sizeof b, MEM32(cells + 4 * (r * cols + 1)));
            fprintf(stderr, "        row %u: \"%s\" \"%s\" ...\n", r, a, b);
        }
        for (r = 0; r < rows; r++)
            for (c = 0; c < cols; c++) {
                uint32_t p = MEM32(cells + 4 * (r * cols + c));
                if (p && !table_ptr_ok(p)) {                /* 0: an empty field */
                    table_str(a, sizeof a, r ? MEM32(cells + 4 * ((r - 1) * cols)) : 0);
                    fprintf(stderr, "        first bad cell: row %u column %u = %08X (row before starts \"%s\")\n",
                            r, c, p, a);
                    return;
                }
            }
        fprintf(stderr, "        every cell a pointer\n");
    }
}

/* ── The game's state machine, traced (MC3_STATE_TRACE=1) ──
 *
 * The object behind the main loop's dispatch (DUB 0x0051886C, Remix
 * 0x00557ED0) keeps the state at +4 (1..7, the main loop's switch); its
 * vtable +4 (sub_00020E40) requests a screen, +8 (sub_00020EC0) runs a pass and
 * applies requests. Logs every request with its caller and every change of
 * state -- the DUB Edition goes to state 4 for the boot movies, the Remix
 * stayed in 1 (2026-10-24). */
static int state_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_STATE_TRACE");
        on = e && e[0] == '1';
    }
    return on;
}
static int roam_room(void);                     /* free roam: the city is the room (roam_game.c) */
static void online_car_picked(void);            /* the Online profile keeps the car picked (below) */
static void roam_race_over(uint32_t esp);       /* free roam: which way RACE OVER came (roam_cars.c) */
static void rra_state_request(uint32_t esp);    /* free roam: an AI race's way back (roam_race_ai.c) */
static void fr_no_opponents(void);              /* free roam: the cruise without AI racers (below) */
extern void sub_00020E40_gen(void);
void sub_00020E40(void)
{
    if (state_trace_on())
        fprintf(stderr, "[STATE] request %u (state %u) from %08X\n", MEM32(g_esp + 4),
                MEM32(g_ecx + 4), MEM32(g_esp));
    rra_state_request(g_esp);                    /* may turn the front end into the cruise (request 6) */
    roam_race_over(g_esp);
    roam_room();                                 /* the city picked (free roam room), before the load */
    online_car_picked();                         /* a car picked on the setup screen, before the load */
    fr_no_opponents();
    sub_00020E40_gen();
}
static uint32_t s_state_obj;                    /* for MC3_AUTOSTART (sub_0020B6F0) */

/* What the player is doing, for the page's "where" message (web/index.html
 * lanWhere -> the Worker's players list). Recomputed at each state pass
 * (sub_00020EC0, every frame) from the state at obj+4 and the game mode
 * [0x53F2D4] (docs/roam-races.md 1.1, 1.2); the page reads s_activity through
 * mc3_activity_box (web/pre.js Module.mc3Activity):
 *   0 loading  state 0/1/4 (boot, the intro movies); 0 also before the first pass
 *   1 menu     state 7 (front end: title, main menu, setup screens)
 *   2 career   states 2/3/6 (level, pause, reload) with mode 2 (Career and Tokyo)
 *   3 arcade   states 2/3/6 with any other mode (Arcade races; Online's Free Roam
 *              cruise too, mode 0: the server's hub says Free Roam first)
 * State 5 (garage) keeps the activity it was entered from when that was career
 * or arcade (the career's garage), else it is a menu (the main menu's Garage).
 * Unknown states: unchanged. */
enum { ACT_LOADING, ACT_MENU, ACT_CAREER, ACT_ARCADE };
static volatile int32_t s_activity;
static void activity_update(uint32_t obj)
{
    static int32_t last;                        /* the last activity outside the garage */
    uint32_t st = MEM32(obj + 4);
    int32_t a;
    switch (st) {
    case 0: case 1: case 4: a = ACT_LOADING; break;
    case 7: a = ACT_MENU; break;
    case 2: case 3: case 6: a = MEM32(0x0053F2D4u) == 2u ? ACT_CAREER : ACT_ARCADE; break;
    case 5: a = last == ACT_CAREER || last == ACT_ARCADE ? last : ACT_MENU; s_activity = a; return;
    default: return;
    }
    s_activity = last = a;
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE uintptr_t mc3_activity_box(void) { return (uintptr_t)&s_activity; }
#endif
static void online_garage_left(void);           /* the Online profile is saved (below) */
static void roam_pool_pass(uint32_t obj, int after); /* free roam: remote cars at state requests (roam_pool.c) */
#ifdef __EMSCRIPTEN__
static void fr_direct_pass(uint32_t obj);       /* Online: the deferred cruise load (below) */
#endif
extern void sub_00020EC0_gen(void);
void sub_00020EC0(void)
{
    uint32_t obj = g_ecx, before = MEM32(obj + 4);
    s_state_obj = obj;
#ifdef __EMSCRIPTEN__
    fr_direct_pass(obj);                        /* Online's Request(6), outside the menu's Flash dispatch */
#endif
    roam_pool_pass(obj, 0);                     /* standalone cars out before a cruise teardown (roam_pool.c) */
    sub_00020EC0_gen();
    roam_pool_pass(obj, 1);
    activity_update(obj);
    if (state_trace_on() && MEM32(obj + 4) != before)
        fprintf(stderr, "[STATE] %u -> %u\n", before, MEM32(obj + 4));
    if (before == 5u && MEM32(obj + 4) != 5u)  /* the garage (state 5) is left */
        online_garage_left();
}

/* ── Remix bring-up: callee-saved registers on the movie path (MC3_ABI_CHECK=1) ──
 *
 * The state machine's "enter state 4" (sub_00021D30) stores the state
 * through esi after sub_00021390 returns, and the Remix never left state 1
 * (2026-10-24): something on the path to the first movie returns with a
 * callee-saved register changed. Each wrapper below compares ebx, esi,
 * edi and ebp before and after and says which function broke them, once. */
static void abi_call(void (*fn)(void), const char *name)
{
    static int on = -1;
    uint32_t b = g_ebx, s = g_esi, d = g_edi, p = g_ebp;
    if (on < 0) {
        const char *e = getenv("MC3_ABI_CHECK");
        on = e && e[0] == '1';
    }
    fn();
    (void)p;                     /* ebp: frameless lifted code does not write it back */
    if (on && (g_ebx != b || g_esi != s || g_edi != d)) {
        static int said;
        if (said++ < 20)
            fprintf(stderr, "[ABI] %s changed%s%s%s (esi %08X -> %08X, edi %08X -> %08X)\n", name,
                    g_ebx != b ? " ebx" : "", g_esi != s ? " esi" : "", g_edi != d ? " edi" : "",
                    s, g_esi, d, g_edi);
    }
}
extern void sub_00021390_gen(void);
void sub_00021390(void)
{
    uint32_t obj = g_ecx;
    if (state_trace_on())
        fprintf(stderr, "[STATE] sub_00021390 enter: this %08X state %u, from %08X\n",
                obj, MEM32(obj + 4), MEM32(g_esp));
    abi_call(sub_00021390_gen, "sub_00021390");
    if (state_trace_on())
        fprintf(stderr, "[STATE] sub_00021390 leave: esi %08X edi %08X, state %u\n",
                g_esi, g_edi, MEM32(obj + 4));
}
extern void sub_00029020_gen(void);
void sub_00029020(void) { abi_call(sub_00029020_gen, "sub_00029020"); }
extern void sub_0015AA30_gen(void);
void sub_0015AA30(void) { abi_call(sub_0015AA30_gen, "sub_0015AA30"); }
extern void sub_003639C0_gen(void);
void sub_003639C0(void) { abi_call(sub_003639C0_gen, "sub_003639C0"); }
extern void sub_003638A0_gen(void);
void sub_003638A0(void) { abi_call(sub_003638A0_gen, "sub_003638A0"); }
extern void sub_002D1610_gen(void);
void sub_002D1610(void) { abi_call(sub_002D1610_gen, "sub_002D1610"); }
extern void sub_002DA8A0_gen(void);
void sub_002DA8A0(void) { abi_call(sub_002DA8A0_gen, "sub_002DA8A0"); }
extern void sub_002D5F30_gen(void);
void sub_002D5F30(void) { abi_call(sub_002D5F30_gen, "sub_002D5F30"); }
extern void sub_000CE440_gen(void);
void sub_000CE440(void) { abi_call(sub_000CE440_gen, "sub_000CE440"); }
extern void sub_00417BC0_gen(void);
void sub_00417BC0(void) { abi_call(sub_00417BC0_gen, "sub_00417BC0"); }
extern void sub_003AD0F0_gen(void);
void sub_003AD0F0(void) { abi_call(sub_003AD0F0_gen, "sub_003AD0F0"); }
extern void sub_002DBBC0_gen(void);
void sub_002DBBC0(void) { abi_call(sub_002DBBC0_gen, "sub_002DBBC0"); }

/* ── Real-time steps below 30 fps ──
 *
 * The game measures each frame with the CPU's cycle counter (sub_0020B6F0:
 * rdtsc at 733 MHz) and clamps the step to [0x4B2D60, 0x4B2D5C]. The race
 * setup (sub_00020880) sets the maximum to 1 / its rate -- 1/30 s -- so on
 * the Xbox, which held 30 fps, nothing moved slower than real time. On the
 * Switch the Remix drops below 30 in rain, with nitrous and in dense parts of
 * the city, and every such frame advanced the world by only 1/30 s: slow
 * motion, the car "slowing down" (2026-10-30; the perf report's game frame
 * time stayed 0.0333 at 19 fps). The engine's own fallback without a rate is
 * 0.1 s, so steps up to MC3_MAX_DT (default 0.0667, 15 fps) keep the race at
 * its real speed with fewer frames. MC3_MAX_DT=0: the game's own cap. */
extern void sub_00020880_gen(void);
void sub_00020880(void)
{
    static float cap = -1.0f;
    static int logged;
    sub_00020880_gen();
    if (cap < 0.0f) {
        const char *e = getenv("MC3_MAX_DT");
        cap = e ? (float)atof(e) : 0.0667f;
        if (cap > 0.1f) cap = 0.1f;            /* the engine's own limit */
    }
    if (cap > 0.0f && MEMF(0x004B2D5Cu) < cap) {
        if (logged++ < 4)
            fprintf(stderr, "[TIME] frame step capped at %.4f s by the game; %.4f s here (MC3_MAX_DT)\n",
                    MEMF(0x004B2D5Cu), cap);
        MEMF(0x004B2D5Cu) = cap;
    }
}

/* MC3_TIME_TRACE=1: the game's frame step (sub_0020B6F0 -> 0x4B2D70), once
 * a second: steps taken, their shortest / mean / longest, the clamp
 * [0x4B2D60, 0x4B2D5C] and the mode flags that pick how the step is made
 * (0x534774-76 snapping and fixed steps, 0x534AC4 / 0x534A9C replay). */
/* MC3_REALTIME=1 (off by default): in a race the game runs fixed steps --
 * 0x534770 set, the step 0x4B2D58 = 1/30 s written whatever the frame took
 * (0x0020B942) -- so below 30 fps the race is slow motion (trace above,
 * 2026-11-02: 0.0167 s a step at 4 fps in the flythrough, 0.0333 in races
 * at 18 fps). With it, a race frame that took longer advances by the real
 * time, up to MC3_MAX_DT (0.0667 s), through the same variables the fixed
 * path writes: 0x4B2D70 (step), 0x4B2D4C (scaled), 0x53475C (game time),
 * 0x4B2D48 (clamped) and 0x4B2D50 (its reciprocal). Only steps of 1/30
 * (races); the title flythrough's 1/60 half-speed showcase is left alone.
 * Replays recorded this way may play back slightly differently. Under
 * MC3_60FPS the race's 1/60 steps are stretched the same way. */
static void realtime_step(float real)
{
    static int on = -1;
    static float cap;
    float f = MEMF(0x004B2D58u), scale = MEMF(0x004B2D6Cu), d, cl;
    if (on < 0) {
        const char *e = getenv("MC3_REALTIME"), *c = getenv("MC3_MAX_DT");
        on = e && e[0] == '1';
        cap = c && atof(c) > 0.0 ? (float)atof(c) : 0.0667f;
        if (cap > 0.1f) cap = 0.1f;
        if (on)
            fprintf(stderr, "[TIME] races advance in real time below 30 fps, steps up to %.4f s"
                    " (MC3_REALTIME)\n", cap);
    }
    if (!on || MEM32(0x00534770u) == 0 || f < (s_fps60 ? 0.015f : 0.030f) || f > 0.037f
        || real <= f * 1.1f)
        return;
    d = real < cap ? real : cap;
    MEMF(0x004B2D70u) = d;
    MEMF(0x0053475Cu) += (d - f) * scale;           /* game time: the real step, not f */
    MEMF(0x004B2D4Cu) = d * scale;
    cl = d * scale;
    if (cl < MEMF(0x004B2D60u)) cl = MEMF(0x004B2D60u);
    if (cl > MEMF(0x004B2D5Cu)) cl = MEMF(0x004B2D5Cu);
    MEMF(0x004B2D48u) = cl;
    MEMF(0x004B2D50u) = 1.0f / cl;
}

/* MC3_60FPS=1 (off by default: the renderer can't hold 60 yet): races and
 * cruise at 60 game frames a second. The race setup (sub_00020880 ->
 * sub_0020BB00(rate, n)) leaves the fixed step 0x4B2D58 = 1/(rate*n) =
 * 1/30 with 0x534770 = 1; in states 2/3 (race, cruise) it is halved to 1/60
 * before the step runs (the step recomputes 0x4B2D48/50 from it, and its
 * clamp [0x4B2D60, 0x4B2D5C] = [0.0001, 1/rate or 0.1] lets 1/60 through),
 * and sub_0041B550 presents at interval 1. Leaving states 2/3 puts 1/30
 * back. The title flythrough (1/60, not in states 2/3) and the variable-step
 * mode (0x534770 = 0, measured time) are left alone. Untested risks: code
 * that moves a fixed amount per frame rather than per step (lerps, camera
 * smoothing, Flash HUD) runs 2x fast; replays and ghosts recorded at 1/30
 * may play back differently; System Link peers must both run the same mode;
 * the race object's +0x58 rate (60/30/20 for sub_0009BE00) is not changed. */
/* The page's in-game Settings (web/index.html setSetting -> web/pre.js
 * mc3SetLive -> mc3_live_box): word i, 0 = the environment's value as at
 * start. MC3_LIVE_60FPS 1 off / 2 on (fps60_step, from the next drive or
 * race); MC3_LIVE_CARS MC3_ROAM_CARS, -1 auto (roam_cars_total, read live by
 * the remote cars pool); MC3_LIVE_DRIFT MC3_DRIFT 1 off / 2 on / 3 strong (drift.c). */
enum { MC3_LIVE_60FPS, MC3_LIVE_CARS, MC3_LIVE_DRIFT, MC3_LIVE_N };
#ifdef __EMSCRIPTEN__
static int32_t s_mc3_live[MC3_LIVE_N];
EMSCRIPTEN_KEEPALIVE uintptr_t mc3_live_box(void) { return (uintptr_t)s_mc3_live; }
#endif
static int32_t mc3_live(int i)
{
#ifdef __EMSCRIPTEN__
    return __atomic_load_n(&s_mc3_live[i], __ATOMIC_RELAXED);
#else
    (void)i;
    return 0;
#endif
}

static void fps60_step(void)
{
    static int on = -1, env = -1;
    static float halved;                        /* the 1/60 we wrote, 0 = none */
    uint32_t st = s_state_obj ? MEM32(s_state_obj + 4) : 0;
    float f = MEMF(0x004B2D58u);
    int32_t l = mc3_live(MC3_LIVE_60FPS);
    if (env < 0) {
        const char *e = getenv("MC3_60FPS");
        env = e && e[0] == '1';
    }
    /* a live change applies at once, mid-drive too (the step is the only thing it switches; was next drive only:
     * Free Roam never leaves states 2/3, so it stayed at 30) */
    if (on < 0 || (l ? l == 2 : env) != on) {
        on = l ? l == 2 : env;
        if (!on && halved != 0.0f) {            /* off: the 1/30 back, as on leaving states 2/3 */
            if (f == halved)
                MEMF(0x004B2D58u) = f * 2.0f;
            halved = 0.0f;
            s_fps60 = 0;
        }
        if (on || l)
            fprintf(stderr, "[TIME] races and cruise step 1/%d s at interval %d (MC3_60FPS%s)\n", on ? 60 : 30,
                    on ? 1 : 2, l ? ", in-game Settings" : "");
    }
    if (!on)
        return;
    if ((st == 2 || st == 3) && MEM32(0x00534770u) != 0) {
        if (f >= 0.030f && f <= 0.037f)
            MEMF(0x004B2D58u) = halved = f * 0.5f;
    } else if (halved != 0.0f) {
        if (f == halved)
            MEMF(0x004B2D58u) = f * 2.0f;
        halved = 0.0f;
    }
    s_fps60 = halved != 0.0f && MEMF(0x004B2D58u) == halved;
}

/* Drift mode (MC3_DRIFT, live): the arcade drift assist on our own car, drift_step */
#include "drift.c"

/* ── Traffic LOD: wheels and shadow pop in at 75 m ──
 *
 * Found on the PS2 Remix by The_Hexagone (Reddit r/midnightclub), same code
 * here. The ambient (traffic) car update sub_00073FF0 compares the camera
 * distance of the viewport (+0x144 + 4*view) with the float at 0x4AFD1C
 * (75.0, .data; the file image holds 75.0 too and sub_00073310, run from
 * sub_0006C5D0, resets it): beyond it the car takes the far path
 * (loc_00074D1A), whose draw leaves out the wheels and the drop shadow, so
 * the wheels pop in at close range. Nothing else writes the float and
 * sub_00073310 can run at each load, so it is written every frame, before
 * the frame step. MC3_TRAFFIC_WHEEL_DIST in metres, default 150; 75 = the
 * game's own. Neighbours (not changed): 0x4AFD18 = 250.0 (far fade, set
 * from 0x4AFCD0), 0x4AFD20 = 60.0 (threshold in sub_00073FF0/00074E60/
 * 00075104), 0x4B0184 = 75.0 (dynamic-light radius, sub_000952C0). */
static void traffic_lod_step(void)
{
    static float dist = -1.0f;
    if (dist < 0.0f) {
        const char *e = getenv("MC3_TRAFFIC_WHEEL_DIST");
        dist = e && atof(e) > 0.0 ? (float)atof(e) : 150.0f;
        fprintf(stderr, "[LOD] traffic wheels drawn to %g m\n", dist);
    }
    MEMF(0x004AFD1Cu) = dist;
}

static void online_strings(void);               /* Online (O1), at the end of this file */
extern void sub_0020B6F0_gen(void);
void sub_0020B6F0(void)
{
    static int on = -1;
    static unsigned n;
    static float lo = 1e9f, hi = 0.0f, sum = 0.0f;
    static uint32_t last_ms;
    static uint64_t last_ns;
    float dt, real;
    if (on < 0) {
        const char *e = getenv("MC3_TIME_TRACE");
        on = e && e[0] == '1';
    }
    {
        struct timespec ts;
        uint64_t now;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        real = last_ns ? (float)((now - last_ns) / 1e9) : 0.0f;
        last_ns = now;
    }
    fps60_step();
    drift_step();
    traffic_lod_step();
    online_strings();
    roam_room();
    sub_0020B6F0_gen();
    realtime_step(real);
    if (s_state_obj) {                          /* states 2 and 3: a race or cruise (sub_0001EDB0) */
        uint32_t st = MEM32(s_state_obj + 4);
        mc3_autostart_frame(st == 2 || st == 3, 2);   /* Career, Tokyo Challenge, Arcade */
    }
    if (!on)
        return;
    dt = MEMF(0x004B2D70u);
    n++; sum += dt;
    if (dt < lo) lo = dt;
    if (dt > hi) hi = dt;
    {
        struct timespec ts;
        uint32_t ms;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ms = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
        if (ms - last_ms >= 1000u) {
            fprintf(stderr, "[TIME] %u steps/s, step %.4f .. %.4f (mean %.4f) s; fixed step %.4f;"
                    " clamp %.4f .. %.4f; flags snap %u fixed %u/%u/%08X replay %08X/%u\n",
                    n, lo, hi, n ? sum / n : 0.0f, MEMF(0x004B2D58u), MEMF(0x004B2D60u), MEMF(0x004B2D5Cu),
                    MEM8(0x00534776u), MEM8(0x00534774u), MEM8(0x00534775u), MEM32(0x00534770u),
                    MEM32(0x00534AC4u), MEM8(0x00534A9Cu));
            n = 0; sum = 0.0f; lo = 1e9f; hi = 0.0f;
            last_ms = ms;
        }
    }
}

/* ── DirectSound: stopping a voice while the APU waits for us ──
 *
 * sub_003C77C0 waits for a voice to stop: while the voice's flags (+0x12)
 * have 0x8000, sub_003C77C0 calls sub_003C6B43, which reads the system time
 * and, 500 ms after the stop was asked for (+0x78), stops the voice itself
 * (sub_003C6A5A). The APU says "stopped" through its front end; when the
 * front end is trapped, it waits for DirectSound's interrupt handler, which
 * runs on the same emulated interrupt thread that is in this loop -- on the
 * Xbox the interrupt would pre-empt it. So the loop ran out its 500 ms
 * holding the guest lock: the main thread stood still for 499 ms
 * ([gil] ... held by thread 1007 (timer)), the hangs in the DUB Edition's
 * races (Switch, 2026-10-27); the Remix's DirectSound is the same code. Now the loop is the
 * same, except that once the front end has been trapped for
 * MC3_VOICE_STOP_MS (1) of it, the voice is stopped as DirectSound does
 * at its timeout. MC3_VOICE_STOP_MS=0: the original loop. */
extern int mcpx_apu_fe_trapped(void);
extern void sub_003C77C0_gen(void);
extern void sub_003C6B43(void);
extern void sub_003C6A5A(void);
static uint64_t voice_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void voice_call(void (*fn)(void), uint32_t voice)
{
    g_esp -= 4; MEM32(g_esp) = 0;          /* return address; thiscall, no arguments */
    g_ecx = voice;
    fn();                                  /* ret: pops it */
}
void sub_003C77C0(void)
{
    static int ms = -1, logged;
    uint32_t v = g_ecx;
    uint64_t trapped_since = 0;
    int forced = 0;
    if (ms < 0) {
        const char *e = getenv("MC3_VOICE_STOP_MS");
        ms = e ? atoi(e) : 1;
    }
    if (ms <= 0) {
        sub_003C77C0_gen();
        return;
    }
    if (MEM8(v + 0x12) & 1) {
        while (MEM16(v + 0x12) & 0x8000) {
            if (!forced && (MEM16(v + 0x12) & 0x8001) == 0x8001 && mcpx_apu_fe_trapped()) {
                uint64_t now = voice_ms_now();
                if (!trapped_since)
                    trapped_since = now;
                if (now - trapped_since >= (uint64_t)ms) {
                    if (logged++ < 10)
                        fprintf(stderr, "[DSOUND] voice %08X: stop waited %u ms on a trapped APU"
                                " front end -- stopped as at DirectSound's timeout\n", v,
                                (unsigned)(now - trapped_since));
                    voice_call(sub_003C6A5A, v);
                    forced = 1;
                    continue;
                }
            } else {
                trapped_since = 0;
            }
            voice_call(sub_003C6B43, v);
        }
    }
    g_ecx = v;
    g_esp += 4;                            /* ret */
}

/* ── DirectSound's DSP command wait: its doorbell found at run time ──
 *
 * sub_003C3D23 (DSOUND) hands the audio DSP a command block and waits for the
 * DSP to clear the command word at block + 0x810. No DSP is emulated: the
 * runtime answers words registered with it (apu_dsp_ack_add). The block is
 * [[[this+8]+0x10]], the object's command buffer, and moves with the
 * title's allocations -- the Remix's sat at 0x83B10000 when told the TV
 * is widescreen and at 0x83A5C000 when not (2026-10-25), so a fixed
 * address hung one of the two and zeroed a stray word in the other. */
extern void apu_dsp_ack_add(uint32_t va);
extern void sub_003C3D23_gen(void);
void sub_003C3D23(void)
{
    uint32_t dev = MEM32(g_ecx + 8);
    uint32_t buf = dev ? MEM32(dev + 0x10) : 0;
    uint32_t block = buf ? MEM32(buf) : 0;
    if (block)
        apu_dsp_ack_add(block + 0x810);
    sub_003C3D23_gen();
}

/* ── System Link: the XNet API answered by the runtime (MC3_NETPLAY=1) ──
 *
 * MC3 plays System Link over plain Winsock datagrams: UDP :1000 with
 * SO_BROADCAST (discovery to 255.255.255.255:1000) and VDP :1001, both
 * non-blocking (sub_0005D2C0); peers by XNADDR -> XNetXnAddrToInAddr. The
 * statically linked XNET library (CXn at [0x0055FDAC], its NIC driver,
 * secure key exchange) cannot run: no NIC interrupts. So its exported
 * thunks (`mov ecx, [0x55FDAC]; jmp CXn::method`, XNET section) are
 * answered by xboxrecomp/src/net/xnet_hle.c over host UDP sockets (the web
 * build plugs in its own transport). XNetStartup / WSAStartup never create
 * CXn, so the Live-only thunks left lifted (XNetQosLookup, ...) keep
 * returning WSANOTINITIALISED, and RECOMP_NET_LINK=0 keeps the NIC idle.
 * Addresses read by disassembly of the USA XBE (2026-11-05; EUR = USA code).
 * Off by default: each override runs the lifted thunk unless MC3_NETPLAY=1.
 * Native transport: RECOMP_NET_PORT_BASE / _ADDR / _PEERS (xnet_hle.c). */
#include "net/xnet_hle.h"
#include "net/roam/roam_api.h"                  /* free roam (O3): roam_game.c */

static int netplay_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("MC3_NETPLAY");
        on = e && *e == '1';
    }
    return on;
}

#define NET_ARG(i) MEM32(g_esp + 4u * (i))
static uint8_t *net_ptr(uint32_t va) { return va ? (uint8_t *)XBOX_PTR(va) : NULL; }
/* stdcall return: result in eax, pop the return address and the arguments */
static void net_ret(uint32_t eax, uint32_t nargs) { g_eax = eax; g_esp += 4u + 4u * nargs; }

/* XNetGetEthernetLinkStatus (XAPI, .text): the System Link menu is gated on it */
extern void sub_002E9B40_gen(void);
void sub_002E9B40(void)
{
    if (!netplay_on()) { sub_002E9B40_gen(); return; }
    net_ret(xnet_link_status(), 0);
}

/* XNetStartup(params) */
extern void sub_003CC350_gen(void);
void sub_003CC350(void)
{
    if (!netplay_on()) { sub_003CC350_gen(); return; }
    net_ret((uint32_t)xnet_startup(), 1);
}

/* WSAStartup(wVersionRequested, lpWSAData). wVersion is the version asked
 * for, up to 2.2 (Winsock's rule). MC3's network init (sub_001A2870) asks
 * for 2.0 and quits unless it gets exactly 2.0 (`cmp al, 2; jne; test ah,
 * ah; je`): "InitWinSock - Couldn't find a usable WinSock DLL", then exit()
 * -- the browser's hang after the Rockstar logos with MC3_NETPLAY=1. */
extern void sub_003CC367_gen(void);
void sub_003CC367(void)
{
    uint8_t *wsa;
    uint32_t want = NET_ARG(1);
    uint8_t major = want & 0xFF, minor = (want >> 8) & 0xFF;
    int r;
    if (!netplay_on()) { sub_003CC367_gen(); return; }
    if (major > 2 || (major == 2 && minor > 2))
        major = 2, minor = 2;
    wsa = net_ptr(NET_ARG(2));
    r = xnet_startup();
    if (!r && wsa) {
        wsa[0] = major; wsa[1] = minor;   /* wVersion */
        wsa[2] = 2; wsa[3] = 2;           /* wHighVersion */
    }
    net_ret((uint32_t)(wsa ? r : XNET_WSAEFAULT), 2);
}

/* WSACleanup() (CXn cleanup with fWSA = 1) */
extern void sub_003CC455_gen(void);
void sub_003CC455(void)
{
    if (!netplay_on()) { sub_003CC455_gen(); return; }
    net_ret(0, 0);
}

/* WSAGetLastError() (the thunk jumps to XAPI's GetLastError) */
extern void sub_003CC4E5_gen(void);
void sub_003CC4E5(void)
{
    if (!netplay_on()) { sub_003CC4E5_gen(); return; }
    net_ret((uint32_t)xnet_last_error(), 0);
}

/* XNetRandom(pb, cb) */
extern void sub_003CBF8F_gen(void);
void sub_003CBF8F(void)
{
    uint8_t *p;
    if (!netplay_on()) { sub_003CBF8F_gen(); return; }
    p = net_ptr(NET_ARG(1));
    if (p) xnet_random(p, NET_ARG(2));
    net_ret(0, 2);
}

/* XNetCreateKey(pxnkid, pxnkey) */
extern void sub_003CBF9A_gen(void);
void sub_003CBF9A(void)
{
    uint8_t *kid, *key;
    if (!netplay_on()) { sub_003CBF9A_gen(); return; }
    kid = net_ptr(NET_ARG(1));
    key = net_ptr(NET_ARG(2));
    if (kid && key) xnet_create_key(kid, key);
    net_ret(0, 2);
}

/* XNetRegisterKey(pxnkid, pxnkey): no security, nothing to register */
extern void sub_003CBFA5_gen(void);
void sub_003CBFA5(void)
{
    if (!netplay_on()) { sub_003CBFA5_gen(); return; }
    net_ret(0, 2);
}

/* XNetUnregisterKey(pxnkid) */
extern void sub_003CBFB0_gen(void);
void sub_003CBFB0(void)
{
    if (!netplay_on()) { sub_003CBFB0_gen(); return; }
    net_ret(0, 1);
}

/* XNetXnAddrToInAddr(pxna, pxnkid, pina): the peer's plain address, xna.ina */
extern void sub_003CBFBB_gen(void);
void sub_003CBFBB(void)
{
    uint32_t xna, ina;
    if (!netplay_on()) { sub_003CBFBB_gen(); return; }
    xna = NET_ARG(1);
    ina = NET_ARG(3);
    if (!xna || !ina) { net_ret(XNET_WSAEFAULT, 3); return; }
    MEM32(ina) = MEM32(xna);
    net_ret(0, 3);
}

/* XNetQosListen(pxnkid, pb, cb, dwBitsPerSec, dwFlags): Live matchmaking only */
extern void sub_003CBFF4_gen(void);
void sub_003CBFF4(void)
{
    if (!netplay_on()) { sub_003CBFF4_gen(); return; }
    net_ret(0, 5);
}

/* XNetQosRelease(pxnqos) */
extern void sub_003CC01F_gen(void);
void sub_003CC01F(void)
{
    if (!netplay_on()) { sub_003CC01F_gen(); return; }
    net_ret(0, 1);
}

/* XNetGetTitleXnAddr(pxna) */
extern void sub_003CC02A_gen(void);
void sub_003CC02A(void)
{
    uint8_t *xna;
    if (!netplay_on()) { sub_003CC02A_gen(); return; }
    xna = net_ptr(NET_ARG(1));
    net_ret(xna ? xnet_get_title_xnaddr(xna) : 0x0001u, 1);
}

/* XNetGetConnectStatus(ina): asked after XNetXnAddrToInAddr (sub_0005C00F);
 * no key exchange, so always connected */
extern void sub_003CC13F_gen(void);
void sub_003CC13F(void)
{
    if (!netplay_on()) { sub_003CC13F_gen(); return; }
    net_ret(XNET_CONNECT_STATUS_CONNECTED, 1);
}

/* socket(af, type, protocol) */
extern void sub_003CC035_gen(void);
void sub_003CC035(void)
{
    if (!netplay_on()) { sub_003CC035_gen(); return; }
    net_ret(xnet_socket((int)NET_ARG(1), (int)NET_ARG(2), (int)NET_ARG(3)), 3);
}

/* closesocket(s) */
extern void sub_003CC040_gen(void);
void sub_003CC040(void)
{
    if (!netplay_on()) { sub_003CC040_gen(); return; }
    net_ret((uint32_t)xnet_closesocket(NET_ARG(1)), 1);
}

/* ioctlsocket(s, cmd, argp) */
extern void sub_003CC056_gen(void);
void sub_003CC056(void)
{
    if (!netplay_on()) { sub_003CC056_gen(); return; }
    net_ret((uint32_t)xnet_ioctlsocket(NET_ARG(1), NET_ARG(2), net_ptr(NET_ARG(3))), 3);
}

/* setsockopt(s, level, optname, optval, optlen) */
extern void sub_003CC061_gen(void);
void sub_003CC061(void)
{
    if (!netplay_on()) { sub_003CC061_gen(); return; }
    net_ret((uint32_t)xnet_setsockopt(NET_ARG(1), (int)NET_ARG(2), (int)NET_ARG(3),
                                      net_ptr(NET_ARG(4)), (int)NET_ARG(5)), 5);
}

/* bind(s, name, namelen) */
extern void sub_003CC070_gen(void);
void sub_003CC070(void)
{
    if (!netplay_on()) { sub_003CC070_gen(); return; }
    net_ret((uint32_t)xnet_bind(NET_ARG(1), net_ptr(NET_ARG(2)), (int)NET_ARG(3)), 3);
}

/* Free Roam detaches the game's System Link session (fr_sl_detached, below):
 * its datagrams are dropped both ways, so no peer joins or leaves it. */
static int fr_sl_detached(void);

/* recvfrom(s, buf, len, flags, from, fromlen) */
extern void sub_003CC0BE_gen(void);
void sub_003CC0BE(void)
{
    int r;
    if (!netplay_on()) { sub_003CC0BE_gen(); return; }
    do
        r = xnet_recvfrom(NET_ARG(1), net_ptr(NET_ARG(2)), (int)NET_ARG(3), (int)NET_ARG(4),
                          net_ptr(NET_ARG(5)), net_ptr(NET_ARG(6)));
    while (r >= 0 && fr_sl_detached());          /* detached: drained, ends as "would block" */
    net_ret((uint32_t)r, 6);
}

/* sendto(s, buf, len, flags, to, tolen) */
extern void sub_003CC0DC_gen(void);
void sub_003CC0DC(void)
{
    if (!netplay_on()) { sub_003CC0DC_gen(); return; }
    if (fr_sl_detached()) {                      /* "sent" */
        net_ret(NET_ARG(3), 6);
        return;
    }
    net_ret((uint32_t)xnet_sendto(NET_ARG(1), net_ptr(NET_ARG(2)), (int)NET_ARG(3), (int)NET_ARG(4),
                                  net_ptr(NET_ARG(5)), (int)NET_ARG(6)), 6);
}

/* ── Online (O1): the main menu's "Networking" becomes "Online" ──
 *
 * Single-player groundwork for our own online mode (the networking itself
 * comes later). Main menu (mcMenuModeSelect, built by sub_0018C770) item 3,
 * CM_Networking, called sub_0018C5A0: the Xbox Live / System Link screen
 * (FE [0x50F148] -> sub_00154B30(0x1E)). It now does what Arcade's item
 * does (sub_0018C530: [0x50F14C] = 0, sub_0013AB20, sub_0013A590, Go(0x19);
 * Cruise, 1 player are its defaults) inside an Online session:
 *   - money ([[0x557F0C]+0xC]+0xACC, the career manager's progress object)
 *     is 2,000,000 on entering and each time the garage opens (state 5,
 *     sub_00021A60), so the main menu's Garage buys and tunes with it;
 *   - nothing is saved: the profile writer sub_00022040 (creates the file,
 *     then sub_000193F0 writes it) returns "saved" without touching the
 *     disc. sub_0001A1F0 is not the writer: it reads the older .mc3 format
 *     (op 3 of sub_00022310, called while listing saves);
 *   - the session ends when Career, Tokyo or Arcade is picked: money and
 *     the garage (0x18340 bytes at 0x0053F308, saved and loaded raw, so no
 *     pointers to fix) go back to what they were, nothing leaks into
 *     career. A profile applied meanwhile (sub_00019560: the main menu's
 *     Garage does it; the main menu itself has none applied, money 0)
 *     does not end it: the loaded career is what goes back at the end;
 *   - every car, part, rim and paint is unlocked through the game's own
 *     unlock flags (0x0053F2EE cars, 0x0053F2F2 garage items; below), put
 *     back at the end.
 * The strings: once the current language's table is loaded, every entry's
 * UTF-16 text is rewritten in place, never longer (Xbox Live -> Online,
 * Xbox System Link -> LAN Play, ...), and CM_Networking reads "Online".
 * The table ([0x507E08 + 4*[0x507E08+0x28]]: bucket count, buckets of
 * {hash, entry, next}; entry+8 = text) is read here, not through
 * sub_000EA750, which adds an entry for a key it does not find. */
#define ONLINE_MONEY        2000000u
#define ONLINE_GARAGE_VA    0x0053F308u
#define ONLINE_GARAGE_SIZE  0x18340u
/* Unlock flags the game reads before its unlocking rules (globals, not in
 * the save). Rule item types are the names at 0x004B3A18: 0 GENERIC, 1 VP
 * (vehicle purchase), 5 C (city), 6 RACE_C, 9 PERF. The cheat handler
 * sub_001923B0's three codes set 0x0053F2ED (every city, type 5,
 * sub_00289CC0) and 0x0053F2EC (every race, type 6, sub_0028A1B0): neither
 * reaches a car or a part, so the garage stayed locked with them alone.
 * Cars: 0x0053F2EE, read by the VP checks sub_00289D20/D40/D80 (showroom
 * list sub_00154710/sub_001550B0, its shop status sub_00145A10). Parts,
 * paint, rims, perf levels: 0x0053F2F2, the garage's own "all unlocked"
 * flag (sub_0013E2D0/E3C0, sub_001465E0, sub_00147600 level 3,
 * sub_0014E8E0 ...), which the GENERIC (sub_0028B4E0) and PERF
 * (sub_0028A2F0) checks have no flag of their own for. Nothing in the
 * game writes 0x0053F2EE or 0x0053F2F2. */
#define ONLINE_UNLOCK_RACES 0x0053F2ECu
#define ONLINE_UNLOCK_CITY  0x0053F2EDu
#define ONLINE_UNLOCK_CARS  0x0053F2EEu
#define ONLINE_UNLOCK_GARAGE 0x0053F2F2u
static const uint32_t s_online_flag[4] = {
    ONLINE_UNLOCK_RACES, ONLINE_UNLOCK_CITY, ONLINE_UNLOCK_CARS, ONLINE_UNLOCK_GARAGE
};
static int s_online, s_online_money_ok;
static uint32_t s_online_money;
static uint8_t s_online_garage[ONLINE_GARAGE_SIZE];
static uint8_t s_online_unlock[4];

static int online_ram(uint32_t va)               /* RAM, or its contiguous window */
{
    return (va >= 0x00010000u && va < 0x04000000u) || (va >= 0x80000000u && va < MC3_CONTIG_END);
}

static uint32_t online_money_va(void)
{
    uint32_t mgr = MEM32(0x00557F0Cu), prog;
    if (!online_ram(mgr))
        return 0;
    prog = MEM32(mgr + 0xCu);
    return online_ram(prog) ? prog + 0xACCu : 0;
}

/* Read back: how many showroom cars the game's own check calls buyable.
 * The showroom (sub_00154710) asks sub_00289D80 (thiscall (car name),
 * ret 4; this = [[[0x557F0C]+0xC]+0xC]) for each of the [0x557EB4] cars,
 * CarCfg i at 0x541700 + i*0x108 (in the garage blob), name at +0xE3. */
void sub_00289D80(void);
static uint32_t online_cars_unlocked(uint32_t *total)
{
    uint32_t mgr = MEM32(0x00557F0Cu), prog, rules, n = MEM16(0x00557EB4u), i, ok = 0;
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    *total = 0;
    if (!online_ram(mgr) || !online_ram(prog = MEM32(mgr + 0xCu)) || !online_ram(rules = MEM32(prog + 0xCu))
        || !online_ram(MEM32(0x00557EB0u)))
        return 0;
    if (n > 127u)                                /* the blob holds 127 */
        n = 127u;
    for (i = 0; i < n; i++) {
        g_esp = (r[4] - 0x200u) & ~0xFu;
        g_esp -= 4; MEM32(g_esp) = 0x00541700u + i * 0x108u + 0xE3u;
        g_esp -= 4; MEM32(g_esp) = 0;           /* return address */
        g_ecx = rules;
        sub_00289D80();
        ok += (g_eax & 0xFFu) != 0;
    }
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
    *total = n;
    return ok;
}

/* ── The Online profile: the session's money and garage, kept between sessions ──
 *
 * Browser only (native: $2,000,000 each session as before). The file is
 * /opfs/online/profile.bin (OPFS, beside the career's UDATA), little-endian:
 *   +0  'MC3O'   +4 version 2   +8 body size (4 + 0x18340 + 4)
 *   +12 CRC-32 (zlib's) of the body   +16 cars owned (for the page)
 *   +20 body: money (int32), then the garage blob (0x0053F308, raw, as the
 *       career save keeps it; CarCfg vtables are this XBE's; the garage
 *       cars' parts, paint and vinyls are their CarCfgs in it), then
 *       (version 2) the car picked: byte 0x0053F2FC (1 showroom car,
 *       0 garage car), a zero byte, int16 0x0053F2FE (its index). Those
 *       globals sit outside the blob and the career save; Arcade's setup
 *       (sub_0013AB20, Free Roam's way in) copies that CarCfg (showroom
 *       0x541700 + i*0x108, garage 0x53F810 + i*0x108) into player 0's
 *       entry, the car the cruise builds; the garage and the car row write
 *       them (e.g. at 0x0014E029, 0x0015516D). Version 1 files (no car) still
 *       load: the car stays the game's pick, as before. Then (version 3)
 *       the races won (uint32, online_progress.c: they unlock abilities and
 *       reward paints); versions 1 and 2 load with 0 wins.
 * Loaded at each session's start (a missing, short, old or damaged file:
 * a fresh start, logged); none yet: $2,000,000 and the garage as it is, as
 * before. Taken from the game and saved when the garage (state 5) is left,
 * when another car was picked by a state request (the setup screen's
 * load) and at the session's end, before the career goes back; the career's
 * .mc3 is never written while Online (sub_00022040). A garage applied by
 * the game meanwhile (the main menu's Garage loads the career,
 * sub_00019560) is replaced by the online one the way the game applies a
 * profile: copy (sub_0026BD10's), sub_0026BC60(1), sub_0026CBB0. */
#define ONLINE_PROFILE_MAGIC 0x4F33434Du        /* "MC3O" */
#define ONLINE_PROFILE_VER   3u
#define ONLINE_PROFILE_HDR   20u
#define ONLINE_PROFILE_BODY1 (4u + ONLINE_GARAGE_SIZE)          /* version 1 */
#define ONLINE_PROFILE_BODY2 (ONLINE_PROFILE_BODY1 + 4u)        /* version 2: + the car picked */
#define ONLINE_PROFILE_BODY  (ONLINE_PROFILE_BODY2 + 4u)        /* version 3: + the races won */
#define ONLINE_CAR_FROM      0x0053F2FCu        /* player 0's pick: 1 showroom car, 0 garage car */
#define ONLINE_CAR_INDEX     0x0053F2FEu        /* int16, its index */
static uint32_t s_on_money = ONLINE_MONEY;      /* the online profile's money */
static uint8_t s_on_garage[ONLINE_GARAGE_SIZE]; /* and its garage, once s_on_have */
static int s_on_have;
static uint8_t s_on_car[4];                     /* and its car: from, 0, index (LE) */
static int s_on_car_ok;                         /* s_on_car holds one */
static uint32_t s_on_wins;                      /* races won Online (online_progress.c) */
#ifdef __EMSCRIPTEN__
static uint8_t s_online_car[4];                 /* the career's pick, for the end */
#endif

static void online_car_get(uint8_t c[4])
{
    uint16_t i = MEM16(ONLINE_CAR_INDEX);
    c[0] = MEM8(ONLINE_CAR_FROM); c[1] = 0; c[2] = (uint8_t)i; c[3] = (uint8_t)(i >> 8);
}

/* The CarCfg a pick names (as sub_0013AB20 reads it), 0 when it names none:
 * a garage car past the garage's count ([0x53F80C], 30 at most) or a
 * showroom car past the showroom's ([0x557EB4], 127 at most). */
static uint32_t online_car_cfg(const uint8_t c[4])
{
    int32_t i = (int16_t)(c[2] | c[3] << 8);
    if (c[0] == 1u && i >= 0 && i < 127 && (uint32_t)i < MEM16(0x00557EB4u))
        return 0x00541700u + (uint32_t)i * 0x108u;
    if (c[0] == 0u && i >= 0 && i < 30 && (uint32_t)i < MEM8(0x0053F80Cu))
        return 0x0053F810u + (uint32_t)i * 0x108u;
    return 0;
}

/* The profile's car becomes the game's pick (after its garage is in);
 * none or not there: the game's pick stays and becomes the profile's. */
static void online_car_restore(void)
{
    uint32_t cfg = s_on_car_ok ? online_car_cfg(s_on_car) : 0;
    if (cfg) {
        MEM8(ONLINE_CAR_FROM) = s_on_car[0];
        MEM16(ONLINE_CAR_INDEX) = (uint16_t)(s_on_car[2] | s_on_car[3] << 8);
        fprintf(stderr, "[ONLINE] profile car: %.24s (%s car %d)\n", (const char *)XBOX_PTR(cfg + 0xE3u),
                s_on_car[0] ? "showroom" : "garage", (int16_t)(s_on_car[2] | s_on_car[3] << 8));
        return;
    }
    if (s_on_car_ok)
        fprintf(stderr, "[ONLINE] profile car (%u, %d) is not there: the game's pick\n", s_on_car[0],
                (int16_t)(s_on_car[2] | s_on_car[3] << 8));
    online_car_get(s_on_car);
    s_on_car_ok = 1;
}

/* Garage cars: of the 30 CarCfg at +0x508, those with a string at +0xF9,
 * the test sub_0026CBB0 makes per slot before it looks up the name (+0xE3). */
static uint32_t online_cars(const uint8_t *g)
{
    uint32_t i, n = 0;
    for (i = 0; i < 30u; i++)
        n += g[0x508u + i * 0x108u + 0xF9u] != 0;
    return n;
}

void sub_0026BC60(void);
void sub_0026CBB0(void);
static void online_garage_apply(void)           /* the online garage into the game */
{
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    memcpy((uint8_t *)XBOX_PTR(ONLINE_GARAGE_VA), s_on_garage, ONLINE_GARAGE_SIZE);
    g_esp = (r[4] - 0x200u) & ~0xFu;
    g_esp -= 4; MEM32(g_esp) = 1;               /* thiscall (1), ret 4 */
    g_esp -= 4; MEM32(g_esp) = 0;
    g_ecx = ONLINE_GARAGE_VA;
    sub_0026BC60();
    g_esp = (r[4] - 0x200u) & ~0xFu;
    g_esp -= 4; MEM32(g_esp) = 0;               /* thiscall () */
    g_ecx = ONLINE_GARAGE_VA;
    sub_0026CBB0();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
}

#ifdef __EMSCRIPTEN__
#include <sys/stat.h>
uint32_t rc_crc32(const uint8_t *p, uint32_t n);    /* xboxrecomp/src/net/roam/roam_core.c */
#define ONLINE_PROFILE_DIR  "/opfs/online"
#define ONLINE_PROFILE_PATH ONLINE_PROFILE_DIR "/profile.bin"
static uint8_t s_on_file[ONLINE_PROFILE_HDR + ONLINE_PROFILE_BODY];

static void online_profile_load(void)
{
    uint32_t h[5] = { 0 };
    size_t n;
    const char *bad = NULL;
    FILE *f = fopen(ONLINE_PROFILE_PATH, "rb");
    s_on_have = 0;
    s_on_car_ok = 0;
    s_on_wins = 0;
    s_on_money = ONLINE_MONEY;
    if (!f) {
        fprintf(stderr, "[ONLINE] no profile yet: $%u and the garage as it is\n", ONLINE_MONEY);
        return;
    }
    n = fread(s_on_file, 1, sizeof s_on_file, f);  /* versions 1 and 2 are shorter */
    fclose(f);
    if (n >= sizeof h)
        memcpy(h, s_on_file, sizeof h);
    if (n < ONLINE_PROFILE_HDR)
        bad = "short";
    else if (h[0] != ONLINE_PROFILE_MAGIC)
        bad = "not an online profile";
    else if (!(h[1] == 1u && h[2] == ONLINE_PROFILE_BODY1) && !(h[1] == 2u && h[2] == ONLINE_PROFILE_BODY2) &&
             !(h[1] == ONLINE_PROFILE_VER && h[2] == ONLINE_PROFILE_BODY))
        bad = "another version";
    else if (n != ONLINE_PROFILE_HDR + h[2])
        bad = "short";
    else if (h[3] != rc_crc32(s_on_file + ONLINE_PROFILE_HDR, h[2]))
        bad = "damaged (CRC)";
    if (bad) {
        fprintf(stderr, "[ONLINE] profile " ONLINE_PROFILE_PATH " %s: a fresh start ($%u)\n", bad, ONLINE_MONEY);
        return;
    }
    memcpy(&s_on_money, s_on_file + ONLINE_PROFILE_HDR, 4);
    memcpy(s_on_garage, s_on_file + ONLINE_PROFILE_HDR + 4, ONLINE_GARAGE_SIZE);
    s_on_have = 1;
    if (h[1] >= 2u) {
        memcpy(s_on_car, s_on_file + ONLINE_PROFILE_HDR + ONLINE_PROFILE_BODY1, 4);
        s_on_car_ok = 1;
    }
    if (h[1] >= 3u)
        memcpy(&s_on_wins, s_on_file + ONLINE_PROFILE_HDR + ONLINE_PROFILE_BODY2, 4);
    fprintf(stderr, "[ONLINE] profile loaded (version %u): $%u, %u cars, %u races won\n", h[1], s_on_money,
            online_cars(s_on_garage), (unsigned)s_on_wins);
}

static void online_profile_save(void)           /* written beside, then renamed over: never half a file */
{
    uint32_t h[5] = { ONLINE_PROFILE_MAGIC, ONLINE_PROFILE_VER, ONLINE_PROFILE_BODY, 0, online_cars(s_on_garage) };
    FILE *f;
    int ok;
    if (!s_on_have)
        return;
    memcpy(s_on_file + ONLINE_PROFILE_HDR, &s_on_money, 4);
    memcpy(s_on_file + ONLINE_PROFILE_HDR + 4, s_on_garage, ONLINE_GARAGE_SIZE);
    memcpy(s_on_file + ONLINE_PROFILE_HDR + ONLINE_PROFILE_BODY1, s_on_car, 4);
    memcpy(s_on_file + ONLINE_PROFILE_HDR + ONLINE_PROFILE_BODY2, &s_on_wins, 4);
    h[3] = rc_crc32(s_on_file + ONLINE_PROFILE_HDR, ONLINE_PROFILE_BODY);
    memcpy(s_on_file, h, sizeof h);
    mkdir(ONLINE_PROFILE_DIR, 0777);
    f = fopen(ONLINE_PROFILE_PATH ".tmp", "wb");
    ok = f && fwrite(s_on_file, 1, sizeof s_on_file, f) == sizeof s_on_file;
    if (f)
        ok = (fclose(f) == 0) && ok;
    ok = ok && rename(ONLINE_PROFILE_PATH ".tmp", ONLINE_PROFILE_PATH) == 0;
    if (ok)
        fprintf(stderr, "[ONLINE] profile saved: $%u, %u cars, %u races won\n", s_on_money, h[4], (unsigned)s_on_wins);
    else
        fprintf(stderr, "[ONLINE] profile NOT saved (" ONLINE_PROFILE_PATH "): the last one stays\n");
}
#else
static void online_profile_load(void) {}
static void online_profile_save(void) {}
#endif

static void online_money(void)                  /* the session's money and unlocks */
{
    uint32_t m = online_money_va(), i, n, total;
    if (!s_online)
        return;
    if (m)
        MEM32(m) = s_on_money;
    for (i = 0; i < 4; i++)
        MEM8(s_online_flag[i]) = 1;
    n = online_cars_unlocked(&total);
    fprintf(stderr, "[ONLINE] unlocks applied: %u cars (of %u in the showroom), garage items flag %u\n",
            n, total, MEM8(ONLINE_UNLOCK_GARAGE));
}

static void online_keep(void)                   /* the career's money and garage, for the end */
{
    uint32_t m = online_money_va();
    s_online_money_ok = m != 0;
    s_online_money = m ? MEM32(m) : 0;
    memcpy(s_online_garage, (uint8_t *)XBOX_PTR(ONLINE_GARAGE_VA), ONLINE_GARAGE_SIZE);
}

static void online_enter(void)
{
    if (!s_online) {
        online_keep();
        for (int i = 0; i < 4; i++)
            s_online_unlock[i] = MEM8(s_online_flag[i]);
        s_online = 1;
#ifdef __EMSCRIPTEN__
        online_car_get(s_online_car);            /* the career's car pick, for the end */
#endif
        online_profile_load();
        if (s_on_have)
            online_garage_apply();
        online_car_restore();                    /* before Arcade's setup copies the pick (sub_0013AB20) */
        fprintf(stderr, "[ONLINE] on: money %u -> %u, all cars and parts unlocked, career money and"
                " garage kept aside, no saves until Career, Tokyo or Arcade\n", s_online_money, s_on_money);
    }
    online_money();
}

/* The garage (state 5) was left: what it bought and tuned is the online
 * profile now (browser only). */
static void online_garage_left(void)
{
#ifdef __EMSCRIPTEN__
    uint32_t m = online_money_va();
    if (!s_online)
        return;
    if (m)
        s_on_money = MEM32(m);
    memcpy(s_on_garage, (uint8_t *)XBOX_PTR(ONLINE_GARAGE_VA), ONLINE_GARAGE_SIZE);
    s_on_have = 1;
    online_car_get(s_on_car);                   /* the car picked, in that garage */
    s_on_car_ok = 1;
    online_profile_save();
#endif
}

/* A state request (sub_00020E40): another car picked (the setup screen's
 * car row, the garage) is saved with the money and garage it belongs to,
 * so a closed page keeps it too (browser only). */
static void online_car_picked(void)
{
#ifdef __EMSCRIPTEN__
    uint8_t c[4];
    if (!s_online)
        return;
    online_car_get(c);
    if (!s_on_car_ok || memcmp(c, s_on_car, 4) != 0)
        online_garage_left();
#endif
}

static void online_leave(void)
{
    uint32_t m = online_money_va();
    if (!s_online)
        return;
    online_car_picked();                         /* a car picked since */
    online_profile_save();                       /* as taken at the last garage exit */
    if (m && s_online_money_ok)
        MEM32(m) = s_online_money;
    memcpy((uint8_t *)XBOX_PTR(ONLINE_GARAGE_VA), s_online_garage, ONLINE_GARAGE_SIZE);
    for (int i = 0; i < 4; i++)
        MEM8(s_online_flag[i]) = s_online_unlock[i];
#ifdef __EMSCRIPTEN__
    MEM8(ONLINE_CAR_FROM) = s_online_car[0];     /* the career's car pick back */
    MEM16(ONLINE_CAR_INDEX) = (uint16_t)(s_online_car[2] | s_online_car[3] << 8);
#endif
    s_online = 0;
    roam_stop();                                 /* free roam (O3) ends with the session */
    fprintf(stderr, "[ONLINE] off: money %u, the garage and the unlocks are the career's again\n",
            s_online_money);
}

extern void sub_0018C530_gen(void);
void sub_0018C530(void) { online_leave(); sub_0018C530_gen(); }    /* Arcade */
extern void sub_0018C560_gen(void);
void sub_0018C560(void) { online_leave(); sub_0018C560_gen(); }    /* Career */
extern void sub_0018C580_gen(void);
void sub_0018C580(void) { online_leave(); sub_0018C580_gen(); }    /* Tokyo */
/* Main menu "Online": native -> the original networking screen 0x1E (FE ->
 * sub_00154B30(0x1E)). Browser: straight into Free Roam (the screen's
 * Internet_Play pick below, sub_00180160, which ends in Arcade's own main
 * menu callback sub_0018C530: same functor type as this one; in the
 * browser it loads the cruise at once, fr_direct); held White
 * (Q/Shift, a shoulder button) while picking it opens screen 0x1E as
 * before, for Online Races (LAN_Play, System Link). */
#ifdef __EMSCRIPTEN__
#include "input/xinput_xbox.h"
#endif
extern void sub_0018C5A0_gen(void);
void sub_00180160(void);
void sub_0018C5A0(void)
{
#ifdef __EMSCRIPTEN__
    XBOX_INPUT_STATE st;
    if (!(xbox_InputGetState(0, &st) == 0 && st.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] > XBOX_ANALOG_BUTTON_THRESHOLD)) {
        fprintf(stderr, "[ONLINE] main menu Online: straight into Free Roam (hold White for Online Races)\n");
        sub_00180160();
        return;
    }
#endif
    sub_0018C5A0_gen();
}
/* Screen 0x1E "Free Roam" (Internet_Play, callback sub_00180160): Arcade's way + the Online session.
 * "Online Races" (LAN_Play, sub_001801C0) stays the game's System Link, no session. */
extern void sub_00180160_gen(void);
static void roam_begin(uint32_t cfg);           /* free roam on + our car (O4), roam_game.c */
#ifdef __EMSCRIPTEN__
/* Browser: Free Roam loads at once, no setup (0x19) or Start (0x1B)
 * screen. What those screens do, in their order: Arcade's main menu item
 * (sub_0018C530 without its Go(0x19): [0x50F14C] = 0, sub_0013AB20 copies
 * the car picked (0x53F2FC/FE, the online profile's, online_car_restore)
 * into the settings [0x53F2E8], sub_0013A590), the setup's builder override
 * (San Diego, Cruise, 1 player; fr_cruise), its Go (sub_0013B110: event
 * index by name sub_0026B010, SetEvent sub_0026B8E0 on the settings), then
 * the Start screen's A (sub_00185780 -> sub_00184FD0 when [0x50F14C] == 0:
 * checks, [0x53F2E4] = copy of the settings, Request(6)), as the main
 * menu's own Request(0x12) item does from here (sub_0018C5C0). The Start
 * screen's +0x1E0 byte picks 0x4B36C4 (table 0x4B25AC: 0,1,2,3,4,5,10,15;
 * 0 at boot): a stand-in screen gives index 0 and the old value is put
 * back, so it stays the game's. MC3_FR_DIRECT=0 brings the screens back.
 * The A callback still goes to screen 0x19 (sub_0018C530_gen) and these
 * steps run at the next state pass (sub_00020EC0), so the setup screen shows
 * for a frame at most. A callback that changed no screen (5428501) hung the
 * game (2026-11-11): the main menu's Flash event dispatch (sub_00201990
 * under sub_0018CC90 -> sub_00160B90 -> sub_002071C0) never ended, main
 * thread holding the guest lock, no frame, Request(6) never applied; it hung
 * the same with these steps deferred and no Go. */
static uint32_t roam_call(recomp_func_t fn, uint32_t ecx, uint32_t top, int n, const uint32_t *args);
static void fr_cruise(uint32_t s);
void sub_0013AB20(void);
void sub_0013A590(void);
void sub_0026B010(void);
void sub_0026B8E0(void);
void sub_00184FD0(void);
static int s_fr_pending;                         /* Online picked: load at the next state pass */
static void fr_direct(void)
{
    const char *e = getenv("MC3_FR_DIRECT");
    s_fr_pending = !(e && e[0] == '0');
}
static void fr_direct_pass(uint32_t sm)
{
    uint32_t s = MEM32(0x0053F2E8u), top = (g_esp - 0x800u) & ~0xFu, obj = top + 0x100u, a[2];
    uint8_t keep = MEM8(0x004B36C4u);
    if (!s_fr_pending)
        return;
    s_fr_pending = 0;
    if (MEM32(sm + 4u) != 7u || !online_ram(s) || !online_ram(MEM32(0x0053F2E4u))) {
        fprintf(stderr, "[ONLINE] Free Roam: direct load dropped (state %u)\n", MEM32(sm + 4u));
        return;
    }
    MEM32(0x0050F14Cu) = 0;                      /* not a System Link game */
    roam_call(sub_0013AB20, 0, top, 0, NULL);
    roam_call(sub_0013A590, 0, top, 0, NULL);
    MEM32(s) = 0;                                /* San Diego */
    fr_cruise(s);
    a[0] = 0;
    a[1] = roam_call(sub_0026B010, s, top, 0, NULL);
    roam_call(sub_0026B8E0, s, top, 2, a);
    MEM32(s + 0x4Cu) = 1;
    MEM32(s + 0x40u) = 0;                        /* no AI racers */
    MEM8(obj + 0x1E0u) = 0;
    roam_call(sub_00184FD0, obj, top, 0, NULL);
    MEM8(0x004B36C4u) = keep;
    fprintf(stderr, "[ONLINE] Free Roam: straight into the San Diego cruise (event %d, %.24s), no setup or Start"
            " screen, at the state pass (MC3_FR_DIRECT=0 off)\n", (int32_t)a[1], (const char *)XBOX_PTR(s + 0x68u));
}
#endif
void sub_00180160(void)
{
    uint32_t cfg = 0x00541700u;
    online_enter();
#ifdef __EMSCRIPTEN__
    if (online_car_cfg(s_on_car))                /* the profile's car to the peers, not showroom car 0 */
        cfg = online_car_cfg(s_on_car);
#endif
    roam_begin(cfg);
#ifdef __EMSCRIPTEN__
    fr_direct();                                 /* the cruise load at the next state pass */
#endif
    sub_0018C530_gen();                          /* Go(0x19) still: without a screen change the menu's dispatch never ends */
}

/* The main menu's builder copies each label's text into its item when it
 * runs (CM_Networking: SetLabel sub_00348D70, a wcsncpy into the item),
 * and it can run before the per-frame hook has seen the table: rewrite the
 * table first, so the copy already reads "Online". */
extern void sub_0018C770_gen(void);
void sub_00348AF0(void);
/* "Online" first: the builder adds Career, Tokyo, Arcade, Online as slots
 * 0-3 of the FE's shared list (AddItem sub_00348BE0 appends; its first
 * argument is an item id kept in the record, not a position). Per slot the
 * list holds a functor at list+0xEC+16*slot (+4 = callback, slots < 20), a
 * record at [list+0x248]+16*(rows*col+slot) {id lo, id hi, label block,
 * arg} (rows [list+0x230], col [list+0x258], count [list+0x238+4*col]) and
 * a label block of [list+0x234] {text, flag} pairs that +8 points to. Slot
 * 3 moves to 0 (contents moved, +8 kept), then SetSelection(0, -1)
 * (sub_00348AF0, thiscall (slot, col), ret 8) again, as the builder ended,
 * so the cursor starts on Online and the selected id is Online's. The
 * browser drops the Arcade item (callback 0x0018C530) before that. */
static void main_menu_online_first(void)
{
    uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
    uint32_t fe = MEM32(0x0050F148u), lst, col, base, nl, i, k, w;
    uint32_t fn[4], rec[3], lab[2 * 8];
    if (!online_ram(fe) || !online_ram(fe = MEM32(fe + 0xCu)) || !online_ram(lst = MEM32(fe + 0xFCu)))
        return;
    col = MEM32(lst + 0x258u);
    nl = MEM32(lst + 0x234u);
    if (col > 7u || nl > 8u || (int32_t)MEM32(lst + 0x238u + 4u * col) < 4
        || MEM32(lst + 0xECu + 3u * 16u + 4u) != 0x0018C5A0u)   /* not the menu we know: leave it */
        return;
    base = MEM32(lst + 0x230u) * col;
#define MM_REC(s) (MEM32(lst + 0x248u) + 16u * (base + (s)))
    for (w = 0; w < 4; w++) fn[w] = MEM32(lst + 0xECu + 3u * 16u + 4u * w);
    rec[0] = MEM32(MM_REC(3)); rec[1] = MEM32(MM_REC(3) + 4u); rec[2] = MEM32(MM_REC(3) + 0xCu);
    for (k = 0; k < 2 * nl; k++) lab[k] = MEM32(MEM32(MM_REC(3) + 8u) + 4u * k);
    for (i = 3; i > 0; i--) {                    /* slots 0-2 one down */
        for (w = 0; w < 4; w++)
            MEM32(lst + 0xECu + i * 16u + 4u * w) = MEM32(lst + 0xECu + (i - 1u) * 16u + 4u * w);
        MEM32(MM_REC(i)) = MEM32(MM_REC(i - 1u));
        MEM32(MM_REC(i) + 4u) = MEM32(MM_REC(i - 1u) + 4u);
        MEM32(MM_REC(i) + 0xCu) = MEM32(MM_REC(i - 1u) + 0xCu);
        for (k = 0; k < 2 * nl; k++)
            MEM32(MEM32(MM_REC(i) + 8u) + 4u * k) = MEM32(MEM32(MM_REC(i - 1u) + 8u) + 4u * k);
    }
    for (w = 0; w < 4; w++) MEM32(lst + 0xECu + 4u * w) = fn[w];
    MEM32(MM_REC(0)) = rec[0]; MEM32(MM_REC(0) + 4u) = rec[1]; MEM32(MM_REC(0) + 0xCu) = rec[2];
    for (k = 0; k < 2 * nl; k++) MEM32(MEM32(MM_REC(0) + 8u) + 4u * k) = lab[k];
#ifdef __EMSCRIPTEN__
    {   /* Browser: no Arcade item (Free Roam is its way into the cruise). Every
         * list routine reads the count [list+0x238+4*col] live (SetSelection's
         * bound, the up/down wrap, the visible rows): later slots move one up
         * as above and the count drops by one. MC3_ARCADE=1 keeps it. */
        static int keep = -1;
        uint32_t n = MEM32(lst + 0x238u + 4u * col), a;
        if (keep < 0) {
            const char *e = getenv("MC3_ARCADE");
            keep = e && e[0] == '1';
        }
        for (a = 0; !keep && n <= 20u && a < n; a++) {
            if (MEM32(lst + 0xECu + a * 16u + 4u) != 0x0018C530u)
                continue;
            for (i = a; i + 1u < n; i++) {       /* slots after Arcade one up */
                for (w = 0; w < 4; w++)
                    MEM32(lst + 0xECu + i * 16u + 4u * w) = MEM32(lst + 0xECu + (i + 1u) * 16u + 4u * w);
                MEM32(MM_REC(i)) = MEM32(MM_REC(i + 1u));
                MEM32(MM_REC(i) + 4u) = MEM32(MM_REC(i + 1u) + 4u);
                MEM32(MM_REC(i) + 0xCu) = MEM32(MM_REC(i + 1u) + 0xCu);
                for (k = 0; k < 2 * nl; k++)
                    MEM32(MEM32(MM_REC(i) + 8u) + 4u * k) = MEM32(MEM32(MM_REC(i + 1u) + 8u) + 4u * k);
            }
            MEM32(lst + 0x238u + 4u * col) = n - 1u;
            fprintf(stderr, "[ONLINE] main menu: Arcade item (slot %u) removed, %u items (MC3_ARCADE=1 keeps it)\n",
                    a, n - 1u);
            break;
        }
    }
#endif
#undef MM_REC
    g_esp = (r[4] - 0x200u) & ~0xFu;
    g_esp -= 4; MEM32(g_esp) = 0xFFFFFFFFu;      /* col: the current one */
    g_esp -= 4; MEM32(g_esp) = 0;                /* slot 0 */
    g_esp -= 4; MEM32(g_esp) = 0;                /* return address */
    g_ecx = lst;
    sub_00348AF0();
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
    g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
}
/* "Online (N)": N players in Free Roam now, from the page's players push
 * (web/pre.js writes freeRoam.total into mc3_online_count_box once a second;
 * -1 unknown: plain "Online"). The builder's SetLabel (sub_00348D70 override
 * below) gets a copy of CM_Networking's text with " (N)" for that item, so
 * the count is the one at each build of the menu: the item's text is copied
 * once (wcsncpy, n = [[list+0x244] + 12*col + 4]); a label that would not
 * fit stays "Online". */
static int s_mm_build;                           /* inside the main menu's builder */
static uint32_t str_table(uint32_t *n, uint32_t *buckets);
static uint32_t str_text(uint32_t n, uint32_t buckets, const char *key);
#ifdef __EMSCRIPTEN__
static int32_t s_on_count = -1;
EMSCRIPTEN_KEEPALIVE uintptr_t mc3_online_count_box(void) { return (uintptr_t)&s_on_count; }
#endif
static void main_menu_count_label(uint32_t esp)  /* SetLabel, thiscall (item, column, text), esp at its return */
{
#ifdef __EMSCRIPTEN__
    uint32_t n, b, t, cols, col = MEM32(esp + 8u), max, buf = (esp - 0x400u) & ~0xFu, i;
    int32_t c = __atomic_load_n(&s_on_count, __ATOMIC_RELAXED);
    char num[16];
    unsigned nl;
    if (!s_mm_build || c < 0 || !str_table(&n, &b) || !(t = str_text(n, b, "CM_Networking")) ||
        MEM32(esp + 0xCu) != t || col > 7u || !online_ram(g_ecx) || !online_ram(cols = MEM32(g_ecx + 0x244u)))
        return;
    max = MEM32(cols + 12u * col + 4u);
    nl = (unsigned)snprintf(num, sizeof num, " (%d)", (int)c);
    for (i = 0; i < 32u && MEM16(t + 2u * i); i++)
        MEM16(buf + 2u * i) = MEM16(t + 2u * i);
    if (i + nl > max || i == 32u)
        return;
    for (unsigned k = 0; k < nl; k++)
        MEM16(buf + 2u * (i + k)) = (uint8_t)num[k];
    MEM16(buf + 2u * (i + nl)) = 0;
    MEM32(esp + 0xCu) = buf;
#else
    (void)esp;
#endif
}
/* mcMenuModeSelect build */
void sub_0018C770(void)
{
    online_strings();
    s_mm_build = 1;
    sub_0018C770_gen();
    s_mm_build = 0;
    main_menu_online_first();
}

extern void sub_00021A60_gen(void);
static void roam_garage(void);                  /* free roam: status garage (roam_game.c) */
void sub_00021A60(void) { online_money(); roam_garage(); sub_00021A60_gen(); }    /* state 5: the garage opens */

/* Free Roam vs the game's own System Link session (built at boot, port 1000
 * open for good): when it sees a peer join, the host (OnPeerJoined
 * sub_000244C0) sends RACE_CONFIG state 5 (SendRaceConfig sub_001AE040, msg
 * 0x43) and every member's OnState (sub_00025E30) restarts the cruise
 * ('host_selected_restart'). In Free Roam (the Online session, not a System
 * Link game: [0x50F14C] == 0) both ends drop state 5; Online Races untouched. */
/* More than state 5 ends the cruise: the session's member-left handler
 * (vtable +0x38, sub_00026FD0: host_left / host_connection_lost ...) opens
 * FE screen 0x12 (sub_00155760), requests state 3 and leaves the session
 * (sub_00049A80) when a peer times out -- e.g. after our switch link drops
 * for a second. So in Free Roam the session gets no traffic at all
 * (recvfrom/sendto above): it never has members, as when playing alone.
 * The roam layer has its own link. MC3_FR_SL=1 keeps System Link traffic. */
static int fr_sl_detached(void)
{
    static int keep = -1, was;
    int on;
    if (keep < 0) {
        const char *e = getenv("MC3_FR_SL");
        keep = e && e[0] == '1';
    }
    on = !keep && s_online && MEM32(0x0050F14Cu) == 0;
    if (on != was)
        fprintf(stderr, "[NET] free roam: System Link session %s\n",
                on ? "detached (its datagrams dropped)" : "attached again");
    was = on;
    return on;
}
static int fr_net_drop(uint32_t state)
{
    if (state != 5 || !s_online || MEM32(0x0050F14Cu) != 0)
        return 0;
    fprintf(stderr, "[NET] free roam: ignored System Link restart (state 5)\n");
    return 1;
}
/* SendRaceConfig, thiscall (state, x), ret 8. */
extern void sub_001AE040_gen(void);
void sub_001AE040(void)
{
    if (fr_net_drop(MEM32(g_esp + 4u))) {
        g_esp += 12u;
        return;
    }
    sub_001AE040_gen();
}
/* Session OnState, thiscall (state), ret 4. */
extern void sub_00025E30_gen(void);
void sub_00025E30(void)
{
    if (fr_net_drop(MEM32(g_esp + 4u))) {
        g_esp += 8u;
        return;
    }
    sub_00025E30_gen();
}

/* The profile writer, thiscall (name, object, mode), ret 12, al = done. */
extern void sub_00022040_gen(void);
void sub_00022040(void)
{
    if (s_online) {
        static int said;
        if (!said++)
            fprintf(stderr, "[ONLINE] save skipped: the profile stays as it was before Online\n");
        g_eax = (g_eax & ~0xFFu) | 1u;
        g_esp += 16u;
        return;
    }
    sub_00022040_gen();
}

/* A profile is applied, thiscall (flag), ret 4, al = done: the reader
 * (sub_00022310 op 1 -> sub_00019EC0) fills the save manager, then the
 * garage goes to 0x0053F308, the career through sub_00277E20 (money
 * reset to +0xAD0, then the saved one) and sub_0026CBB0. The main menu
 * does not apply the profile (money reads 0, the progress object's
 * constructor value); its Garage item does. So the session stays on: the
 * freshly loaded money and garage become what goes back at the end, and
 * the session's money is put back on top. (The reader alone is no load:
 * sub_00019730 reads a profile into the manager without applying it.) */
extern void sub_00019560_gen(void);
void sub_00019560(void)
{
    sub_00019560_gen();
    if (s_online && (g_eax & 0xFFu)) {
        online_keep();
        if (s_on_have) {                         /* the online garage back on top, and its car */
            online_garage_apply();
            online_car_restore();
        }
        online_money();
        fprintf(stderr, "[ONLINE] a profile was loaded: its money (%u) and garage are kept aside now,"
                " the session's money is %u%s\n", s_online_money, s_on_money,
                s_on_have ? ", the online garage is back" : "");
    }
}

static uint32_t str_hash(const char *k)          /* sub_000EA710 */
{
    uint32_t h = 0, c;
    for (; *k; k++) {
        c = ((uint32_t)(int32_t)(int8_t)*k + h) * 0x401u;
        h = c ^ (c >> 6);
    }
    c = h * 9u;
    return (c ^ (c >> 11)) * 0x8001u;
}

/* The current language's table, 0 until it is there. */
static uint32_t str_table(uint32_t *n, uint32_t *buckets)
{
    uint32_t lang = MEM32(0x00507E08u + 0x28u), t;
    if (lang >= 10u)
        return 0;
    t = MEM32(0x00507E08u + 4u * lang);
    if (!online_ram(t))
        return 0;
    *n = MEM32(t);
    *buckets = MEM32(t + 4u);
    return *n && *n < 0x100000u && online_ram(*buckets) ? t : 0;
}

static uint32_t str_text(uint32_t n, uint32_t buckets, const char *key)
{
    uint32_t h = str_hash(key), node = MEM32(buckets + 4u * (h % n)), entry;
    unsigned guard = 0;
    for (; online_ram(node) && guard < 100000u; node = MEM32(node + 8u), guard++)
        if (MEM32(node) == h) {
            entry = MEM32(node + 4u);
            return online_ram(entry) && online_ram(MEM32(entry + 8u)) ? MEM32(entry + 8u) : 0;
        }
    return 0;
}

static const char *const s_str_from[] = {
    "Xbox System Link", "System Link", "Xbox Live", "Xbox LIVE", "Xbox Communicator",
    "Xbox console", "Xbox Dashboard", "Gamertag", "Xbox",
};
static const char *const s_str_to[] = {
    "LAN Play", "LAN Play", "Online", "Online", "headset",
    "console", "Dashboard", "Name", "console",     /* last: "Xbox doesn't" -> "console doesn't" */
};

/* Rewrites one UTF-16 text in guest memory; every replacement is shorter. */
static int str_patch(uint32_t t)
{
    uint16_t s[1024];
    unsigned len, i, k, r, fl, tl;
    int changed = 0;
    if (!online_ram(t) || (t & 1u))
        return 0;
    for (len = 0; len < 1024u && MEM16(t + 2u * len); len++)
        s[len] = MEM16(t + 2u * len);
    if (len == 1024u)
        return 0;
    for (r = 0; r < sizeof s_str_from / sizeof *s_str_from; r++) {
        fl = (unsigned)strlen(s_str_from[r]);
        tl = (unsigned)strlen(s_str_to[r]);
        for (i = 0; i + fl <= len; i++) {
            for (k = 0; k < fl && s[i + k] == (uint8_t)s_str_from[r][k]; k++)
                ;
            if (k < fl)
                continue;
            for (k = 0; k < tl; k++)
                s[i + k] = (uint8_t)s_str_to[r][k];
            memmove(s + i + tl, s + i + fl, (len - i - fl) * sizeof *s);
            len -= fl - tl;
            i += tl - 1u;
            changed = 1;
        }
    }
    if (changed) {
        for (i = 0; i < len; i++)
            MEM16(t + 2u * i) = s[i];
        MEM16(t + 2u * len) = 0;
    }
    return changed;
}

/* The sign-in texts show the page's nick (MC3_PLAYER_NAME, "Player" if empty),
 * cut to each text's own length: the buffers are not grown. */
static void online_nick(uint32_t n, uint32_t buckets)
{
    static const char *const keys[] = { "Who_Signed_In", "Not Signed In",
        "Not Signed In: Pass code needed", "Sign-In Failed" };
    const char *nick = getenv("MC3_PLAYER_NAME");
    unsigned k, i, len;
    uint32_t t;
    if (!nick || !*nick)
        nick = "Player";
    for (k = 0; k < sizeof keys / sizeof *keys; k++) {
        if (!(t = str_text(n, buckets, keys[k])) || (t & 1u))
            continue;
        for (len = 0; len < 64u && MEM16(t + 2u * len); len++)
            ;
        for (i = 0; i < len && nick[i]; i++)
            MEM16(t + 2u * i) = (uint8_t)nick[i] < 0x80 ? (uint8_t)nick[i] : '?';
        MEM16(t + 2u * i) = 0;
    }
}

/* Overwrites a key's UTF-16 text in place when the original is at least as long. */
static void str_set(uint32_t n, uint32_t buckets, const char *key, const char *txt)
{
    uint32_t t = str_text(n, buckets, key);
    unsigned len, i, tl = (unsigned)strlen(txt);
    if (!t || (t & 1u))
        return;
    for (len = 0; len < 64u && MEM16(t + 2u * len); len++)
        ;
    if (len < tl)
        return;
    for (i = 0; i < tl; i++)
        MEM16(t + 2u * i) = (uint8_t)txt[i];
    MEM16(t + 2u * tl) = 0;
}

static void online_strings(void)
{
    static uint32_t done;                        /* the table already rewritten */
    uint32_t n, buckets, t = str_table(&n, &buckets), net, b, node, entry;
    unsigned patched = 0, guard;
    if (!t || t == done || !(net = str_text(n, buckets, "CM_Networking")))
        return;                                  /* not loaded yet, or done */
    done = t;
    str_set(n, buckets, "Internet_Play", "Free Roam");   /* before the walk: it would shorten them */
    str_set(n, buckets, "LAN_Play", "Online Races");
    for (b = 0; b < n; b++)
        for (node = MEM32(buckets + 4u * b), guard = 0; online_ram(node) && guard < 100000u;
             node = MEM32(node + 8u), guard++) {
            entry = MEM32(node + 4u);
            if (online_ram(entry))
                patched += (unsigned)str_patch(MEM32(entry + 8u));
        }
    online_nick(n, buckets);
    for (b = 0; b < 6u && MEM16(net + 2u * b); b++)
        ;
    if (b == 6u) {                               /* room for "Online" */
        static const char on[] = "Online";
        for (b = 0; b < 6u; b++)
            MEM16(net + 2u * b) = (uint8_t)on[b];
        MEM16(net + 12u) = 0;
    }
    fprintf(stderr, "[ONLINE] strings: %u texts rewritten (language %u), CM_Networking %s\n",
            patched, MEM32(0x00507E08u + 0x28u), b == 6u ? "-> Online" : "too short, kept");
}

/* ── Free roam (O3): remote players as cars in the Online cruise ── */
#include "roam_game.c"
#include "online_progress.c"                    /* Online: race prizes, wins unlock abilities and reward paints */
#include "roam_cars.c"                          /* MC3_ROAM_CARS: room for standalone remote cars */
#include "roam_pool.c"                          /* free roam: the remote cars pool (who drives which car, builds) */
#include "roam_races.c"                         /* free roam: the cities' race start points (roam_races.h) */
#include "roam_race_ai.c"                       /* free roam: an AI race from a start point, then back (feature 3) */
#include "roam_map.c"                           /* Online cruise: garage icons, race start points, players' blips */
#include "roam_tags.c"                          /* free roam: players' names over their cars (page overlay) */
#include "roam_challenge.c"                     /* free roam: 1v1 street challenge in the cruise (feature 2) */

/* A car's "needs a reset" flag ([ecx+0xC] bit 0, ecx = [[veh+8]+0xC]; a
 * leaf, thiscall, ret). The vehicle update (vtable 0x454F40 +8,
 * sub_000FFD90) tests it at 0x000FFEE7: set -- the car fell into the sea --
 * and the car a player's (veh+0x5E = 0), the race not a network one
 * (+0x108 = 0), that racer not done, it sets the FE's +0xE9, requests state
 * 1 and opens the pause menu for player [veh+0x5D] (sub_00154640): screen 2,
 * the one without Resume (Restart Cruise / View Replay / Options / Change
 * Vehicle / Go to Menus). In Free Roam the remote slots are player cars in a
 * cruise that is not a network race, so a peer driving into the sea gave
 * everyone near the menu over their own cruise (2026-11-10; the flash
 * pause_x.xbck read after '[roam] ... left', no RACE OVER line). There, only
 * our own car (player 0's) answers the test; any other reads "no reset" (its
 * pose is the peer's, whose own game resets it). */
void sub_000A5890(void)
{
    uint32_t flag = MEM8(g_ecx + 0xCu) & 1u, p0, veh0;
    if (flag && MEM32(g_esp) == 0x000FFEECu && roam_free_cruise()) {
        p0 = MEM32(MEM32(ROAM_PM_VA) + 8u);
        veh0 = online_ram(p0) ? MEM32(p0 + 0x14u) : 0;
        if (g_esi != veh0) {
            static unsigned said;
            if (said++ < 10u)
                fprintf(stderr, "[ROAM] car %08X (player slot %u) in the water: the game's pause menu for it kept "
                                "shut\n", (unsigned)g_esi, (unsigned)MEM8(g_esi + 0x5Du));
            flag = 0;
        }
    }
    g_eax = (g_eax & ~0xFFu) | flag;
    g_esp += 4u;
}

/* Online progression (online_progress.c), in the Online session only: the
 * driving abilities' checks (rule types 0xC..0x10 cdecl, 0xB cdecl (special)
 * ret 4) and the GENERIC check (thiscall (name, x), ret 8: the reward paints)
 * answer "unlocked" for what the races won reached, else the game's; the
 * race flow's finish (sub_001CFC50, the career's prize routine, a no-op
 * outside game mode 2, run as is) pays our AI race's place. */
extern void sub_00289E10_gen(void);
void sub_00289E10(void) { if (!op_ability(0xC, 0)) sub_00289E10_gen(); }
extern void sub_00289E30_gen(void);
void sub_00289E30(void) { if (!op_ability(0xD, 0)) sub_00289E30_gen(); }
extern void sub_00289E50_gen(void);
void sub_00289E50(void) { if (!op_ability(0xE, 0)) sub_00289E50_gen(); }
extern void sub_00289E70_gen(void);
void sub_00289E70(void) { if (!op_ability(0xF, 0)) sub_00289E70_gen(); }
extern void sub_00289E90_gen(void);
void sub_00289E90(void) { if (!op_ability(0x10, 0)) sub_00289E90_gen(); }
extern void sub_00289EB0_gen(void);
void sub_00289EB0(void) { if (!op_ability(0xB, 1)) sub_00289EB0_gen(); }
extern void sub_0028B4E0_gen(void);
void sub_0028B4E0(void) { if (!op_paint()) sub_0028B4E0_gen(); }
extern void sub_001CFC50_gen(void);
void sub_001CFC50(void)
{
    if (s_online && s_rra_race)
        op_ai_finish(g_ecx, (g_esp - 0x200u) & ~0xFu);
    sub_001CFC50_gen();
}

/* The HUD map's points of interest into the map server's list (vtable
 * 0x450A00): in the Online session every frame (roam_map.c). */
extern void sub_000BF060_gen(void);
void sub_000BF060(void) { rmap_pois(); sub_000BF060_gen(); }

/* Free Roam's garage map (roam_map.c): built after the HUD's init
 * (thiscall (player), ret 4), its icons pushed after each clear of the map
 * server (thiscall, ret), deleted before the HUD's destructor (thiscall, ret). */
extern void sub_000B6A90_gen(void);
void sub_000B6A90(void)
{
    uint32_t hud = g_ecx, top = g_esp, player = MEM32(g_esp + 4u);
    sub_000B6A90_gen();
    rmap_hud_init(hud, player, top);
}

extern void sub_000D0140_gen(void);
void sub_000D0140(void)
{
    uint32_t server = g_ecx, top = g_esp;
    sub_000D0140_gen();
    rmap_server_cleared(server, top);
}

extern void sub_000B7720_gen(void);
void sub_000B7720(void)
{
    rmap_hud_gone(g_ecx, g_esp);
    sub_000B7720_gen();
}

/* The map's player blips (minimap sub_000C3660, full map sub_000C3E70;
 * thiscall, ret 4): then the race start points and the remote players
 * (roam_map.c). */
extern void sub_000BF850_gen(void);
void sub_000BF850(void)
{
    uint32_t map = g_ecx, top = g_esp;
    sub_000BF850_gen();
    if (!rch_map(map, top))                     /* a street challenge's course replaces the race start icons */
        rmap_races(map, top);
    rmap_far(map, top);
}

/* The race's draw (vtable +0x110, thiscall (viewer), ret 4) and a car's
 * icon (thiscall, 6 args, ret 0x18): remote cars the game leaves without
 * its opponent arrow get one (roam_tags.c). */
extern void sub_001CF3B0_gen(void);
void sub_001CF3B0(void)
{
    uint32_t vp = MEM32(g_esp + 4u), top = g_esp;
    roam_icons_begin();
    sub_001CF3B0_gen();
    roam_icons_end(vp, top);
}

extern void sub_000FA070_gen(void);            /* here too: the relift's scan reads this file only */
void sub_000FA070(void)
{
    roam_icon_seen(g_ecx, g_esp);
    sub_000FA070_gen();
}

/* The player manager is made ([0x558814], ctor sub_002BE100, one player
 * per race-setup entry): the Online cruise gets its remote slots first. */
extern void sub_002BE2C0_gen(void);
void sub_002BE2C0(void) { roam_cars_registry(); roam_build(); sub_002BE2C0_gen(); roam_built(); roam_pool_load(); }

/* The car builder's controller step (builder vtable 0x476940): a remote
 * slot's car gets its controller as System Link's remote cars do
 * (roam_build_step). */
extern void sub_002CB320_gen(void);
void sub_002CB320(void) { roam_build_step(sub_002CB320_gen); }

/* The cruise's trigger manager, per frame (vtable 0x4A4EB8 +0x24, thiscall,
 * world +0x5C): each trigger is tested against players 0 .. [0x53F2E0]+0x4C
 * - 1 (sub_003641B0 -> Test, vtable +0x10). Garage doors are two triggers
 * made at load (sub_001CD380): the door that opens (vtable 0x46B928, Test
 * sub_001E2D60: only with player count 1 and race type 0/1, so it stayed
 * shut with our 1 + N slots and the car stopped at the entrance) and the
 * way in behind it (vtable 0x46B8A0, hit sub_001E2F70: this city's garage
 * -> sub_0018C5C0, the main menu's Garage: request 0x12 -> state 5; another
 * city's: count 1 again). In free roam the manager sees the single-player
 * cruise: count 1, so only our car opens doors and drives in (a peer's car
 * entering a garage must not take us there). */
extern void sub_00364260_gen(void);
void sub_00364260(void)
{
    uint32_t setup = MEM32(ROAM_SETUP_VA), n = 0;
    if (s_roam_slots && online_ram(setup) && MEM32(setup + 0x4Cu) == 1u + (uint32_t)s_roam_slots) {
        n = MEM32(setup + 0x4Cu);
        MEM32(setup + 0x4Cu) = 1u;
    }
    sub_00364260_gen();
    if (n)
        MEM32(setup + 0x4Cu) = n;
}

/* City unlocked? (cdecl (city), ret 4): the career's rule check alone
 * (sub_00292CD0, type 5 'C'), without the "every city" flag 0x0053F2ED that
 * its twin sub_00289CC0 tests first. The cruise load makes a city's garage
 * door and way-in triggers (sub_001CD380, call at 0x001CDA74) and the garage
 * map its icons (sub_000BF060) only for a city this calls unlocked. The
 * Online session sets 0x0053F2ED, so Free Roam could pick Detroit, but a
 * profile that had not reached Detroit in the career got no garage there:
 * the door stayed shut and no icon showed (2026-11-09; San Diego is always
 * unlocked). Online, the check takes the flag as sub_00289CC0 does. */
void sub_00289CC0(void);
extern void sub_00289CE0_gen(void);
void sub_00289CE0(void)
{
    if (s_online)
        sub_00289CC0();
    else
        sub_00289CE0_gen();
}

/* ── Free Roam's setup screen: Arcade's screen 0x19 on the Free Roam path ──
 *
 * The Free Roam pick (sub_00180160) goes through Arcade's setup screen
 * (mcMenuArcade; its vtable +0xDC is the builder). In the Online session
 * (s_online; not a System Link game: [0x50F14C] == 0) the screen reads
 * title "FREE ROAM", its mode stays Cruise and its local players row
 * ("Players: 1") shows the free-roam players in the city on the city row
 * ("Online: N"); local players stay 1. Normal Arcade ends the session
 * first (sub_0018C530), so it is untouched.
 * The screen's settings [0x53F2E8]: +0 city (index into the city table
 * [0x557EE4], 0x50 bytes a city; the hub's room id, as roam_room's and the
 * page's CITIES), +0x18 mode (0 Cruise: sub_0013A690's label keys), +0x48
 * custom race, +0x4C local players. The builder sub_0013BA60 adds rows to
 * the FE's shared list ([[0x50F148]+0xC]+0xFC; AddItem sub_00348BE0,
 * SetLabel sub_00348D70 thiscall (item, column 2, UTF-16), which copies
 * the text): city, mode, a race row unless Cruise, players (key
 * "CM_%d_Players"), a car per player. Mode left/right: sub_0013B9B0 -> the
 * cycler sub_0013AEF0 (settings, dir, flag) -> rebuild; the city row
 * (sub_0013B6F0) and the screen's enter (sub_0013C310) call the same
 * cycler; players left/right: sub_0013A250. The title: the enter calls
 * sub_00154E40("CM_Arcade"): the text as UTF-8 into the screen manager
 * [[[0x50F148]+0xC]+0xC] +0x60 (0x78 bytes), then Flash variable "title"
 * (sub_001395E0 on [[0x50F15C]+0xFAC], else on [manager+0x1E8] with
 * 0x45772C). The manager's +0xE0 is the screen asked for last (Go,
 * sub_00163830). The players row keeps the game's own label: the online
 * count per city it once showed cost a server poll every 20 s.
 * The options screen (time of day, weather, pedestrians, traffic) is screen
 * 0x1A, "MenuCustom" (mcMenuCustom, vtable 0x45D7B8, ctor sub_00165BC0): Go
 * (sub_0013B110) asks for the id kept at screen+0x1FC (the factory
 * sub_00156C90 builds "MenuArcade" with 0x1A, sub_0013C190), and the options
 * screen's OK (sub_00166230) copies its choices into the settings
 * (+4 time, +8 weather, +0x10, +0xC; only when they differ from the defaults
 * Go has just put at 0x4B36BC/C0/CC/C8: -1, -1, 4, 4) and asks for its own
 * +0x1E4 = 0x1B. The builder override makes the Arcade screen ask for 0x1B
 * right away: the defaults, no options screen. */
#define FR_SETTINGS_VA 0x0053F2E8u
static int fr_setup(void)
{
    static int off = -1;                         /* MC3_FR_SETUP=0: Arcade's own screen, untouched */
    if (off < 0) {
        const char *e = getenv("MC3_FR_SETUP");
        off = e && e[0] == '0';
    }
    return !off && s_online && MEM32(0x0050F14Cu) == 0;
}

/* Cruise for the screen's city. Go (sub_0013B110) ignores the mode at
 * +0x18: it looks the event name at +0x68 up in the city's list
 * (sub_0026B010 -> sub_0026DFB0; city 0x50 bytes in [0x557EE4]: +0x14
 * count, +0x18 names, +0x20 events of 0x30 bytes, type at +0) and
 * sub_0026B8E0 sets the mode from that event's type. A stale race name
 * (the race row's last pick, or the validator moving past a cruise it
 * thinks locked) launched a race under a "Cruise" label. The name becomes
 * the city's first type-0 event, as sub_0026D9E0 picks; none: empty, so
 * the lookup fails and sub_0026B8E0's -1 path builds the cruise. */
static void fr_cruise(uint32_t s)
{
    uint32_t tbl = MEM32(0x00557EE4u), city = MEM32(s), c, n, i, j, name = 0;
    MEM32(s + 0x18u) = 0;
    MEM8(s + 0x47u) = 0xFFu;
    MEM8(s + 0x48u) = 0;
    if (online_ram(tbl) && city < 12u) {
        c = tbl + city * 0x50u;
        n = MEM32(c + 0x14u);
        for (i = 0; i < n && i < 256u; i++)
            if (MEM32(MEM32(c + 0x20u) + i * 0x30u) == 0) {
                name = MEM32(MEM32(c + 0x18u) + 4u * i);
                break;
            }
    }
    for (j = 0; online_ram(name) && j < 0x9Fu && MEM8(name + j); j++)
        MEM8(s + 0x68u + j) = MEM8(name + j);
    MEM8(s + 0x68u + j) = 0;
}

static uint32_t fr_settings(void)
{
    uint32_t s = MEM32(FR_SETTINGS_VA);
    return online_ram(s) ? s : 0;
}

/* No AI racers in the Free Roam cruise. Go (sub_0013B110) sets the
 * settings' opponent count +0x40 from the event (sub_00165F50: the cruise
 * event's OpponentCount, at most 5), and the options screen's builder
 * (sub_001662F0) sets it back to 0 for a mode without an opponents row
 * (Cruise, jump table 0x16694C). Skipping that screen kept the count: the
 * cruise's race init (sub_001C13E0 -> .rac reader sub_001D3AE0, racers =
 * min(.rac, 8, setup +0x40) while +0x10D) and the AI manager sub_001E43C0
 * built them, red Eclipses driving in the traffic lanes (2026-11-10). Each
 * state request before the load zeroes it in every cruise setup (type 0;
 * the AI race from a start point is another type and keeps its own). */
static void fr_no_opponents(void)
{
    static const uint32_t k_setup[3] = { 0x0053F2E0u, 0x0053F2E4u, FR_SETTINGS_VA };
    uint32_t i, s;
    if (!fr_setup())
        return;
    for (i = 0; i < 3; i++)
        if (online_ram(s = MEM32(k_setup[i])) && MEM32(s + 0x18u) == 0 && MEM32(s + 0x40u) != 0) {
            fprintf(stderr, "[ONLINE] Free Roam: %u AI racers in the cruise setup [%08X] removed\n",
                    (unsigned)MEM32(s + 0x40u), (unsigned)k_setup[i]);
            MEM32(s + 0x40u) = 0;
        }
}

static uint32_t fr_screens(void)                /* the FE's screen manager, 0 none */
{
    uint32_t fe = MEM32(0x0050F148u), a;
    if (!online_ram(fe) || !online_ram(a = MEM32(fe + 0xCu)) || !online_ram(a = MEM32(a + 0xCu)))
        return 0;
    return a;
}

/* The builder, thiscall (): Cruise, one local player. */
extern void sub_0013BA60_gen(void);
void sub_0013BA60(void)
{
    uint32_t s = fr_settings();
    if (!fr_setup() || !s) {
        sub_0013BA60_gen();
        return;
    }
    MEM32(s) = 0;                                /* San Diego only for now (owner, 2026-11-11): the city row stays on it */
    fr_cruise(s);                                /* every settings handler ends here (+0xDC): last word before Go */
    if (online_ram(g_ecx) && MEM32(g_ecx + 0x1FCu) == 0x1Au)
        MEM32(g_ecx + 0x1FCu) = 0x1Bu;           /* Go asks for the options screen (0x1A); ask for what its OK asks for */
    MEM32(s + 0x4Cu) = 1;
    sub_0013BA60_gen();
    MEM32(s + 0x4Cu) = 1;                        /* it takes [0x53F2E4]'s count in a network session */
}

/* SetLabel, thiscall (item, column, text), ret 12. */
extern void sub_00348D70_gen(void);
void sub_00348D70(void)
{
    rra_label(g_esp);                            /* AI race: "Go to Menus" reads "Back to Free Roam" (roam_race_ai.c) */
    main_menu_count_label(g_esp);                /* main menu: "Online (N)" */
    sub_00348D70_gen();
}

/* The mode cycler, cdecl (settings, dir, flag): in Free Roam the mode stays
 * Cruise (dir 0 from mode 0 lands on 0; the cycler still sets the city's
 * cruise up, as a city change needs). */
extern void sub_0013AEF0_gen(void);
void sub_0013AEF0(void)
{
    uint32_t s = MEM32(g_esp + 4u);
    if (fr_setup() && online_ram(s)) {
        MEM32(s) = 0;                            /* San Diego only (the city row calls this cycler too) */
        MEM32(s + 0x18u) = 0;
        MEM8(s + 0x48u) = 0;
        MEM32(g_esp + 8u) = 0;
    }
    sub_0013AEF0_gen();
}

/* The players row's left/right, thiscall (dir), ret 4: nothing in Free Roam. */
extern void sub_0013A250_gen(void);
void sub_0013A250(void)
{
    if (fr_setup()) {
        g_esp += 8u;
        return;
    }
    sub_0013A250_gen();
}

/* The screen title, cdecl (string key): Arcade's reads "FREE ROAM" in Free
 * Roam. */
void sub_001395E0(void);
extern void sub_00154E40_gen(void);
void sub_00154E40(void)
{
    static const char title[] = "FREE ROAM";
    uint32_t key = MEM32(g_esp + 4u), scr, fl, obj, var, i;
    int ours = fr_setup() && online_ram(key) && !memcmp((const void *)XBOX_PTR(key), "CM_Arcade", 10);
    sub_00154E40_gen();
    if (!ours || !(scr = fr_screens()))
        return;
    for (i = 0; i < sizeof title; i++)
        MEM8(scr + 0x60u + i) = (uint8_t)title[i];
    fl = MEM32(0x0050F15Cu);
    if (fl) {
        obj = online_ram(fl) ? MEM32(fl + 0xFACu) : 0;
        var = 0x0045B9F8u;                       /* "title" */
    } else {
        obj = MEM32(scr + 0x1E8u);
        var = 0x0045772Cu;
    }
    if (online_ram(obj)) {
        uint32_t r[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };
        g_esp = (r[4] - 0x200u) & ~0xFu;
        g_esp -= 4; MEM32(g_esp) = scr + 0x60u;  /* thiscall (name, text), ret 8 */
        g_esp -= 4; MEM32(g_esp) = var;
        g_esp -= 4; MEM32(g_esp) = 0;            /* return address */
        g_ecx = obj;
        sub_001395E0();
        g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3];
        g_esp = r[4]; g_ebp = r[5]; g_esi = r[6]; g_edi = r[7];
    }
    fprintf(stderr, "[ONLINE] setup screen: FREE ROAM, Cruise, players row = online in the city\n");
}
