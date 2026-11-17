/*
 * fps - real FPS counter on the ScanPage (EXStar Hub).
 *
 * What it does:
 *   The vendor shows a scan statistics panel whose "FPS" value comes from
 *   the capture pipeline (real_time_info_model, C++) and reads a constant
 *   ~80-90 regardless of what the UI is actually rendering. This module
 *   adds a small live "FPS: N" readout at the bottom right of the scan
 *   page (like the scan view overlays), counting the real UI frames via
 *   the window's frameSwapped signal.
 *
 * Mechanism:
 *   - LdrRegisterDllNotification (include\exstar_qrc.h): ScanPage_E7.dll
 *     is caught the instant it is mapped, before its DllMain and before
 *     Qt compiles its QML;
 *   - the scan page QML lives in a qrc record [u32 BE rawSize][zlib]
 *     inside the DLL's read-only image; the record inflates to the file
 *     that starts with the comment `/// ScanPage_E7`;
 *   - a Text { id: exFpsLabel ... } block (Connections -> frameSwapped
 *     frame counter + 1 s Timer sampler, anchors bottom-right) is
 *     inserted right after the root Item's `id: root` line;
 *   - the edited text goes through the shared safe write path
 *     (exq_writeRecord): level-9 re-deflate into the ORIGINAL compressed
 *     length, rawSize update, verify-by-reinflate, byte-exact restore on
 *     any mismatch. Nothing on disk changes; a restart restores stock.
 *
 * Configuration (modules\fps.ini):
 *   target       = ScanPage_E7.dll
 *   anchor       = /// ScanPage_E7     identifies the scan page stream
 *   insert_after = id: root            snippet lands after this line
 *   right        = 62                  right margin, px
 *   bottom       = 56                  bottom margin, px
 *   size         = 16                  font pixel size
 *   color        = "#8ecbff"           label colour (quotes: '#' starts
 *                                      a comment unless quoted)
 *   verbose      = 1                   log details to mod.log
 * Enable/disable: [fps] enabled = 0|1 in modules.ini.
 *
 * Failure modes (silent for the app by design):
 *   - anchor record absent / not unique  -> no patch, vendor restructured;
 *   - re-deflate bigger than the record  -> no patch (logged with sizes);
 *   - verification inflate mismatch      -> record restored, logged.
 *
 * Offline dry-run: tools\replace_lab.exe (see README) - the live packed
 * size must equal the predicted one byte for byte.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exstar_sdk.h"
#include "exstar_qrc.h"

#define MOD_VERSION   "1.0.0"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "fps",
    "Real FPS",
    MOD_VERSION,
    "live frame-rate readout on the scan page (counts real UI frames)"
};

static const ExHost *g_host;

static char g_target[64]    = "ScanPage_E7.dll";
static char g_anchor[64]    = "/// ScanPage_E7";
static char g_insert[64]    = "id: root";
static int  g_right         = 62;
static int  g_bottom        = 56;
static int  g_size          = 16;
static char g_color[32]     = "#8ecbff";
static int  g_verbose       = 1;
static int  g_done;                     /* stream patched (or refused)   */

/* rendered QML snippet, built once in ex_init from the ini knobs */
static char g_snip[4096];
static size_t g_snipLen;

/* ------------------------------------------------------------------ */
/* one record: insert the snippet after the marker line                */
/* ------------------------------------------------------------------ */

static int fpsRecord(unsigned char *q, size_t avail, unsigned long recOff,
                     void *ctx)
{
    const char *who = (const char *)ctx;
    unsigned raw;
    size_t lc, used = 0;
    unsigned char *text, *patched;
    unsigned char *a, *al, *ins;
    size_t insOff, newRaw;
    exq_result res;

    text = exq_inflateAt(q, avail, &raw, &used);
    if (!text)
        return 0;
    lc = used;

    /* the scan page file, exactly once, not patched yet */
    if (exq_countBytes(text, raw, g_anchor, strlen(g_anchor)) != 1
        || exq_memmem(text, raw, "exFpsLabel", 10) != NULL) {
        free(text);
        return 0;
    }
    a = (unsigned char *)exq_memmem(text, raw, g_anchor, strlen(g_anchor));
    al = (unsigned char *)exq_memmem(a, raw - (size_t)(a - text),
                                     g_insert, strlen(g_insert));
    if (!al) {
        if (g_verbose)
            g_host->log("fps: %s record @0x%lX has anchor but no \"%s\"",
                        who, recOff, g_insert);
        free(text);
        return 0;
    }
    ins = (unsigned char *)memchr((void *)al, '\n', raw - (size_t)(al - text));
    if (!ins) {
        free(text);
        return 0;
    }
    insOff = (size_t)(ins - text) + 1;
    newRaw = raw + g_snipLen;

    patched = (unsigned char *)malloc(newRaw);
    if (!patched) {
        free(text);
        return 0;
    }
    memcpy(patched, text, insOff);
    memcpy(patched + insOff, g_snip, g_snipLen);
    memcpy(patched + insOff + g_snipLen, text + insOff, raw - insOff);
    free(text);

    exq_writeRecord(q, lc, patched, newRaw, &res);
    if (res.ok)
        g_host->log("fps: %s stream @0x%lX raw %u->%u, packed %lu<-%lu (ok)",
                    who, recOff, raw, (unsigned)newRaw, (unsigned long)lc,
                    res.packed);
    else if (!res.fits) {
        if (g_verbose)
            g_host->log("fps: %s record @0x%lX would need %lu B > %lu B - "
                        "left stock (raw %u -> %u)", who, recOff, res.need,
                        (unsigned long)lc, raw, (unsigned)newRaw);
    } else if (res.protErr && g_verbose) {
        g_host->log("fps: %s record @0x%lX VirtualProtect failed (%lu)",
                    who, recOff, res.protErr);
    } else if (res.verifyFail) {
        g_host->log("fps: %s VERIFY FAILED @0x%lX - record restored, "
                    "left stock", who, recOff);
    }
    free(patched);
    return res.ok;
}

