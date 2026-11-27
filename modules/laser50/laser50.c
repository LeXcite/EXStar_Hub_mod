/*
 * laser50 - the full "50 laser lines" feature (EXStar Hub).
 *
 * One module, two coupled halves - they only make sense together:
 *
 *   [lines]   the matching pipeline keeps laser plane ids in a window:
 *             the scan platform calls the exported
 *             Sn3DAlgorithm::set_match_plane_id_vulkan(ctx,
 *             &startId, &endId, &startIdR, &endIdR) with (3, 21, 3, 21)
 *             - 19+19 lines of the 25+25 the optics project. A detour
 *             (host->hook, MinHook) at the export (located by NAME,
 *             include\exstar_export.h) swaps exactly that tuple for
 *             (0, 24, 0, 24); any other call (e.g. the 7-line mode)
 *             passes through untouched.
 *
 *   [caption] the Light Source Mode button then still reads the vendor
 *             nominal "39 lines": the laser QML labels it with
 *             qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)
 *             (= 39; the hanzi source string IS translated via the
 *             vendor catalogs, e.g. "%1线" -> "%1 lines"). The module
 *             rewrites ONLY the formatted number to 50 at DLL map time
 *             (compressed qrc record inside ScanPage_E7.dll, shared
 *             machinery include\exstar_qrc.h), so the button keeps the
 *             UI language and reads "50 lines" - now also a TRUE label.
 *             Rewriting the translated source string itself is the one
 *             thing that must never be done: it breaks the catalog
 *             lookup and leaks raw text into every locale (the
 *             standalone captions v1.0.0 bug).
 *
 * Process model: the module is loaded everywhere the host runs. The
 * [lines] half idles unless algorithmLzy.dll appears (scan pipeline
 * processes); the [caption] half idles unless ScanPage_E7.dll appears
 * (the UI process). Both halves can live in one process - each is
 * independently armed.
 *
 * Configuration (modules\laser50.ini):
 *   target_module   = algorithmLzy.dll        [lines] DLL with export
 *   needle          = set_match_plane_id_vulkan@
 *   strict          = 1      only remap the exact from_* tuple
 *   from_left_start = 3      from_left_end  = 21
 *   from_right_start= 3      from_right_end = 21
 *   to_left_start   = 0      to_left_end    = 24
 *   to_right_start  = 0      to_right_end   = 24
 *   log_calls       = 6      log the first N calls with values
 *   captions        = 1      0 = keep the stock "39" on the button
 *   cap_target      = ScanPage_E7.dll         [caption] DLL with the qrc
 *   cap_anchor      = ScanSettingModelProviderLaser
 *   cap_pattern     = qsTr("%1线").arg($scanSettingModelLaser.crossLaserLineShowNum)
 *   cap_replacement = qsTr("%1线").arg(50)
 *   cap_expected    = 1
 *   verbose         = 1
 * Enable/disable: [laser50] enabled = 0|1 in modules.ini (the Mod
 * Manager keeps the host sibling whitelist in sync: the pipeline lives
 * in scan processes, the caption in the UI process).
 *
 * Failure modes (silent for the app by design):
 *   - target module never loads here        -> that half idle, logged;
 *   - needle matches zero or several exports-> no hook, logged;
 *   - no qrc record has anchor+pattern exactly cap_expected times
 *                                           -> no caption patch, logged;
 *   - caption re-deflate bigger than the record -> no patch (sizes);
 *   - caption verification inflate mismatch -> record restored, logged;
 *   - vendor changed the tuple              -> strict mode passes the
 *     original values through (logged on first mismatch).
 *
 * Offline dry-runs: dumpbin /exports for the needle uniqueness;
 * tools\replace_lab.exe for the caption (the live packed size must
 * equal the predicted one byte for byte). See README.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exstar_sdk.h"
#include "exstar_export.h"
#include "exstar_qrc.h"

#define MOD_VERSION "1.1.0"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "laser50",
    "Laser 25x25",
    MOD_VERSION,
    "all 25+25 laser lines (plane-id remap) + the button reads 50 lines"
};

static const ExHost *g_host;
static int  g_verbose = 1;

/* ================= [lines] the plane-id detour ===================== */

