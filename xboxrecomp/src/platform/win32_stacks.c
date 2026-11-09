/* win32_stacks.c -- every host thread's native stack, on Windows x86-64.
 *
 * The Linux tool for "who waits on what" is gdb (make hoststacks); Windows
 * has none at hand. This suspends each other thread of the process in turn,
 * walks its stack with the image's unwind tables (RtlVirtualUnwind) and
 * prints one line per frame: module, offset, and the address as the module
 * was linked (resolve with llvm-symbolizer --obj=<module> <addr>, or
 * tools/windows/symbolize.sh). Called by the watchdog (RECOMP_WATCHDOG_SECS)
 * and xbox_host_stacks_dump(). Found the first Windows boot's stall
 * (2026-10-23).
 */
#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static void frame_line(DWORD tid, int n, DWORD64 pc)
{
    HMODULE mod = NULL;
    char path[MAX_PATH] = "?";
    const char *name = path;
    DWORD64 linked = pc;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(ULONG_PTR)pc, &mod) && mod) {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mod;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)mod + dos->e_lfanew);
        GetModuleFileNameA(mod, path, sizeof path);
        for (const char *p = path; *p; p++)
            if (*p == '\\' || *p == '/') name = p + 1;
        /* The address the module was linked for: what a symbolizer wants. */
        linked = pc - (DWORD64)(ULONG_PTR)mod + nt->OptionalHeader.ImageBase;
    }
    fprintf(stderr, "  [HOSTSTACK] %5lu #%-2d %s+0x%llx  (%s 0x%llx)\n", (unsigned long)tid, n,
            name, (unsigned long long)(pc - (DWORD64)(ULONG_PTR)mod), name,
            (unsigned long long)linked);
}

#define MAX_FRAMES 40

#if defined(__x86_64__) || defined(_M_X64)
/* Addresses only: the thread is suspended, and printing now could wait for
 * a lock it holds (the C runtime's stream lock). */
static int walk(CONTEXT *c, DWORD64 *pcs)
{
    int n;
    for (n = 0; n < MAX_FRAMES && c->Rip; n++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f;
        pcs[n] = c->Rip;
        f = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (f) {
            PVOID handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &handler_data,
                             &establisher, NULL);
        } else {                                 /* a leaf: return address on top */
            c->Rip = *(DWORD64 *)(ULONG_PTR)c->Rsp;
            c->Rsp += 8;
        }
    }
    return n;
}
#elif defined(__aarch64__) || defined(_M_ARM64)
static int walk(CONTEXT *c, DWORD64 *pcs)
{
    int n;
    for (n = 0; n < MAX_FRAMES && c->Pc; n++) {
        DWORD64 base = 0, pc = c->Pc;
        PRUNTIME_FUNCTION f;
        pcs[n] = pc;
        f = RtlLookupFunctionEntry(pc, &base, NULL);
        if (f) {
            PVOID handler_data;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, c, &handler_data,
                             &establisher, NULL);
        } else {                                 /* a leaf: the link register */
            c->Pc = c->Lr;
            c->Lr = 0;
        }
        if (c->Pc == pc)
            break;                               /* no progress: stop */
    }
    return n;
}
#endif

void xbox_host_stacks_dump(const char *why)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    if (snap == INVALID_HANDLE_VALUE)
        return;
    fprintf(stderr, "[HOSTSTACK] every thread's native stack (%s)\n", why ? why : "dump");
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        HANDLE h;
        CONTEXT c;
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
            continue;
        h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                       FALSE, te.th32ThreadID);
        if (!h)
            continue;
        if (SuspendThread(h) != (DWORD)-1) {
            DWORD64 pcs[MAX_FRAMES];
            int n = 0, i;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &c)) {
                n = walk(&c, pcs);
            }
            ResumeThread(h);
            fprintf(stderr, "[HOSTSTACK] thread %lu\n", (unsigned long)te.th32ThreadID);
            for (i = 0; i < n; i++)
                frame_line(te.th32ThreadID, i, pcs[i]);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    fflush(stderr);
}
#endif