static int patchImage(HMODULE mod, const char *who)
{
    return exq_scanImage(mod, 0x8000, fpsRecord, (void *)who);
}

/* ------------------------------------------------------------------ */
/* DLL load notification + worker                                      */
/* ------------------------------------------------------------------ */

static void fpsOnMap(HMODULE base, const char *name, void *ctx)
{
    (void)ctx;
    if (g_done || !exq_ciContains(name, g_target))
        return;
    if (patchImage(base, name))
        g_done = 1;
}

static unsigned WINAPI worker(LPVOID arg)
{
    int notif;
    ULONGLONG t0 = GetTickCount64();

    (void)arg;
    notif = exq_onMapped(fpsOnMap, NULL);
    while (!g_done) {
        HMODULE h = GetModuleHandleA(g_target);

        if (h && patchImage(h, g_target))
            g_done = 1;
        if (g_done || GetTickCount64() - t0 > 300000)
            break;
        Sleep(GetTickCount64() - t0 < 10000 ? 20 : 500);
    }
    if (!g_done && g_verbose)
        g_host->log("fps: target %s never patched%s", g_target,
                    notif ? "" : " (no dll notification either)");
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

    if (host->cfg_string(host, "target", buf, sizeof buf))
        strncpy(g_target, buf, sizeof g_target - 1);
    if (host->cfg_string(host, "anchor", buf, sizeof buf))
        strncpy(g_anchor, buf, sizeof g_anchor - 1);
    if (host->cfg_string(host, "insert_after", buf, sizeof buf))
        strncpy(g_insert, buf, sizeof g_insert - 1);
    g_right  = host->cfg_int(host, "right", 62);
    g_bottom = host->cfg_int(host, "bottom", 56);
    g_size   = host->cfg_int(host, "size", 16);
    if (host->cfg_string(host, "color", buf, sizeof buf) && buf[0])
        strncpy(g_color, buf, sizeof g_color - 1);
    g_verbose = host->cfg_int(host, "verbose", 1);
    if (g_right < 0)  g_right = 0;
    if (g_bottom < 0) g_bottom = 0;
    if (g_size < 6)   g_size = 6;
    if (g_size > 64)  g_size = 64;

    if (!g_anchor[0] || !g_insert[0])
        return 1;

    /* QML disk cache would serve stale compiled bindings for the
     * un-patched source signature (popup_dim lesson) */
    SetEnvironmentVariableA("QML_DISABLE_DISK_CACHE", "1");

    exq_loadZlib();
    if (!exq_zlibOk)
        return 2;                         /* no zlib in process (yet?)   */

    g_snipLen = (size_t)_snprintf(g_snip, sizeof g_snip - 1,
        "    Text {\r\n"
        "        id: exFpsLabel\r\n"
        "        property int exFps: 0\r\n"
        "        property int exFrames: 0\r\n"
        "        z: 99999\r\n"
        "        anchors.right: parent.right\r\n"
        "        anchors.rightMargin: %d\r\n"
        "        anchors.bottom: parent.bottom\r\n"
        "        anchors.bottomMargin: %d\r\n"
        "        color: \"%s\"\r\n"
        "        font.pixelSize: %d\r\n"
        "        text: exFps > 0 ? \"FPS: \" + exFps : \"\"\r\n"
        "        Connections {\r\n"
        "            target: window\r\n"
        "            ignoreUnknownSignals: true\r\n"
        "            function onFrameSwapped() { exFpsLabel.exFrames++ }\r\n"
        "        }\r\n"
        "        Timer {\r\n"
        "            interval: 1000\r\n"
        "            running: exFpsLabel.visible\r\n"
        "            repeat: true\r\n"
        "            triggeredOnStart: true\r\n"
        "            onTriggered: { exFpsLabel.exFps = exFpsLabel.exFrames;"
        " exFpsLabel.exFrames = 0 }\r\n"
        "        }\r\n"
        "    }\r\n",
        g_right, g_bottom, g_color, g_size);
    if (g_snipLen == (size_t)-1)
        return 3;

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 4;
    CloseHandle(th);

    host->log("fps v%s armed in pid=%lu: target %s, margins %d/%d, "
              "font %d %s", MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_target, g_right, g_bottom, g_size, g_color);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* nothing to undo: the patched stream lives in the mapped DLL view
     * and dies with the process */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;
    (void)reason;
    (void)r;
    return TRUE;                         /* never fail */
}
