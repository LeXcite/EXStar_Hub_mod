/*
 * fitfill - "Zoom to Fit" fills the frame (EXStar Hub).
 *
 * What it does:
 *   Ctrl+D / "Zoom to Fit" normally leaves the model at ~35% of the
 *   window: the viewport's CustomizeManipulator runs in "zoom
 *   optimization" mode, which pins the camera distance instead of
 *   framing the model's bounding box. The vendor enables that mode once
 *   per view: Sn3DCanvasView::init (librenderkit.extensions.digit.dll)
 *   calls CustomizeManipulator::setIsZoomOptimization(true) - byte-
 *   exactly where the friend's version-bound one-byte file patch turned
 *   the immediate 1 into 0. This module achieves the same effect
 *   structurally: a detour on the EXPORTED setter forces the value to
 *   false for every caller, in every process that loads the renderer.
 *
 * Mechanism (no offsets, no file patches):
 *   - worker polls until librenderkit.dll appears in THIS process
 *     (the renderer loads dynamically; in processes without it the
 *     module stays silently idle);
 *   - the setter is located by export NAME substring (default needle
 *     "?setIsZoomOptimization@CustomizeManipulator@@", must be unique);
 *   - host->hook (MinHook) detours the entry; the detour forwards the
 *     configured `force` value (0 = disable the mode) and logs the
 *     first N calls - that log line is the live verification trail;
 *   - the field behind the setter (this+0x138) is documented by RE,
 *     never written directly: whatever logic reads it sees a consistent
 *     value because every write in the vendor build goes through this
 *     setter (verified: the only discovered call site is the init call;
 *     the paired getter reads the same byte).
 *
 * Configuration (modules\fitfill.ini):
 *   target   = librenderkit.dll
 *   needle   = ?setIsZoomOptimization@CustomizeManipulator@@
 *   force    = 0            value to pass instead (0 disables zoom
 *                           optimization -> fit fills the frame)
 *   log_calls = 6           log the first N set() calls
 *   verbose  = 1
 * Enable/disable: [fitfill] enabled = 0|1 in modules.ini.
 *
 * Failure modes (silent for the app by design):
 *   - target module absent here   -> idle;
 *   - needle not unique/absent    -> no hook, logged once;
 *   - hook refused                -> no hook, logged.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "exstar_sdk.h"
#include "exstar_export.h"

#define MOD_VERSION "1.0.0"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "fitfill",
    "Zoom to Fit Fill",
    MOD_VERSION,
    "Zoom to Fit frames the model to the viewport (forces zoom-optimization off)"
};

static const ExHost *g_host;

static char g_target[64] = "librenderkit.dll";
static char g_needle[96] = "?setIsZoomOptimization@CustomizeManipulator@@";
static int  g_force      = 0;
static int  g_log_calls  = 6;
static int  g_verbose    = 1;
static int  g_done;

typedef void (__cdecl *set_flag_fn)(void *, int);
static set_flag_fn g_orig;

static volatile LONG g_calls;

/* ------------------------------------------------------------------ */
/* the detour                                                          */
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
    while (!g_done) {
        tryInstall();
        if (g_done || GetTickCount64() - t0 > 1800000)
            break;
        Sleep(GetTickCount64() - t0 < 60000 ? 50 : 1000);
    }
    if (!g_done && g_verbose)
        g_host->log("fitfill: %s never appeared in this process (%lu) - "
                    "idle", g_target, (unsigned long)GetCurrentProcessId());
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
    if (g_log_calls < 0)
        g_log_calls = 0;
    if (!g_target[0] || !g_needle[0])
        return 1;

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("fitfill v%s armed in pid=%lu: %s \"%s\", force=%d",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_target, g_needle, g_force);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* the detour dies with the process */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;
    (void)reason;
    (void)r;
    return TRUE;                         /* never fail */
}