static char g_target[64]   = "algorithmLzy.dll";
static char g_needle[96]   = "set_match_plane_id_vulkan@";
static int  g_strict       = 1;
static int  g_from[4]      = { 3, 21, 3, 21 };
static int  g_to[4]        = { 0, 24, 0, 24 };
static int  g_log_calls    = 6;
static int  g_done;                       /* hook installed (or refused) */

typedef int (__cdecl *set_planes_fn)(void *, const int *, const int *,
                                     const int *, const int *);
static set_planes_fn g_orig;

static volatile LONG g_calls;               /* total calls through       */
static volatile LONG g_logged;              /* per-remap log budget      */
static volatile LONG g_passthrough_logged;  /* one strict-miss notice    */

static int __cdecl detour(void *ctx, const int *a, const int *b,
                          const int *c, const int *d)
{
    int v[4];
    int readable = 1;
    int match;

    v[0] = v[1] = v[2] = v[3] = 0;
    __try {
        if (a) v[0] = *a;
        if (b) v[1] = *b;
        if (c) v[2] = *c;
        if (d) v[3] = *d;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readable = 0;
    }

    /* the four references must all be real: without them the original
     * will do whatever it does with nulls - not our business */
    if (!readable || !a || !b || !c || !d)
        return g_orig(ctx, a, b, c, d);

    match = (v[0] == g_from[0] && v[1] == g_from[1]
             && v[2] == g_from[2] && v[3] == g_from[3]);

    if (!g_strict)
        match = 1;

    {   /* diagnostics: first N calls always, every remap after that */
        LONG n = InterlockedIncrement(&g_calls);
        if (n <= g_log_calls
            || (match && InterlockedIncrement(&g_logged) <= g_log_calls)) {
            g_host->log("laser50: call %ld: (%d,%d,%d,%d)%s",
                        (long)n, v[0], v[1], v[2], v[3],
                        match ? " -> remap" : " (pass through)");
        }
    }

    if (!match) {
        if (g_strict && InterlockedExchange(&g_passthrough_logged, 1) == 0)
            g_host->log("laser50: values differ from from_* (%d,%d,%d,%d)"
                        " - strict pass through (check laser50.ini)",
                        v[0], v[1], v[2], v[3]);
        return g_orig(ctx, a, b, c, d);
    }

    return g_orig(ctx, &g_to[0], &g_to[1], &g_to[2], &g_to[3]);
}

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
            g_host->log("laser50: %s loaded but needle \"%s\" matched "
                        "%d export(s) - staying idle", g_target, g_needle,
                        hits);
        g_done = 1;                          /* decided: no hook         */
        return;
    }
    if (g_host->hook(target, (void *)detour, &tramp) && tramp) {
        g_orig = (set_planes_fn)tramp;
        g_done = 1;
        g_host->log("laser50: hooked %s (target %s): strict=%d "
                    "(%d,%d,%d,%d) -> (%d,%d,%d,%d)",
                    name, g_target, g_strict,
                    g_from[0], g_from[1], g_from[2], g_from[3],
                    g_to[0], g_to[1], g_to[2], g_to[3]);
    } else if (g_verbose) {
        g_host->log("laser50: host->hook refused %s - staying idle", name);
        g_done = 1;
    }
}

/* ================= [caption] qrc text rewrite ====================== */
/* (shared framing: [u32 BE rawSize][zlib stream] records in read-only  *
 *  sections; the next record's boundary may never move)                */

static int  g_captionsOn = 1;
static char g_capTarget[64] = "ScanPage_E7.dll";
static char g_capAnchor[96] = "ScanSettingModelProviderLaser";
/* UTF-8 bytes as escapes so the source needs no codepage luck. The
 * translated qsTr source string ("%1线") is KEPT (vendor catalogs map
 * it: EN "%1 lines"); only the formatted number becomes 50. */
static char g_capPattern[192] =
    "qsTr(\"%1\xE7\xBA\xBF\").arg($scanSettingModelLaser.crossLaserLineShowNum)";
static char g_capRepl[192] =
    "qsTr(\"%1\xE7\xBA\xBF\").arg(50)";
