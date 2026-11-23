/*
 * merge_select.c - choose which scans feed the common mesh.
 *
 * Version 1.0.0
 *
 * The problem (verified on product 2.1.0-10):
 *   Several scans can live in one project, and the vendor's "generate
 *   common mesh" step feeds EVERY scan whose project file says
 *   "generate a point cloud" into the merge. The single exported
 *   question behind that decision is
 *      Sn3DFile.dll!HandlePrjFile::isGenerateCloud(void) const -> bool
 *   and the only module that ever asks it is Sn3DMultiProjects.dll -
 *   its common-mesh collection calls it once per scan right before
 *   appending the scan to the mesh input list; a false answer takes the
 *   same branch a scan with cloud generation switched off takes.
 *   (Full evidence chain: docs/research/merge-common-mesh.md.)
 *   There is no stock way to drop one scan from just that step;
 *   deleting the scan would also destroy its stored data.
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
 * guessing any paths.
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
 */

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <intrin.h>

#include "exstar_sdk.h"

#define MOD_VERSION   "1.0.0"

#define SEL_FILE  L"C:\\Users\\Public\\Shining3d\\EXStar Hub\\merge-select.ini"
#define MAP_FILE  L"C:\\Users\\Public\\Shining3d\\EXStar Hub\\merge-select.map.txt"

#define PATH_CAP  512                    /* wchar path/key buffers      */
#define SEEN_MAX  64                     /* dedupe window for map+log   */

static const ExHost *g_host;
static int           g_verbose = 1;
static wchar_t       g_selFile[PATH_CAP];
static wchar_t       g_mapFile[PATH_CAP];

/* vendor functions, resolved by exported name */
typedef unsigned char (*FnIsCloud)(void *self);
typedef void *(*FnFolderRet)(void *ret, void *self);   /* QString sret  */
typedef const unsigned short *(*FnUtf16)(const void *q);
typedef void (*FnDtor)(void *q);

static FnIsCloud   g_origIsCloud;        /* trampoline from host->hook  */
static FnFolderRet g_getFolderPath;
static FnUtf16     g_qUtf16;
static FnDtor      g_qDtor;

/* the common-mesh code lives in this image; callers from anywhere else
 * (project open/save, Sn3DFile internals) keep seeing the vendor answer */
static unsigned char *g_mpBase;
static size_t         g_mpSize;

static volatile LONG g_installed;        /* detour in place             */
static volatile LONG g_broken;           /* resolve/hook failed: idle   */
static volatile LONG g_reportBusy;       /* serialize map/log writes    */
static HANDLE        g_wake;             /* dll-load event for worker   */

/* scan keys already reported to the map file / log */
static wchar_t g_seen[SEEN_MAX][PATH_CAP];
static int     g_seenN;

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

/* report "<hh:mm:ss> <key>  included=<v>" once per key: map file always,
 * mod log when verbose; all callers are serialized via g_reportBusy   */
static void reportScan(const wchar_t *key, int included)
{
    wchar_t msg[PATH_CAP + 128];
    SYSTEMTIME st;
    char u8[PATH_CAP * 2 + 160];
    int i;

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
    if (!included)
        wcscat_s(msg, (sizeof msg / sizeof msg[0]),
                 L"   <- left out of the common mesh");
    mapAppend(msg);
    if (g_verbose &&
        WideCharToMultiByte(CP_UTF8, 0, msg, -1, u8, (int)sizeof u8,
                            NULL, NULL) > 0)
        g_host->log("merge_select: %s", u8);
}

/* create the selection file with a readable header once */
static void ensureSelFile(void)
{
    static const char hdr[] =
        "; merge_select - which scans feed the common mesh\r\n"
        ";\r\n"
        "; One section per scan; the section name is the scan folder\r\n"
        "; written as <project folder>\\<scan folder> (the full scan folder\r\n"
        "; path works too, it is checked first). Scans that are not listed\r\n"
        "; are kept, exactly like stock behaviour.\r\n"
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

/* 1 = keep the scan in the merge (explicitly or by any fallback),
 * 0 = explicitly excluded. Never faults: vendor objects under SEH.  */
static int scanIncluded(void *self)
{
    unsigned long long qbuf[4];          /* QString return slot         */
    wchar_t key[PATH_CAP];
    const unsigned short *u = NULL;
    int v, i;

    if (!g_getFolderPath || !g_qUtf16 || !g_qDtor)
        return 1;
    if (!GetPrivateProfileIntW(L"Settings", L"enabled", 1, g_selFile))
        return 1;

    __try {
        (void)(g_getFolderPath)(qbuf, self);   /* sret: hidden 1st arg  */
        u = (const unsigned short *)(g_qUtf16)(qbuf);
        if (u) {
            for (i = 0; u[i] && i < PATH_CAP - 1; i++)
                key[i] = (wchar_t)u[i];
            key[i] = 0;
        }
        (g_qDtor)(qbuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;                       /* vendor object unreadable    */
    }
    if (!u || !key[0])
        return 1;
    cleanPath(key, key, PATH_CAP);
    if (!key[0])
        return 1;

    /* section lookup: full folder path, then <parent>\<leaf>, then keep */
    v = GetPrivateProfileIntW(key, L"included", -1, g_selFile);
    if (v == -1) {
        wchar_t sh[PATH_CAP];
        if (shortKey(key, sh, PATH_CAP)) {
            wcscpy_s(key, PATH_CAP, sh);
            v = GetPrivateProfileIntW(key, L"included", -1, g_selFile);
        }
    }
    if (v == -1)
        v = 1;                          /* not listed -> keep (stock)  */

    if (InterlockedCompareExchange(&g_reportBusy, 1, 0) == 0) {
        reportScan(key, v != 0);
        InterlockedExchange(&g_reportBusy, 0);
    }
    return v != 0;
}

/* detour on HandlePrjFile::isGenerateCloud */
static unsigned char __fastcall det_isCloud(void *self)
{
    unsigned char r = g_origIsCloud(self);
    unsigned char *ra = (unsigned char *)_ReturnAddress();

    if (!r)
        return 0;
    if (ra < g_mpBase || ra >= g_mpBase + g_mpSize)
        return r;                       /* caller outside the merge code */
    return scanIncluded(self) ? r : 0;
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
    ensureSelFile();
    InterlockedExchange(&g_installed, 1);
    g_host->log("merge_select: hooked HandlePrjFile::isGenerateCloud "
                "(active for Sn3DMultiProjects.dll callers) - see %S",
                g_selFile);
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

    host->log("merge_select v%s armed in pid=%lu",
              MOD_VERSION, (unsigned long)GetCurrentProcessId());
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* the detour is owned by the host's MinHook teardown; nothing here */
}
