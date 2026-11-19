/*
 * fitfill - "Zoom to Fit" fills the frame (EXStar Hub).
 *
 * What it does (two mechanisms, one per vendor era):
 *
 *  A) Up to 2.0.0-45 - the zoom-optimization flag.
 *   Ctrl+D / "Zoom to Fit" normally leaves the model at ~35% of the
 *   window: the viewport's CustomizeManipulator runs in "zoom
 *   optimization" mode, which pins the camera distance instead of
 *   framing the model's bounding box. The vendor enables that mode once
 *   per view: Sn3DCanvasView::init (librenderkit.extensions.digit.dll)
 *   calls CustomizeManipulator::setIsZoomOptimization(true). This
 *   module achieves the same effect as the version-bound one-byte file
 *   patch that cleared the immediate: a detour on the EXPORTED setter
 *   forces the value to false for every caller, in every process that
 *   loads the renderer.
 *   NOTE: on 2.1.0-10 the flag became dead code (written, read by
 *   nobody - see docs/research/zoom-fit-optimization.md §6), so this
 *   hook alone no longer changes the behaviour there. It stays armed:
 *   a correct no-op on new builds, the actual fix on old ones.
 *
 *  B) From 2.1.0-10 - the scan-scale call site.
 *   The new fit pipeline consults RenderKitExtDigitSingleton's
 *   "scan scale" mode instead. The app configures it once from
 *   sn3d.digit.scenecontrol.dll, whose code reads:
 *       call  RenderKitExtDigitSingleton::instance()   ; rax
 *       mov   rcx, rax
 *       xor   edx, edx              <- bool argument: false
 *       call  [setEnableScanScale]  ; instance->setEnableScanScale(false)
 *   (verified 2.1.0-10, file offset 0xC3C65; the IAT slot behind the
 *   call resolves to ?setEnableScanScale@RenderKitExtDigitSingleton@).
 *   With scan scale off, fit keeps the camera at the pinned distance;
 *   forcing the argument to true restores full-frame fitting. This
 *   module rewrites the two argument-setup bytes IN MEMORY only:
 *   anchored through the import table (never a fixed offset), exact
 *   one match required, restart = stock.
 *
 * Mechanism summary (no offsets, no file patches):
 *   - worker polls until each target DLL appears in THIS process (they
 *     load dynamically; in processes without them the module stays
 *     silently idle);
 *   - A: setter located by export NAME substring (default needle
 *     "?setIsZoomOptimization@CustomizeManipulator@@", must be unique);
 *     host->hook (MinHook) detours the entry, the detour forwards the
 *     configured `force` value and logs the first N calls;
 *   - B: the IAT slot of the `ss_import` name imported from the
 *     `ss_dll` DLL is resolved in the scenecontrol image; its 2-byte
 *     bool-setup prologue (`33 D2` = xor edx,edx / `B2 xx` = mov dl,x)
 *     in front of `FF 15 <that slot>` is rewritten to match
 *     `ss_force`. Every raw-memory read runs under SEH.
 *
 * Configuration (modules\fitfill.ini):
 *   target    = librenderkit.dll
 *   needle    = ?setIsZoomOptimization@CustomizeManipulator@@
 *   force     = 0            value to pass instead (0 disables zoom
 *                            optimization -> fit fills the frame)
 *   log_calls = 6            log the first N set() calls
 *   scan_scale = 1           enable mechanism B
 *   ss_target  = sn3d.digit.scenecontrol.dll
 *   ss_dll     = extensions.digit     imported-from DLL (substring)
 *   ss_import  = ?setEnableScanScale@RenderKitExtDigitSingleton@
 *   ss_force   = 1           bool to force at the call site
 *   verbose   = 1
 * Enable/disable: [fitfill] enabled = 0|1 in modules.ini.
 *
 * Failure modes (silent for the app by design):
 *   - target module absent here   -> idle;
 *   - needle not unique/absent    -> no hook, logged once;
 *   - hook refused                -> no hook, logged;
 *   - scan-scale anchor != 1 stock/patched call site -> no patch, log.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "exstar_sdk.h"
#include "exstar_export.h"

#define MOD_VERSION "1.1.0"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "fitfill",
    "Zoom to Fit Fill",
    MOD_VERSION,
    "Zoom to Fit frames the model (forces zoom-optimization off and scan scale on)"
};

static const ExHost *g_host;

/* A) setter hook */
static char g_target[64] = "librenderkit.dll";
static char g_needle[96] = "?setIsZoomOptimization@CustomizeManipulator@@";
static int  g_force      = 0;
static int  g_log_calls  = 6;
static int  g_verbose    = 1;
static int  g_done;