static int  g_capExpected = 1;
static int  g_capDone;                      /* patched or decided off    */

static int capRecord(unsigned char *q, size_t avail, unsigned long recOff,
                     void *ctx)
{
    const char *who = (const char *)ctx;
    size_t patLen = strlen(g_capPattern), replLen = strlen(g_capRepl);
    size_t anchorLen = strlen(g_capAnchor);
    unsigned raw;
    size_t lc, used = 0, newRaw, w, src;
    unsigned char *text, *patched;
    int anchorHits, patHits;
    exq_result res;

    text = exq_inflateAt(q, avail, &raw, &used);
    if (!text)
        return 0;
    lc = used;

    anchorHits = exq_countBytes(text, raw, g_capAnchor, anchorLen);
    patHits    = exq_countBytes(text, raw, g_capPattern, patLen);
    if (!anchorHits || !patHits) {
        free(text);
        return 0;
    }
    if (anchorHits != 1 || patHits != g_capExpected) {
        if (g_verbose)
            g_host->log("laser50: caption record in %s @0x%lX has %d "
                        "anchor + %d pattern hits (expected 1/%d) - left "
                        "stock", who, recOff, anchorHits, patHits,
                        g_capExpected);
        free(text);
        return 0;
    }
    if (replLen && exq_memmem(text, raw, g_capRepl, replLen)) {
        free(text);                          /* already patched          */
        return 1;
    }

    if (replLen >= patLen)
        newRaw = raw + (size_t)patHits * (replLen - patLen);
    else
        newRaw = raw - (size_t)patHits * (patLen - replLen);

    patched = (unsigned char *)malloc(newRaw);
    if (!patched) {
        free(text);
        return 0;
    }
    for (w = 0, src = 0; src < raw; ) {
        if (src + patLen <= raw
            && !memcmp(text + src, g_capPattern, patLen)) {
            memcpy(patched + w, g_capRepl, replLen);
            w += replLen;
            src += patLen;
        } else {
            patched[w++] = text[src++];
        }
    }
    free(text);

    exq_writeRecord(q, lc, patched, newRaw, &res);
    if (res.ok)
        g_host->log("laser50: caption %s stream @0x%lX raw %u->%u, packed "
                    "%lu<-%lu, %d caption(s) rewritten (ok)",
                    who, recOff, raw, (unsigned)newRaw, (unsigned long)lc,
                    res.packed, patHits);
    else if (!res.fits) {
        if (g_verbose)
            g_host->log("laser50: caption record in %s @0x%lX would need "
                        "%lu B > %lu B - left stock", who, recOff, res.need,
                        (unsigned long)lc);
    } else if (res.protErr && g_verbose) {
        g_host->log("laser50: caption record in %s @0x%lX "
                    "VirtualProtect failed (%lu)", who, recOff, res.protErr);
    } else if (res.verifyFail) {
        g_host->log("laser50: caption VERIFY FAILED in %s @0x%lX - record "
                    "restored, left stock", who, recOff);
    }
    free(patched);
    return res.ok;
}

static int capPatchImage(HMODULE mod, const char *who)
{
    return exq_scanImage(mod, 0x800, capRecord, (void *)who);
}

/* ================= armed: notify + worker ========================== */

static void l50OnMap(HMODULE base, const char *name, void *ctx)
{
    (void)ctx;
    /* the caption half must land before the Qt engine parses the QML:
     * patch inside the notification (pre-DllMain) when the qrc carrier
     * is mapped */
    if (!g_captionsOn || g_capDone || !exq_zlibOk)
        return;
    if (!exq_ciContains(name, g_capTarget))
        return;
    if (capPatchImage(base, name))
        g_capDone = 1;
}

