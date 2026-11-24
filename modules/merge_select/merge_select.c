/*
 * merge_select.c - choose which scans feed the common mesh.
 *
 * Version 1.1.0
 *
 * The problem (verified on product 2.1.0-10):
 *   Several scans can live in one project, and the vendor's "generate
 *   common mesh" step feeds EVERY scan whose project file says
 *   "generate a point cloud" into the merge. The single exported
 *   question behind that decision is
 *      Sn3DFile.dll!HandlePrjFile::isGenerateCloud(void) const -> bool
 *   and the only module that ever asks it is Sn3DMultiProjects.dll -
 *   its two common-mesh senders (Qt slots building the
 *   "sendPrjsMeshDataToFZ" payload) call it once per scan right before
 *   appending the scan's mesh to the payload: "yes" adds the scan's
 *   mesh/path data to meshList/pathList/uuidList; "no" takes the
 *   vendor's own "not generate cloud" branch, which leaves the scan's
 *   meshes out and reports its UUID in the payload key uuidHideList,
 *   the list PostProcess excludes from the fusion. Answering "no" is
 *   therefore the native, fully supported way to leave a scan out of
 *   the common mesh.
 *   (Full evidence chain incl. payload keys: docs/research/merge-common-mesh.md.)
 *   There is no stock way to drop one scan from just that step - the
 *   flag is fixed in the project data, no UI control edits it, and
 *   deleting the scan would also destroy its stored data. (The scan
 *   list's eye icon is pure scene visibility: neither sender ever
 *   reads it, verified across all vendor DLLs.)
 *
 * What this module does:
 *   One in-memory detour on that export. It answers "no cloud" for
 *   scans explicitly excluded in
 *      C:\Users\Public\Shining3d\EXStar Hub\merge-select.ini
 *   so the vendor's own merge code skips them; the vendor's "yes" is
 *   never overridden towards "included", so live scanning, storage and
 *   every unlisted scan behave exactly like stock.
 *
 *   Containment: the detour answers differently ONLY while the caller
 *   is inside Sn3DMultiProjects.dll (return-address check); calls from
 *   anywhere else, including Sn3DFile.dll itself, receive the pure
 *   vendor answer.
 *
 * Selection file (created automatically with a readable header):
 *   [Project_1\Scan_2]      ; scan folder: <project folder>\<scan folder>
 *   included = 0            ; 0 = leave this scan out of the common mesh
 *   Lookup order per scan: the scan's full folder path as the section
 *   name first, then "<parent folder>\<leaf folder>", then
 *   include-by-default. A [Settings] enabled = 0 master switch turns
 *   the module off at runtime without uninstalling it.
 *
 * Discovery aid: every scan seen during a common-mesh run is appended
 * once to `merge-select.map.txt` (same folder) with its ready-to-copy
 * section name and the decision, so exclusions can be written without
 * guessing any paths. The manager's "Scan roster" button (Merge
 * Selection card) turns that roster into include checkboxes and keeps
 * this file in sync - ticking a box and saving needs no restart, the
 * detour re-reads the .ini on every decision.
 *
 * Anchoring (project rule: no hardcoded RVAs):
 *   Everything resolves by exported name at runtime:
 *     Sn3DFile.dll   ?isGenerateCloud@HandlePrjFile@@QEAA_NXZ   (target)
 *                    ?getFolderPath@HandlePrjFile@@QEAA?AVQString@@XZ
 *     Qt5Core.dll    ?utf16@QString@@QEBAPEBGXZ, ??1QString@@QEAA@XZ
 *   Any missing name (vendor renamed it in some future build) makes
 *   the module log that once and stay out of the way.
 *
 * Safety:
 *   - the detour adds no failure paths of its own: the folder query
 *     and QString reads run under SEH and any fault falls back to the
 *     vendor's own answer;
 *   - it can only turn "yes" into "no" for an explicitly excluded,
 *   - existing scan, never the other way and never for an unlisted
 *     scan;
 *   - installation happens on one module thread only (the DLL-load
 *     notification merely wakes it): MinHook is never touched while
 *     the loader lock is held, and concurrent install races cannot
 *     happen;
 *   - selection state lives in a plain .ini the user owns; deleting
 *     it restores stock behaviour; disabling the module in the manager
 *     reverts at the next start without touching any file;
 *   - no vendor file is modified; the detour lives in process memory.
 *
 * Compatibility note: a community patchset ships a similar feature
 * driven by `mesh-selection.ini` with scan-UUID sections in the same
 * folder. The two mechanisms are independent and AND together (either
 * one excluding a scan takes it out of the merge). This module never
 * reads or writes those UUID sections.
 *
 * History:
 *   1.0.0  initial version; vendor merge path (common-mesh collection
 *          + native "no cloud" branch) verified statically on
 *          2.1.0-10, detour self-tested in the running hub.
 *   1.1.0  every contained gate call is logged (vendor answer, reason,
 *          outcome) so one mesh run proves the path it took; both
 *          "sendPrjsMeshDataToFZ" senders and the uuidHideList payload
 *          key verified in the disassembly; manager gains the roster
 *          dialog that edits the selection file.
 */

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <intrin.h>

#include "exstar_sdk.h"