/* B) scan-scale call site */
static int  g_ssWant     = 1;
static char g_ssTarget[64] = "sn3d.digit.scenecontrol.dll";
static char g_ssDll[64]    = "extensions.digit";
static char g_ssImport[96] = "?setEnableScanScale@RenderKitExtDigitSingleton@";
static int  g_ssForce      = 1;
static int  g_ssDone;

typedef void (__cdecl *set_flag_fn)(void *, int);
static set_flag_fn g_orig;

static volatile LONG g_calls;

/* ------------------------------------------------------------------ */
/* A) the detour                                                       */
/* ------------------------------------------------------------------ */

static void __cdecl detour(void *self, int value)
{
    int v = value ? 1 : 0;
    int forced = g_force ? 1 : 0;

    {
        LONG n = InterlockedIncrement(&g_calls);
        if (n <= g_log_calls)
            g_host->log("fitfill: setIsZoomOptimization(%d) -> forwarding %d",
                        v, forced);
    }
    g_orig(self, forced);
}

/* ------------------------------------------------------------------ */
/* B) scan-scale call site - IAT-anchored in-memory byte patch         */
/* ------------------------------------------------------------------ */

/* Locate the single IAT slot in `mod` that imports an export whose name
 * contains `nameNeedle` from a DLL whose name contains `dllNeedle`
 * (substrings, lowercase as stored in the import table). Slot address
 * (in the mapped image) is returned in *slotOut. Ambiguity or absence
 * returns NULL with *hitsOut telling how many names matched. All raw
 * reads run under SEH. */