static unsigned WINAPI worker(LPVOID arg)
{
    int notif = 0;
    int capQuiet = 0;
    ULONGLONG t0 = GetTickCount64();

    (void)arg;
    if (g_captionsOn && exq_zlibOk)
        notif = exq_onMapped(l50OnMap, NULL);
    while (!g_done || (!capQuiet && g_captionsOn && !g_capDone)) {
        tryInstall();
        if (g_captionsOn && !g_capDone && exq_zlibOk) {
            HMODULE h = GetModuleHandleA(g_capTarget);

            if (h && capPatchImage(h, g_capTarget))
                g_capDone = 1;
        }
        if (GetTickCount64() - t0 > 1800000)
            break;
        /* the caption half only matters in the UI process: after the
         * app had plenty of time to load its UI DLLs, stop probing so
         * scan processes stay quiet */
        if (!capQuiet && GetTickCount64() - t0 > 120000)
            if (g_captionsOn && !g_capDone && !GetModuleHandleA(g_capTarget))
                capQuiet = 1;
    }
    if (!g_done && g_verbose)
        g_host->log("laser50: %s never appeared in this process (%lu) - "
                    "lines half idle", g_target,
                    (unsigned long)GetCurrentProcessId());
    if (notif == 0 && g_captionsOn && !g_capDone && g_verbose)
        g_host->log("laser50: caption target %s never patched here - "
                    "caption half idle", g_capTarget);
    return 0;
}

/* ================= module entry points ============================= */

EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    char buf[256];
    unsigned tid;
    HANDLE th;
    int i;
    static const char *fromKeys[4] = { "from_left_start", "from_left_end",
                                       "from_right_start",
                                       "from_right_end" };
    static const char *toKeys[4]   = { "to_left_start", "to_left_end",
                                       "to_right_start", "to_right_end" };

    g_host = host;

    if (host->cfg_string(host, "target_module", buf, sizeof buf))
        strncpy(g_target, buf, sizeof g_target - 1);
    if (host->cfg_string(host, "needle", buf, sizeof buf))
        strncpy(g_needle, buf, sizeof g_needle - 1);
    g_strict = host->cfg_int(host, "strict", 1);
    for (i = 0; i < 4; i++) {
        g_from[i] = host->cfg_int(host, fromKeys[i], g_from[i]);
        g_to[i]   = host->cfg_int(host, toKeys[i], g_to[i]);
    }
    g_log_calls = host->cfg_int(host, "log_calls", 6);
    g_verbose   = host->cfg_int(host, "verbose", 1);
    if (g_log_calls < 0)
        g_log_calls = 0;
    if (!g_target[0] || !g_needle[0])
        return 1;

    g_captionsOn = host->cfg_int(host, "captions", 1);
    if (host->cfg_string(host, "cap_target", buf, sizeof buf) && buf[0])
        strncpy(g_capTarget, buf, sizeof g_capTarget - 1);
    if (host->cfg_string(host, "cap_anchor", buf, sizeof buf) && buf[0])
        strncpy(g_capAnchor, buf, sizeof g_capAnchor - 1);
    if (host->cfg_string(host, "cap_pattern", buf, sizeof buf) && buf[0])
        strncpy(g_capPattern, buf, sizeof g_capPattern - 1);
    if (host->cfg_string(host, "cap_replacement", buf, sizeof buf))
        strncpy(g_capRepl, buf, sizeof g_capRepl - 1);  /* "" = delete   */
    g_capExpected = host->cfg_int(host, "cap_expected", 1);
    if (g_capExpected < 1)
        g_capExpected = 1;
    if (g_captionsOn && (!g_capTarget[0] || !g_capAnchor[0]
                         || !g_capPattern[0]))
        g_captionsOn = 0;

    if (g_captionsOn) {
        /* QML disk cache would serve stale compiled bindings (the retired
         * fps module's lesson) */
        SetEnvironmentVariableA("QML_DISABLE_DISK_CACHE", "1");
        exq_loadZlib();                    /* caption half off if absent */
    }

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("laser50 v%s armed in pid=%lu: %s \"%s\", strict=%d, "
              "(%d,%d,%d,%d) -> (%d,%d,%d,%d), captions=%d%s",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_target, g_needle, g_strict,
              g_from[0], g_from[1], g_from[2], g_from[3],
              g_to[0], g_to[1], g_to[2], g_to[3],
              g_captionsOn,
              (g_captionsOn && !exq_zlibOk) ? " (no zlib - off)" : "");
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* the detour and the patched streams live in process memory and
     * die with it */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;
    (void)reason;
    (void)r;
    return TRUE;                         /* never fail */
}