#define MOD_VERSION   "1.3.0"

#define SEL_FILE  L"C:\\Users\\Public\\Shining3d\\EXStar Hub\\merge-select.ini"
#define MAP_FILE  L"C:\\Users\\Public\\Shining3d\\EXStar Hub\\merge-select.map.txt"

#define PATH_CAP  512                    /* wchar path/key buffers      */
#define SEEN_MAX  64                     /* dedupe window for map+log   */

static const ExHost *g_host;
static int           g_verbose = 1;
static int           g_eyewatch = 1;
static int           g_eyeGate = 1;
static wchar_t       g_selFile[PATH_CAP];
static wchar_t       g_mapFile[PATH_CAP];

/* vendor functions, resolved by exported name */
typedef unsigned char (*FnIsCloud)(void *self);
typedef void *(*FnFolderRet)(void *ret, void *self);   /* QString sret  */
typedef const unsigned short *(*FnUtf16)(const void *q);
typedef void (*FnDtor)(void *q);

static FnIsCloud   g_origIsCloud;        /* trampoline from host->hook  */
static FnFolderRet g_getFolderPath;
static FnFolderRet g_getTxPath;          /* fallback identity, impl+0x60*/
static FnUtf16     g_qUtf16;
static FnDtor      g_qDtor;

/* the common-mesh code lives in this image; callers from anywhere else
 * (project open/save, Sn3DFile internals) keep seeing the vendor answer */
static unsigned char *g_mpBase;
static size_t         g_mpSize;

static volatile LONG g_installed;        /* detour in place             */
static volatile LONG g_broken;           /* resolve/hook failed: idle   */
static volatile LONG g_reportBusy;       /* serialize map/log writes    */
static volatile LONG g_logLeft = 200;    /* per-call decision log budget*/
static HANDLE        g_wake;             /* dll-load event for worker   */

/* scan keys already reported to the map file / log */
static wchar_t g_seen[SEEN_MAX][PATH_CAP];
static int     g_seenN;

/* ---- probe (diagnostics): who asks, from where ---------------------
 * Every distinct return address that reaches the hooked vendor fns is
 * logged once as "module+RVA".  The scan-page mesh flow does not go
 * through the merge senders in Sn3DMultiProjects; this maps who does. */
#define SITE_MAX 40
static void         *g_probe[SITE_MAX];
static int           g_probeN;
static volatile LONG g_probeBusy;
static volatile LONG g_probeLeft = 60;

static void probeLog(const char *tag, void *ra, void *self)
{
    int i;
    unsigned long long impl = 0;

    if (InterlockedCompareExchange(&g_probeBusy, 1, 0) != 0)
        return;
    for (i = 0; i < g_probeN; i++)
        if (g_probe[i] == ra) {
            InterlockedExchange(&g_probeBusy, 0);
            return;
        }
    if (g_probeN < SITE_MAX)
        g_probe[g_probeN++] = ra;
    if (g_verbose && InterlockedDecrement(&g_probeLeft) >= 0) {
        char mod[64] = "?";
        unsigned off = 0;
        HMODULE h = NULL;
        __try {
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)ra, &h) && h) {
                char path[MAX_PATH];
                char *b;
                if (GetModuleFileNameA(h, path, sizeof path) > 0) {
                    b = strrchr(path, '\\');
                    strcpy_s(mod, sizeof mod, b ? b + 1 : path);
                    off = (unsigned)((unsigned char *)ra
                                     - (unsigned char *)h);
                }
            }
            if (self)
                impl = *(unsigned long long *)
                           ((unsigned char *)self + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_host->log("merge_select: probe %s %s+0x%X  self=%p impl=%p",
                    tag, mod, off, self, (void *)impl);
    }
    InterlockedExchange(&g_probeBusy, 0);
}

/* (v1.1.3) mesh-getter detours removed: hot-path hang risk, see
 * tryInstall.  probeLog() above stays for the isGenerateCloud probe. */