static void *ex_iatSlot(HMODULE mod, const char *dllNeedle,
                        const char *nameNeedle, void **slotOut, int *hitsOut,
                        char *nameOut, unsigned nameCap)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    DWORD impRva, size;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    int i;

    *slotOut = NULL;
    *hitsOut = 0;
    if (nameOut && nameCap)
        nameOut[0] = 0;
    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return NULL;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return NULL;
        impRva = nt->OptionalHeader
                     .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        size = nt->OptionalHeader
                   .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
        if (!impRva || !size)
            return NULL;
        imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + impRva);
        for (i = 0; i < 200 && imp[i].Name; i++) {
            const char *dll = (const char *)(base + imp[i].Name);
            PIMAGE_THUNK_DATA names;
            DWORD j, ftRva, oftRva;

            if (!strstr(dll, dllNeedle))
                continue;
            ftRva  = imp[i].FirstThunk;
            oftRva = imp[i].OriginalFirstThunk ? imp[i].OriginalFirstThunk
                                               : imp[i].FirstThunk;
            if (!ftRva)
                continue;
            names = (PIMAGE_THUNK_DATA)(base + oftRva);
            for (j = 0; j < 8192 && names[j].u1.AddressOfData; j++) {
                const char *nm;
                if ((names[j].u1.Ordinal >> 63) & 1)
                    continue;               /* import by ordinal: no name */
                nm = (const char *)(base + (DWORD)(DWORD_PTR)
                                          names[j].u1.AddressOfData + 2);
                if (strstr(nm, nameNeedle)) {
                    (*hitsOut)++;
                    if (*hitsOut > 1)
                        return NULL;        /* ambiguous: refuse          */
                    *slotOut = base + ftRva + (DWORD)j * 8;
                    if (nameOut && nameCap) {
                        strncpy(nameOut, nm, nameCap - 1);
                        nameOut[nameCap - 1] = 0;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return *slotOut;
}

/* Read a page-guarded block into `dst` (SEH); returns 0 on fault. */
static int safeRead(const void *src, void *dst, size_t len)
{
    int ok = 1;
    __try {
        memcpy(dst, src, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    return ok;
}

/* One scenecontrol image: find every call
 *     `48 8B C8 <bool-setup> FF 15 <rel32 -> slot>`
 * where <bool-setup> is stock `33 D2` (xor edx,edx = false) or already
 * `B2 xx` (mov dl,x). Rewrites the setup bytes when there is exactly
 * one such site and it is not what we want yet. */
static void tryScanScale(void)
{
    HMODULE mod = GetModuleHandleA(g_ssTarget);
    unsigned char *base;
    unsigned char *slot = NULL;
    int hits = 0;
    char impName[192];
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    unsigned char *hit = NULL;
    int nStock = 0, nOther = 0, i;

    if (g_ssDone || !mod)
        return;
    base = (unsigned char *)mod;

    if (!ex_iatSlot(mod, g_ssDll, g_ssImport, (void **)&slot, &hits,
                    impName, sizeof impName)) {
        if (g_verbose)
            g_host->log("fitfill: scan-scale import \"%s\" from \"%s\" in %s "
                        "matched %d name(s) - staying out of it",
                        g_ssImport, g_ssDll, g_ssTarget, hits);
        g_ssDone = 1;
        return;
    }

    nStock = nOther = 0;
    hit = NULL;
    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96)
            return;
        for (i = 0, sec = IMAGE_FIRST_SECTION(nt);
             i < nt->FileHeader.NumberOfSections; i++, sec++) {
            unsigned char *p;
            size_t len, off;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;                 /* scan read-only code/rodata  */
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            if (len < 13)
                continue;
            for (off = 0; off + 13 <= len; off++) {
                unsigned char w[13];
                int rel;
                unsigned char *tgt;

                if (p[off] != 0x48 || p[off + 1] != 0x8B || p[off + 2] != 0xC8
                    || p[off + 5] != 0xFF || p[off + 6] != 0x15)
                    continue;             /* fast reject before the read  */
                if (!safeRead(p + off, w, sizeof w))
                    break;                /* unreadable tail of section   */
                if (w[3] == 0x33 && w[4] == 0xD2)
                    ;                     /* stock:  xor edx,edx          */
                else if (w[3] == 0xB2)
                    ;                     /* a mov dl,imm8 (any value)    */
                else
                    continue;
                /* the window starts at the preceding `mov rcx,rax`; the
                 * call itself is `FF 15 disp32` at off+5..10, so its
                 * next-instruction (rip) address is off+11          */
                rel = (int)((unsigned)w[7] | ((unsigned)w[8] << 8)
                            | ((unsigned)w[9] << 16) | ((unsigned)w[10] << 24));
                tgt = p + off + 11 + rel; /* FF 15 rip-rel target         */
                if (tgt != slot)
                    continue;             /* different import: not ours   */
                if (w[3] == 0x33)
                    nStock++;
                else
                    nOther++;
                if (!hit)
                    hit = p + off;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                           /* image unreadable             */
    }

    if (!nStock && !nOther) {
        if (g_verbose)
            g_host->log("fitfill: %s mapped but no setEnableScanScale call "
                        "site found (vendor code changed?) - leaving it",
                        g_ssTarget);
        g_ssDone = 1;
        return;
    }
    if (nStock > 1 || (nStock && nOther) || nOther > 1) {
        g_host->log("fitfill: %s has %d stock + %d already-forced call "
                    "site(s) - refusing to guess, app stays as is",
                    g_ssTarget, nStock, nOther);
        g_ssDone = 1;
        return;
    }
    {
        unsigned char want[2];
        want[0] = 0xB2;
        want[1] = (unsigned char)(g_ssForce ? 1 : 0);
        if (hit[3] == want[0] && hit[4] == want[1]) {
            if (g_verbose)
                g_host->log("fitfill: scan scale already forced to %d at "
                            "%s+0x%lX (before us) - nothing to do",
                            g_ssForce, g_ssTarget,
                            (unsigned long)(hit - base + 3));
            g_ssDone = 1;
            return;
        }
        {
            DWORD oldp;
            int patched = 0;

            __try {
                if (VirtualProtect(hit + 3, 2, PAGE_READWRITE, &oldp)) {
                    hit[3] = want[0];
                    hit[4] = want[1];
                    VirtualProtect(hit + 3, 2, oldp, &oldp);
                    FlushInstructionCache(GetCurrentProcess(), hit + 3, 2);
                    patched = 1;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                patched = 0;
            }
            if (patched)
                g_host->log("fitfill: scan scale forced to %d at %s+0x%lX "
                            "(setEnableScanScale call site: the bool "
                            "argument is now %s) - %s",
                            g_ssForce, g_ssTarget,
                            (unsigned long)(hit - base + 3),
                            g_ssForce ? "true" : "false", impName);
            else if (g_verbose)
                g_host->log("fitfill: %s call site found but the page "
                            "refused the write - app stays as is",
                            g_ssTarget);
        }
    }
    g_ssDone = 1;
}

/* ------------------------------------------------------------------ */
/* install + worker                                                    */
/* ------------------------------------------------------------------ */

static void tryInstall(void)
{
    HMODULE mod;
    char name[256];
    int hits = 0;
    void *target;
    void *tramp = NULL;

    if (g_done)
        return;
    mod = GetModuleHandleA(g_target);
    if (!mod)
        return;
    target = ex_findExport(mod, g_needle, name, sizeof name, &hits);
    if (!target) {
        if (g_verbose)
            g_host->log("fitfill: %s loaded but needle \"%s\" matched "
                        "%d export(s) - staying idle", g_target, g_needle,
                        hits);
        g_done = 1;
        return;
    }
    if (g_host->hook(target, (void *)detour, &tramp) && tramp) {
        g_orig = (set_flag_fn)tramp;
        g_done = 1;
        g_host->log("fitfill: hooked %s (target %s), forcing value %d",
                    name, g_target, g_force);
    } else if (g_verbose) {
        g_host->log("fitfill: host->hook refused %s - staying idle", name);
        g_done = 1;
    }
}

static unsigned WINAPI worker(LPVOID arg)
{
    ULONGLONG t0 = GetTickCount64();

    (void)arg;
    while (!(g_done && (!g_ssWant || g_ssDone))) {
        tryInstall();
        if (g_ssWant)
            tryScanScale();
        if ((g_done && (!g_ssWant || g_ssDone))
            || GetTickCount64() - t0 > 1800000)
            break;
        Sleep(GetTickCount64() - t0 < 60000 ? 50 : 1000);
    }
    if (g_verbose) {
        if (!g_done)
            g_host->log("fitfill: %s never appeared in this process (%lu) - "
                        "idle", g_target, (unsigned long)GetCurrentProcessId());
        if (g_ssWant && !g_ssDone)
            g_host->log("fitfill: %s never appeared in this process (%lu) - "
                        "scan scale untouched", g_ssTarget,
                        (unsigned long)GetCurrentProcessId());
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* module entry points                                                 */
/* ------------------------------------------------------------------ */

EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    char buf[256];
    unsigned tid;
    HANDLE th;

    g_host = host;

    if (host->cfg_string(host, "target", buf, sizeof buf) && buf[0])
        strncpy(g_target, buf, sizeof g_target - 1);
    if (host->cfg_string(host, "needle", buf, sizeof buf))
        strncpy(g_needle, buf, sizeof g_needle - 1);
    g_force     = host->cfg_int(host, "force", 0);
    g_log_calls = host->cfg_int(host, "log_calls", 6);
    g_verbose   = host->cfg_int(host, "verbose", 1);
    g_ssWant    = host->cfg_int(host, "scan_scale", 1);
    g_ssForce   = host->cfg_int(host, "ss_force", 1);
    if (host->cfg_string(host, "ss_target", buf, sizeof buf) && buf[0])
        strncpy(g_ssTarget, buf, sizeof g_ssTarget - 1);
    if (host->cfg_string(host, "ss_dll", buf, sizeof buf) && buf[0])
        strncpy(g_ssDll, buf, sizeof g_ssDll - 1);
    if (host->cfg_string(host, "ss_import", buf, sizeof buf) && buf[0])
        strncpy(g_ssImport, buf, sizeof g_ssImport - 1);
    if (g_log_calls < 0)
        g_log_calls = 0;
    if ((!g_target[0] || !g_needle[0]) && (!g_ssWant || !g_ssTarget[0]))
        return 1;

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("fitfill v%s armed in pid=%lu: hook \"%s\" in %s (force=%d), "
              "scan scale %s in %s (force=%d)",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_needle, g_target, g_force,
              g_ssWant ? g_ssImport : "disabled", g_ssTarget, g_ssForce);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* the detour and the patched byte die with the process */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;
    (void)reason;
    (void)r;
    return TRUE;                         /* never fail */
}