static const ExModuleInfo g_info = {
    EX_ABI_VERSION, "merge_select", "Merge Selection", MOD_VERSION,
    "per-scan include switch for the common mesh (merge-select.ini)"
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/* copy src into dst and strip trailing backslashes (keep bare "C:\") */
static void cleanPath(const wchar_t *src, wchar_t *dst, int cap)
{
    int n = 0;
    while (src[n] && n < cap - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
    while (n > 3 && dst[n - 1] == L'\\')
        dst[--n] = 0;
}

/* "C:\root\Project_1\Scan_2" -> "Project_1\Scan_2";
 * false when the path has no parent folder                      */
static int shortKey(const wchar_t *path, wchar_t *out, int cap)
{
    const wchar_t *slash, *prev;
    int i, len;

    slash = wcsrchr(path, L'\\');
    if (!slash || slash == path)
        return 0;
    prev = slash - 1;
    while (prev > path && *prev != L'\\')
        prev--;
    if (prev == path && *prev != L'\\')
        return 0;                       /* no parent folder component  */
    while (*prev == L'\\' && prev > path)
        prev--;
    if (*prev == L'\\')
        prev++;
    len = (int)(slash - prev) + (int)wcslen(slash + 1) + 2; /* + '\' +0 */
    if (len > cap)
        return 0;
    for (i = 0; prev < slash; prev++, i++)
        out[i] = *prev;
    out[i++] = L'\\';
    wcscpy_s(out + i, cap - i, slash + 1);
    return 1;
}

/* append one UTF-16 line (with BOM on creation) to the map file */
static void mapAppend(const wchar_t *line)
{
    HANDLE h;
    DWORD wr;
    WORD bom = 0xFEFF;
    LARGE_INTEGER size = { 0, 0 };

    h = CreateFileW(g_mapFile, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSizeEx(h, &size) && size.QuadPart == 0)
        WriteFile(h, &bom, 2, &wr, NULL);
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &wr, NULL);
    WriteFile(h, L"\r\n", 4, &wr, NULL);
    CloseHandle(h);
}

/* report "<hh:mm:ss> <key>  included=<v>[   <- note]" once per key to
 * the map file; all callers are serialized via g_reportBusy           */
static void reportScan(const wchar_t *key, int included,
                       const wchar_t *note)
{
    wchar_t msg[PATH_CAP + 128];
    SYSTEMTIME st;
    int i;

    if (!key[0])
        return;
    for (i = 0; i < g_seenN; i++)
        if (_wcsicmp(g_seen[i], key) == 0)
            return;
    if (g_seenN < SEEN_MAX)
        wcsncpy_s(g_seen[g_seenN++], PATH_CAP, key, _TRUNCATE);
    else
        return;                         /* window full: stop reporting */

    GetLocalTime(&st);
    swprintf_s(msg, (sizeof msg / sizeof msg[0]),
               L"%02u:%02u:%02u  %ls  included=%d",
               st.wHour, st.wMinute, st.wSecond, key, included ? 1 : 0);
    if (note)
        wcscat_s(msg, (sizeof msg / sizeof msg[0]), note);
    mapAppend(msg);
}

/* create the selection file with a readable header once */
static void ensureSelFile(void)
{
    static const char hdr[] =
        "; merge_select - which scans feed the common mesh\r\n"
        ";\r\n"
        "; One section per scan; the section name is the scan folder\r\n"
        "; written as <project folder>\\<scan folder> (the full scan folder\r\n"
        "; path works too, it is checked first). When the scan has no\r\n"
        "; folder yet (a fresh session), its project name from the mesh\r\n"
        "; pipeline is used instead - the roster shows whichever applies.\r\n"
        "; Scans that are not listed are kept, exactly like stock\r\n"
        "; behaviour.\r\n"
        ";\r\n"
        "; merge-select.map.txt next to this file lists every scan the\r\n"
        "; module has seen during common-mesh runs - copy a section name\r\n"
        "; from there and set included = 0 to drop that scan.\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; master switch: 0 = module inactive even while installed\r\n"
        "enabled = 1\r\n"
        "\r\n"
        "; example:\r\n"
        ";[Project_1\\Scan_2]\r\n"
        ";included = 0\r\n";
    HANDLE h;
    DWORD wr;

    h = CreateFileW(g_selFile, GENERIC_WRITE, 0, NULL,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, hdr, (DWORD)(sizeof hdr - 1), &wr, NULL);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* the decision (detour hot path)                                      */
/* ------------------------------------------------------------------ */

/* decision reasons, parallel to the strings logged for each call     */
#define WHY_INI_EXC   0
#define WHY_INI_INC   1
#define WHY_UNLISTED  2
#define WHY_MASTER    3
#define WHY_NOREAD    4
#define WHY_EYE       5

/* one vendor QString getter -> wchar buffer; 1 when a non-empty string
 * came back. Runs the getter + utf16 + dtor trio under the caller's SEH. */
static int readStr(FnFolderRet fn, void *self, wchar_t *out, int cap)
{
    unsigned long long qbuf[4];          /* QString return slot         */
    const unsigned short *u;
    int i, ok = 0;

    out[0] = 0;
    (void)(fn)(qbuf, self);                  /* sret: hidden 1st arg  */
    u = (const unsigned short *)(g_qUtf16)(qbuf);
    if (u && u[0]) {
        for (i = 0; u[i] && i < cap - 1; i++)
            out[i] = (wchar_t)u[i];
        out[i] = 0;
        ok = 1;
    }
    (g_qDtor)(qbuf);
    return ok;
}

/* by-value Qt5 QString living inside a vendor object (struct { d, ptr }).
 * Qt5 QArrayData: ref@0 size@4 alloc@8, utf16 data behind ptr. Copies to
 * wchar; 1 on a non-empty string. Caller runs it under SEH. */
static int readEmbed(const void *qstr, wchar_t *out, int cap)
{
    const unsigned long long *q = (const unsigned long long *)qstr;
    unsigned long long d, p;
    const unsigned short *u;
    int size, i;

    out[0] = 0;
    if (!q)
        return 0;
    d = q[0];
    p = q[1];
    if (!d || !p)
        return 0;
    size = *(const int *)(d + 4);           /* QArrayData::size        */
    if (size <= 0 || size >= 8192)
        return 0;
    if (size > cap - 1)
        size = cap - 1;
    u = (const unsigned short *)p;
    for (i = 0; i < size; i++)
        out[i] = (wchar_t)u[i];
    out[i] = 0;
    return i > 0;
}

/* One-shot map of an impl object that answered nothing through the
 * getters: dump every by-value string in its first 80 bytes, so the
 * next build can pick the right field even when our guesses miss.
 * Bounded log (g_idLeft), fully SEH-guarded. */
static volatile LONG g_idLeft = 6;
static void idDump(unsigned long long impl)
{
    int off;

    if (InterlockedDecrement(&g_idLeft) < 0)
        return;
    __try {
        for (off = 0; off <= 0x48; off += 8) {
            wchar_t s[96];
            int i, printable = 1;

            if (!readEmbed((const void *)(impl + (unsigned long long)off),
                           s, (int)(sizeof s / sizeof s[0])))
                continue;
            for (i = 0; s[i]; i++)
                if (s[i] < 0x20 || s[i] == L'\\') { printable = 0; break; }
            if (printable)
                g_host->log("merge_select: idmap impl=%p +0x%02X = %ls",
                            (void *)impl, off, s);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

/* 1 = keep the scan in the merge (explicitly or by any fallback),
 * 0 = explicitly excluded. Writes the human-readable scan key into
 * keyOut (empty when every identity query failed) and the reason into
 * *why. *implOut gets the vendor impl pointer for diagnostics.
 * Never faults: vendor objects run under SEH.                           */
static int scanDecision(void *self, wchar_t *keyOut, int cap, int *why,
                        unsigned long long *implOut)
{
    wchar_t folder[PATH_CAP], alt[PATH_CAP], key[PATH_CAP], name[PATH_CAP];
    int v;

    keyOut[0] = 0;
    *implOut = 0;
    if (!g_getFolderPath || !g_qUtf16 || !g_qDtor) {
        *why = WHY_NOREAD;
        return 1;
    }
    if (!GetPrivateProfileIntW(L"Settings", L"enabled", 1, g_selFile)) {
        *why = WHY_MASTER;
        return 1;
    }

    __try {
        /* three identity channels, best first:
         * - the folder path (filled on saved projects)
         * - the embedded NAME string the vendor itself reads at
         *   **(self+0x18)+8 inside HandlePrjFile::getProjInfo - live
         *   session scans fill this even when every getter is empty
         * - the tx project-file name (the getter our RE calls impl+0x60) */
        *implOut = *(unsigned long long *)((unsigned char *)self + 0x18);
        (void)readStr(g_getFolderPath, self, folder, PATH_CAP);
        name[0] = 0;
        if (*implOut)
            (void)readEmbed((const void *)(*implOut + 8), name, PATH_CAP);
        alt[0] = 0;
        if (g_getTxPath)
            (void)readStr(g_getTxPath, self, alt, PATH_CAP);
        if (!folder[0] && !name[0] && !alt[0] && *implOut)
            idDump(*implOut);               /* one-shot object map      */
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = WHY_NOREAD;
        return 1;                       /* vendor object unreadable    */
    }

    /* section lookup: candidates best-first; each tried as the full
     * string, then as <parent>\<leaf>; first hit decides               */
    {
        const wchar_t *primary = NULL, *secondary = NULL;
        if (folder[0])
            primary = folder;
        else if (name[0])
            primary = name;
        else if (alt[0])
            primary = alt;
        if (primary) {
            if (primary != name && name[0])
                secondary = name;
            else if (primary != alt && alt[0])
                secondary = alt;
        }
        if (!primary) {
            *why = WHY_NOREAD;
            return 1;
        }
        v = -1;
        key[0] = 0;
        for (int c = 0; c < 2; c++) {
            const wchar_t *cand = c == 0 ? primary : secondary;
            wchar_t sh[PATH_CAP];
            int t;
            if (!cand)
                continue;
            t = GetPrivateProfileIntW(cand, L"included", -1, g_selFile);
            if (t == -1 && shortKey(cand, sh, PATH_CAP))
                t = GetPrivateProfileIntW(sh, L"included", -1, g_selFile);
            if (t != -1) {
                wcscpy_s(key, PATH_CAP, cand);
                v = t;
                break;
            }
        }
        if (v == -1)
            wcscpy_s(key, PATH_CAP, primary);
    }
    cleanPath(key, key, PATH_CAP);
    {
        wchar_t sh[PATH_CAP];
        if (shortKey(key, sh, PATH_CAP))
            wcscpy_s(key, PATH_CAP, sh);
    }
    wcscpy_s(keyOut, (size_t)cap, key);
    if (v == -1) {
        *why = WHY_UNLISTED;
        return 1;                       /* not listed -> keep (stock)  */
    }
    *why = (v == 0) ? WHY_INI_EXC : WHY_INI_INC;
    return v != 0;
}

/* eye watch (implemented with the meta-object code below) */
static int  eyeLookup(const unsigned char *u);   /* -1 unknown, 0/1 vis */
static void fmtUuid(const unsigned char *u, char *out, int cap);
static void gateEyeUuid(void *self, unsigned char *u, char *ustr, int cap,
                        int *have);

/* detour on HandlePrjFile::isGenerateCloud */
static unsigned char __fastcall det_isCloud(void *self)
{
    unsigned char r = g_origIsCloud(self);
    unsigned char *ra = (unsigned char *)_ReturnAddress();
    static const char *const whyN[] = {
        "excluded in .ini", "included in .ini", "not listed (stock)",
        "master switch off", "identity unreadable (fail-open)",
        "eye icon hidden" };
    wchar_t key[PATH_CAP];
    unsigned long long impl = 0;
    unsigned char ub[16];
    char ustr[48];
    int haveU = 0, eye = -1;
    int why = WHY_NOREAD, inc;

    if (ra < g_mpBase || ra >= g_mpBase + g_mpSize) {
        probeLog("cloud?", ra, self);
        return r;                       /* caller outside the merge code */
    }

    inc = scanDecision(self, key, (int)(sizeof key / sizeof key[0]),
                       &why, &impl);

    /* eye binding: the scan's QUuid at self+0x38 identifies the very
     * row the eye icons toggle.  A hidden eye excludes the scan as
     * long as the .ini has no explicit decision for it (the ini keeps
     * precedence as the power-user override). */
    gateEyeUuid(self, ub, ustr, (int)sizeof ustr, &haveU);
    if (haveU)
        eye = eyeLookup(ub);
    if (g_eyeGate && inc && eye == 0
        && (why == WHY_UNLISTED || why == WHY_NOREAD)) {
        inc = 0;
        why = WHY_EYE;
    }

    if (InterlockedCompareExchange(&g_reportBusy, 1, 0) == 0) {
        if (!r)
            reportScan(key, 0, L"   <- native no-cloud flag, already out");
        else if (!inc)
            reportScan(key, 0, L"   <- left out of the common mesh");
        else
            reportScan(key, 1, NULL);
        InterlockedExchange(&g_reportBusy, 0);
    }
    if (g_verbose && InterlockedDecrement(&g_logLeft) >= 0) {
        char u8[PATH_CAP * 2 + 96];
        const char *whyS = r ? whyN[why] : "vendor flag off";
        if (key[0] &&
            WideCharToMultiByte(CP_UTF8, 0, key, -1, u8, (int)sizeof u8,
                                NULL, NULL) > 0)
            g_host->log("merge_select: gate %s  uuid=%s eye=%d  vendor=%d"
                        "  %s  -> %s",
                        u8, haveU ? ustr : "-", eye, (int)r, whyS,
                        (!r || !inc) ? "SKIP" : "include");
        else
            g_host->log("merge_select: gate <no id>  ra=+0x%llX impl=%p"
                        "  vendor=%d  %s  -> include",
                        (unsigned long long)(ra - g_mpBase), (void *)impl,
                        (int)r, whyS);
    }
    return (r && inc) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* install (module worker thread only - never under the loader lock)   */
/* ------------------------------------------------------------------ */

static void tryInstall(void)
{
    HMODULE sf, qt, mp;
    void *target = NULL;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;

    if (g_installed || g_broken)
        return;
    sf = GetModuleHandleA("Sn3DFile.dll");
    qt = GetModuleHandleA("Qt5Core.dll");
    mp = GetModuleHandleA("Sn3DMultiProjects.dll");
    if (!sf || !qt || !mp)
        return;                         /* keep waiting                */

    __try {
        target = (void *)GetProcAddress(
            sf, "?isGenerateCloud@HandlePrjFile@@QEAA_NXZ");
        g_getFolderPath = (FnFolderRet)GetProcAddress(
            sf, "?getFolderPath@HandlePrjFile@@QEAA?AVQString@@XZ");
        g_getTxPath = (FnFolderRet)GetProcAddress(
            sf, "?getTXProjFullPath@HandlePrjFile@@QEAA?AVQString@@XZ");
        g_qUtf16 = (FnUtf16)GetProcAddress(qt, "?utf16@QString@@QEBAPEBGXZ");
        g_qDtor  = (FnDtor)GetProcAddress(qt, "??1QString@@QEAA@XZ");
        dos = (PIMAGE_DOS_HEADER)mp;
        nt  = (PIMAGE_NT_HEADERS)((unsigned char *)mp + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE
            || nt->Signature != IMAGE_NT_SIGNATURE)
            target = NULL;
        else {
            g_mpBase = (unsigned char *)mp;
            g_mpSize = nt->OptionalHeader.SizeOfImage;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        target = NULL;
    }
    if (!target || !g_getFolderPath || !g_qUtf16 || !g_qDtor) {
        g_host->log("merge_select: vendor exports not found "
                    "(isGenerateCloud/getFolderPath/QString) "
                    "- staying out of it");
        InterlockedExchange(&g_broken, 1);
        return;
    }
    if (!g_host->hook(target, (void *)det_isCloud, (void **)&g_origIsCloud)) {
        g_host->log("merge_select: detour refused - app stays stock");
        InterlockedExchange(&g_broken, 1);
        return;
    }
    /* v1.1.3: the getMeshData detours were removed - that function runs
     * on hot (viewer) paths and the extra hop hung the UI once (WER
     * AppHangB1 2026-11-24 03:36).  The isGenerateCloud probe above is
     * kept: it is off hot paths and already mapped the pipeline. */
    ensureSelFile();
    InterlockedExchange(&g_installed, 1);
    g_host->log("merge_select: hooked HandlePrjFile::isGenerateCloud "
                "(active for Sn3DMultiProjects.dll callers) - see %S",
                g_selFile);
}

/* ------------------------------------------------------------------ */
/* Eye watch (v1.2.0, record-only)                                    */
/*                                                                     */
/* The project-list eye icon is the vendor QML slot                    */
/* ProjectListControllerE7::setNodeVisible(QUuid, bool).  The class is */
/* not exported, but its moc QMetaObject sits in the module image in   */
/* plain Qt5 layout: { superdata, stringdata, data, static_metacall }. */
/* We scan for the object whose className string is                  */
/* "ProjectListControllerE7", verify method 8 in its table is          */
/* "setNodeVisible", then detour qt_static_metacall.  QML eye toggles  */
/* flow through the metacall; direct C++ calls (the mesh dialog's      */
/* force-show burst) bypass it - which is exactly the signal we need.  */
/* Nothing is modified: the state map + log feed the next build.       */
/* ------------------------------------------------------------------ */

static volatile LONG  g_eyeDone;
static volatile LONG  g_eyeBusy;
static volatile LONG  g_eyeSeq;
static volatile LONG  g_eyeLogged = 40;
static int            g_eyeIdx = 8;

#define EYE_MAX 64
typedef struct { unsigned char uuid[16]; int visible; int seq; } EyeEnt;
static EyeEnt g_eye[EYE_MAX];

static int g_metaSeen[32][2];
static int g_metaSeenN;

typedef void (*FnMeta)(void *, int, int, void **);
static FnMeta g_origMeta;

static const char *metaCallName(int c)
{
    switch (c) {
    case 0:  return "invoke";
    case 1:  return "readProp";
    case 2:  return "writeProp";
    case 3:  return "resetProp";
    case 4:  return "editProp";
    case 5:  return "saveProp";
    case 6:  return "isSetProp";
    case 7:  return "unsetProp";
    case 10: return "qtStatic";
    case 11: return "enumFromValue";
    case 12: return "enumFromName";
    default: return "other";
    }
}

/* module name (+ offset) owning an address, "?" when unknown */
static void modOf(void *addr, char *out, int cap, unsigned *offOut)
{
    HMODULE h = NULL;
    strcpy_s(out, cap, "?");
    if (offOut)
        *offOut = 0;
    __try {
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)addr, &h) && h) {
            char path[MAX_PATH];
            char *b;
            if (GetModuleFileNameA(h, path, sizeof path) > 0) {
                b = strrchr(path, '\\');
                strcpy_s(out, cap, b ? b + 1 : path);
                if (offOut)
                    *offOut = (unsigned)((unsigned char *)addr
                                         - (unsigned char *)h);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void fmtUuid(const unsigned char *u, char *out, int cap)
{
    unsigned d1 = u[0] | u[1] << 8 | u[2] << 16 | (unsigned)u[3] << 24;
    unsigned d2 = (unsigned)(u[4] | u[5] << 8);
    unsigned d3 = (unsigned)(u[6] | u[7] << 8);
    _snprintf_s(out, cap, _TRUNCATE,
                "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                d1, d2, d3, u[8], u[9], u[10], u[11], u[12], u[13],
                u[14], u[15]);
}

/* eye state of a raw QUuid struct: -1 = never seen, else visible flag */
static int eyeLookup(const unsigned char *u)
{
    int i;

    for (i = 0; i < EYE_MAX; i++)
        if (g_eye[i].seq && memcmp(g_eye[i].uuid, u, 16) == 0)
            return g_eye[i].visible;
    return -1;
}

/* HandlePrjFile keeps the scan's QUuid as raw struct bytes at offset
 * +0x38 (the very bytes the community patchset hex-encodes for its
 * uuid sections). Read them for gate <-> eye correlation, SEH-safe. */
static void gateEyeUuid(void *self, unsigned char *u, char *ustr, int cap,
                        int *have)
{
    *have = 0;
    ustr[0] = 0;
    memset(u, 0, 16);
    __try {
        memcpy(u, (const unsigned char *)self + 0x38, 16);
        *have = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (*have)
        fmtUuid(u, ustr, cap);
}

static void eyeRecord(const unsigned char *u, int visible)
{
    char s[64];
    int i, slot = -1, seq = (int)InterlockedIncrement(&g_eyeSeq);

    for (i = 0; i < EYE_MAX; i++)
        if (g_eye[i].seq && memcmp(g_eye[i].uuid, u, 16) == 0) {
            slot = i;
            break;
        }
    if (slot < 0)
        for (i = 0; i < EYE_MAX; i++)
            if (!g_eye[i].seq) { slot = i; break; }
    if (slot >= 0) {
        memcpy(g_eye[slot].uuid, u, 16);
        g_eye[slot].visible = visible;
        g_eye[slot].seq = seq;
    }
    if (g_verbose && InterlockedDecrement(&g_eyeLogged) >= 0) {
        fmtUuid(u, s, sizeof s);
        g_host->log("merge_select: eye %s -> %s",
                    s, visible ? "VISIBLE" : "hidden");
    }
}

/* detour on qt_static_metacall: observe, never interfere */
static void __fastcall det_meta(void *obj, int call, int idx, void **a)
{
    int i;

    if (g_verbose && InterlockedCompareExchange(&g_eyeBusy, 1, 0) == 0) {
        int fresh = 1;
        for (i = 0; i < g_metaSeenN && i < 32; i++)
            if (g_metaSeen[i][0] == call && g_metaSeen[i][1] == idx) {
                fresh = 0;
                break;
            }
        if (fresh && g_metaSeenN < 32) {
            g_metaSeen[g_metaSeenN][0] = call;
            g_metaSeen[g_metaSeenN][1] = idx;
            g_metaSeenN++;
            {
                char mod[64];
                unsigned off;
                modOf(_ReturnAddress(), mod, sizeof mod, &off);
                g_host->log("merge_select: eye-meta %s idx=%d via %s+0x%X",
                            metaCallName(call), idx, mod, off);
            }
        }
        InterlockedExchange(&g_eyeBusy, 0);
    }
    if (call == 0 && idx == g_eyeIdx && a && a[1] && a[2]) {
        __try {
            unsigned char u[16];
            memcpy(u, a[1], 16);
            eyeRecord(u, *(const unsigned char *)a[2] ? 1 : 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_origMeta)
        g_origMeta(obj, call, idx, a);
}

/* read the moc stringdata: Qt5 descriptor { ref, size, alloc, flags,
 * offset } where data = (const char *)desc + offset. */
static int metaStr(const unsigned char *D, int idx, char *out, int cap)
{
    const unsigned char *desc;
    const char *p;
    unsigned size;
    long long offv;
    int i;

    out[0] = 0;
    if (idx < 0 || idx > 4096)
        return 0;
    desc = D + (size_t)idx * 24;
    if (*(const int *)(desc + 4) <= 0 || *(const int *)(desc + 4) > 256)
        return 0;
    size = (unsigned)*(const int *)(desc + 4);
    offv = *(const long long *)(desc + 16);
    p = (const char *)desc + offv;
    if (size >= (unsigned)cap)
        size = cap - 1;
    for (i = 0; i < (int)size; i++)
        out[i] = p[i];
    out[i] = 0;
    return 1;
}

/* one pass over the ScanPage_E7 image looking for the controller's
 * QMetaObject; 1 = done (hooked or definitively not here) */
static int eyeTry(void)
{
    HMODULE m = GetModuleHandleA("ScanPage_E7.dll");
    unsigned char *base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    int s, found = 0;

    if (!m)
        return 0;
    base = (unsigned char *)m;
    InterlockedExchange(&g_eyeDone, 1);       /* one attempt is enough */

    __try {
        nt = (IMAGE_NT_HEADERS *)(base + *(const int *)(base + 0x3C));
        sec = (IMAGE_SECTION_HEADER *)
              ((unsigned char *)&nt->OptionalHeader
               + nt->FileHeader.SizeOfOptionalHeader);
        for (s = 0; s < nt->FileHeader.NumberOfSections; s++) {
            IMAGE_SECTION_HEADER *sh = sec + s;
            unsigned char *sv, *se;
            unsigned char *q;

            if (!(sh->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA))
                continue;
            sv = base + sh->VirtualAddress;
            se = sv + sh->Misc.VirtualSize;
            for (q = sv; q + 48 <= se; q += 8) {
                const unsigned int *U;
                const unsigned char *D;
                void *F;
                int revision, cidx, methodCount;
                char cls[64], name[64];

                U = *(const unsigned int *const *)(q + 0x10);
                D = *(const unsigned char *const *)(q + 0x08);
                F = *(void *const *)(q + 0x18);
                if (!U || !D || !F)
                    continue;
                if ((unsigned char *)U < base
                    || (unsigned char *)U >= base
                                    + nt->OptionalHeader.SizeOfImage)
                    continue;
                if (D < base
                    || D >= base + nt->OptionalHeader.SizeOfImage)
                    continue;
                if ((unsigned char *)F < base
                    || (unsigned char *)F
                        >= base + nt->OptionalHeader.SizeOfImage)
                    continue;
                revision = (int)U[0];
                if (revision < 7 || revision > 200)
                    continue;
                cidx = (int)U[1];
                if (!metaStr(D, cidx, cls, sizeof cls)
                    || strcmp(cls, "ProjectListControllerE7") != 0)
                    continue;
                methodCount = (int)U[4];
                if (methodCount < 9)
                    continue;
                /* this vendor's moc layout: 5 uints per method entry,
                 * table starts at U[U[5]]; entry 0 is index 0 */
                {
                    const unsigned int *md = U + U[5];
                    int e;
                    for (e = 0; e < methodCount; e++)
                        if (metaStr(D, (int)md[e * 5], name, sizeof name)
                            && strcmp(name, "setNodeVisible") == 0)
                            break;
                    if (e >= methodCount)
                        continue;
                    g_eyeIdx = e;
                }
                g_host->log("merge_select: eye controller found: "
                            "metaobject=+%llX metacall=+%llX "
                            "setNodeVisible idx=%d",
                            (unsigned long long)(q - base),
                            (unsigned long long)
                                ((unsigned char *)F - base),
                            g_eyeIdx);
                if (g_host->hook(F, (void *)det_meta, (void **)&g_origMeta))
                    g_host->log("merge_select: eye watch armed "
                                "(recording show/hide events, "
                                "no behaviour change)");
                else
                    g_host->log("merge_select: eye metacall hook refused");
                found = 1;
                break;
            }
            if (found)
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_host->log("merge_select: eye scan faulted - watch disabled");
    }
    if (!found && g_verbose)
        g_host->log("merge_select: eye controller not found in "
                    "ScanPage_E7.dll - watch disabled");
    return 1;
}

/* ------------------------------------------------------------------ */
/* DLL load notification: only wakes the worker; the worker owns ALL   */
/* install work (MinHook must never run while the loader lock is held) */
/* ------------------------------------------------------------------ */

#define LDR_DLL_LOADED  1

typedef struct _MS_USTR {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} MS_USTR;
typedef struct _MS_NOTIF {
    ULONG    Flags;
    MS_USTR *FullDllName;
    MS_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} MS_NOTIF;
typedef NTSTATUS (NTAPI *MS_REGNOTIF)(ULONG flags, PVOID fn, PVOID ctx,
                                      PVOID *cookie);

static void *g_notifCookie;

static void WINAPI ms_notify(unsigned reason, const MS_NOTIF *d, void *ctx)
{
    (void)ctx;
    if (reason == LDR_DLL_LOADED && d && d->DllBase)
        SetEvent(g_wake);
}

static int armNotify(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    MS_REGNOTIF reg;

    if (!ntdll)
        return 0;
    reg = (MS_REGNOTIF)(void *)
              GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg)
        return 0;
    return reg(0, (void *)ms_notify, NULL, &g_notifCookie) == 0;
}

static unsigned WINAPI worker(LPVOID arg)
{
    ULONGLONG t0 = GetTickCount64();
    int notif;

    (void)arg;
    notif = armNotify();
    if (!notif && g_verbose)
        g_host->log("merge_select: dll notification unavailable, "
                    "polling only");

    while (!g_installed && !g_broken) {
        tryInstall();
        if (g_installed || g_broken)
            break;
        if (GetTickCount64() - t0 > 900000) {
            if (g_verbose)
                g_host->log("merge_select: Sn3DFile.dll never mapped in "
                            "this process - nothing to do here");
            break;
        }
        /* wake on any DLL load; poll faster during the startup phase */
        WaitForSingleObject(g_wake,
                            GetTickCount64() - t0 < 10000 ? 50 : 500);
    }

    /* eye watch: wait until ScanPage_E7.dll is mapped, then find and
     * observe the vendor's show/hide slot (record-only, ini switch) */
    if (!g_broken && g_eyewatch) {
        ULONGLONG t1 = GetTickCount64();
        while (!g_eyeDone && GetTickCount64() - t1 < 600000) {
            if (eyeTry())
                break;
            WaitForSingleObject(g_wake, 1000);
        }
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
    wchar_t wbuf[PATH_CAP];
    char abuf[PATH_CAP];
    wchar_t *dot, *slash;
    unsigned tid;
    HANDLE th;

    g_host = host;
    g_verbose = host->cfg_int(host, "verbose", 1);
    g_eyewatch = host->cfg_int(host, "eyewatch", 1);
    g_eyeGate = host->cfg_int(host, "eye_gate", 1);

    if (host->cfg_string(host, "file", abuf, sizeof abuf) > 0 && abuf[0]
        && MultiByteToWideChar(CP_UTF8, 0, abuf, -1, wbuf,
                               (int)(sizeof wbuf / sizeof wbuf[0])) > 0) {
        wcsncpy_s(g_selFile, PATH_CAP, wbuf, _TRUNCATE);
        /* the map file always sits next to the selection file */
        wcsncpy_s(g_mapFile, PATH_CAP, g_selFile, _TRUNCATE);
        dot  = wcsrchr(g_mapFile, L'.');
        slash = wcsrchr(g_mapFile, L'\\');
        if (dot && (!slash || dot > slash))
            *dot = 0;
        wcscat_s(g_mapFile, PATH_CAP, L".map.txt");
    } else {
        wcscpy_s(g_selFile, PATH_CAP, SEL_FILE);
        wcscpy_s(g_mapFile, PATH_CAP, MAP_FILE);
    }

    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_wake)
        return 2;
    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th) {
        CloseHandle(g_wake);
        return 2;
    }
    CloseHandle(th);

    {
        char exe[MAX_PATH] = "?", *b;
        char env[64];
        if (GetModuleFileNameA(NULL, exe, sizeof exe) > 0
            && (b = strrchr(exe, '\\')) != NULL)
            memmove(exe, b + 1, strlen(b + 1) + 1);
        _snprintf_s(env, sizeof env, _TRUNCATE, " sf=%d qt=%d mp=%d",
                    GetModuleHandleA("Sn3DFile.dll") != 0,
                    GetModuleHandleA("Qt5Core.dll") != 0,
                    GetModuleHandleA("Sn3DMultiProjects.dll") != 0);
        host->log("merge_select v%s armed in pid=%lu exe=%s%s",
                  MOD_VERSION, (unsigned long)GetCurrentProcessId(),
                  exe, env);
    }
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* the detour is owned by the host's MinHook teardown; nothing here */
}
