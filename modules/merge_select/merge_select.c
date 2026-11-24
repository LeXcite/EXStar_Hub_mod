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

#define MOD_VERSION   "1.4.7"

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
static void gateEyeUuid(void *self, unsigned long long impl, void *wrapper,
                        unsigned char *u, char *ustr, int cap, int *have);

/* ------------------------------------------------------------------ */
/* wrapper-capturing thunk                                             */
/*                                                                     */
/* The merge loop in Sn3DMultiProjects!Sn3DHandleProjects::getMeshData */
/* iterates scan items (wrappers) with `HandlePrjFile* = [rax+0x28]`   */
/* and the scan QUuid at [rax+0x38]; `mov rcx,[rax+0x28]` runs right   */
/* before `call [isGenerateCloud]`, so rax still holds the wrapper at  */
/* the call.  A 65-byte hand-assembled stub therefore:                 */
/*   - grabs the caller's real return address ([rsp] at entry) so the  */
/*     detour can tell merge-loop callers from project open/save,      */
/*   - passes rax (wrapper candidate) as 2nd and the return address as */
/*     3rd argument,                                                  */
/*   - reserves the 32-byte shadow space the x64 ABI requires before   */
/*     calling a C function (1.4.4/1.4.5 did not, so the helper could  */
/*     spill its parameters over the saved flags/registers),           */
/*   - keeps 16-byte stack alignment at the call,                      */
/*   - returns AL only: rax is a volatile return register, so the      */
/*     saved copy is dropped instead of popped (1.4.5 fixed that).     */
/* All other callers see the vendor answer unchanged.                  */
/* ------------------------------------------------------------------ */
static unsigned char *g_thunk;

static int buildThunk(void *helper)
{
    unsigned char *t;
    unsigned long long abs64 = (unsigned long long)helper;

    t = (unsigned char *)VirtualAlloc(NULL, 128,
                                      MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!t)
        return 0;
    t[0] = 0x4C; t[1] = 0x8B; t[2] = 0x04; t[3] = 0x24;              /* mov r8, [rsp]  (ra)    */
    t[4] = 0x48; t[5] = 0x89; t[6] = 0xC2;                           /* mov rdx, rax (wrapper) */
    t[7]  = 0x50; t[8]  = 0x53; t[9]  = 0x51; t[10] = 0x52;          /* push rax rbx rcx rdx   */
    t[11] = 0x56; t[12] = 0x57;                                      /* push rsi rdi           */
    t[13] = 0x41; t[14] = 0x50;                                      /* push r8                */
    t[15] = 0x41; t[16] = 0x51;                                      /* push r9                */
    t[17] = 0x41; t[18] = 0x52;                                      /* push r10               */
    t[19] = 0x41; t[20] = 0x53;                                      /* push r11               */
    t[21] = 0x9C;                                                    /* pushfq                 */
    t[22] = 0x48; t[23] = 0x83; t[24] = 0xEC; t[25] = 0x20;          /* sub rsp, 0x20 (shadow) */
    t[26] = 0x49; t[27] = 0xBA;                                      /* mov r10, helper        */
    memcpy(t + 28, &abs64, 8);
    t[36] = 0x41; t[37] = 0xFF; t[38] = 0xD2;                        /* call r10               */
    t[39] = 0x0F; t[40] = 0xB6; t[41] = 0xC0;                        /* movzx eax, al          */
    t[42] = 0x48; t[43] = 0x83; t[44] = 0xC4; t[45] = 0x20;          /* add rsp, 0x20          */
    t[46] = 0x9D;                                                    /* popfq                  */
    t[47] = 0x41; t[48] = 0x5B;                                      /* pop r11                */
    t[49] = 0x41; t[50] = 0x5A;                                      /* pop r10                */
    t[51] = 0x41; t[52] = 0x59;                                      /* pop r9                 */
    t[53] = 0x41; t[54] = 0x58;                                      /* pop r8                 */
    t[55] = 0x5F; t[56] = 0x5E;                                      /* pop rdi rsi            */
    t[57] = 0x5A; t[58] = 0x59;                                      /* pop rdx rcx            */
    t[59] = 0x5B;                                                    /* pop rbx                */
    t[60] = 0x48; t[61] = 0x83; t[62] = 0xC4; t[63] = 0x08;          /* add rsp, 8 (drop rax)  */
    t[64] = 0xC3;                                                    /* ret (AL = answer)      */
    g_thunk = t;
    return 1;
}

/* the decision half runs under SEH: a vendor object that moved between
 * releases must never take the host process down (fail-open = stock) */
static unsigned char decideCloud(void *self, void *wrapper,
                                 unsigned char *ra, unsigned char r);

/* detour on HandlePrjFile::isGenerateCloud (called via g_thunk, which
 * adds the wrapper candidate and the caller's return address) */
static unsigned char __fastcall det_isCloud(void *self, void *wrapper,
                                            void *ra)
{
    unsigned char r = g_origIsCloud(self);
    unsigned char out = r;

    __try {
        out = decideCloud(self, wrapper, (unsigned char *)ra, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static volatile LONG faulted;
        if (InterlockedIncrement(&faulted) == 1)
            g_host->log("merge_select: gate fault while reading the vendor "
                        "object - this call keeps the stock answer");
        out = r;
    }
    return out;
}

static const char *const g_whyN[] = {
    "excluded in .ini", "included in .ini", "not listed (stock)",
    "master switch off", "identity unreadable (fail-open)",
    "eye icon hidden" };

static unsigned char decideCloud(void *self, void *wrapper,
                                 unsigned char *ra, unsigned char r)
{
    const char *const *whyN = g_whyN;
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

    /* eye binding, two channels, both keyed by the scan QUuid the
     * object carries (offset located at runtime - see uuidScan):
     * - the shared .ini: [uuid] sections written whenever an eye is
     *   toggled (works across processes - the fusion engine's merge
     *   loop reads the same file it never wrote)
     * - the in-process eye map: instant state from the metacall watch
     * An explicit .ini section always wins; the eye map only speaks
     * when the ini has no decision for the scan. */
    gateEyeUuid(self, impl, wrapper, ub, ustr, (int)sizeof ustr, &haveU);
    if (g_eyeGate && haveU && why != WHY_MASTER) {
        wchar_t ukey[64];
        int t = -1;
        if (MultiByteToWideChar(CP_UTF8, 0, ustr, -1, ukey,
                                (int)(sizeof ukey / sizeof ukey[0])) > 0)
            t = GetPrivateProfileIntW(ukey, L"included", -1, g_selFile);
        if (t != -1) {
            inc = (t != 0);
            why = (t != 0) ? WHY_INI_INC : WHY_INI_EXC;
        } else {
            eye = eyeLookup(ub);
            if (inc && eye == 0
                && (why == WHY_UNLISTED || why == WHY_NOREAD)) {
                inc = 0;
                why = WHY_EYE;
            }
        }
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
            g_host->log("merge_select: gate <no id>  uuid=%s eye=%d"
                        "  ra=+0x%llX impl=%p  vendor=%d  %s  -> %s",
                        haveU ? ustr : "-", eye,
                        (unsigned long long)(ra - g_mpBase), (void *)impl,
                        (int)r, whyS,
                        (!r || !inc) ? "SKIP" : "include");
    }
    return (r && inc) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* install (module worker thread only - never under the loader lock)   */
/* ------------------------------------------------------------------ */

/* Which host processes may carry live hooks.  Evidence 2026-11-24
 * 19:02: scanservice.exe died (0xC0000005, write to address 1) with the
 * module loaded in it, and the vendor then refused the project with
 * 0x00000012.  Hooks therefore only ever go into processes that need
 * them, and every other host keeps a completely inert module:
 *   gate_exes     - processes that run Sn3DMultiProjects' merge loop
 *   eyewatch_exes - the QML UI process (eye icons live there)
 * Both are ini switches; an unmatched process logs once and goes idle. */
static char g_exeName[MAX_PATH] = "?";

static int exeAllowed(const char *key, const char *defList)
{
    char list[256];
    size_t i = 0;

    if (g_host->cfg_string(g_host, key, list, (int)sizeof list) <= 0
        || !list[0])
        strcpy_s(list, sizeof list, defList);
    while (i < sizeof list && list[i]) {
        char name[MAX_PATH];
        size_t n = 0;
        int more;

        while (list[i] == ' ' || list[i] == '\t')
            i++;
        while (list[i] && list[i] != ',' && list[i] != ';') {
            if (n + 1 < sizeof name)
                name[n++] = list[i];
            i++;
        }
        more = list[i] != 0;
        if (more)
            i++;
        while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
            n--;
        name[n] = 0;
        if (n && _stricmp(name, g_exeName) == 0)
            return 1;
        if (!more)
            break;
    }
    return 0;
}

static void tryInstall(void)
{
    HMODULE sf, qt, mp;
    void *target = NULL;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;

    if (g_installed || g_broken)
        return;
    if (!exeAllowed("gate_exes",
                    "EXStar Hub.exe,scanservice.exe,Sn3DprocessManager.exe")) {
        g_host->log("merge_select: %s is not in gate_exes - module stays "
                    "inert here (no hooks, no files)", g_exeName);
        InterlockedExchange(&g_broken, 1);
        return;
    }
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
    if (!g_thunk && !buildThunk((void *)det_isCloud)) {
        g_host->log("merge_select: thunk alloc failed - app stays stock");
        InterlockedExchange(&g_broken, 1);
        return;
    }
    if (!g_host->hook(target, (void *)g_thunk, (void **)&g_origIsCloud)) {
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

/* ---- uuid finder ------------------------------------------------
 * Live evidence 2026-11-24: the HandlePrjFile that the merge loop
 * passes to isGenerateCloud carries ZERO at +0x38.  The community
 * patchset key lives in the loop's WRAPPER item (wrapper+0x38 is the
 * QUuid, wrapper+0x28 the HandlePrjFile pointer - two different heap
 * objects), which is not reachable from self.  So instead of trusting
 * one offset, FIND the uuid: the eye watch has recorded the QUuid of
 * every scan row ever toggled, and the module searches the scan object
 * and its vendor impl for those known values - in both memory layouts
 * (QUuid struct little-endian, text-order big-endian) and as UTF-16
 * canonical text.  The first hit (level, offset, layout) is cached and
 * every later decision reads it directly; misses log a compact dump
 * so future vendor builds stay debuggable. */
#define USCAN_WIN  0x200                    /* bytes scanned per obj   */
#define USCAN_MAX  32                       /* known uuids to try      */

static struct { int level, off, lay; } g_uLoc = { -1, 0, 0 };
static volatile LONG g_uScanTries   = 8;
static volatile LONG g_uScanLogged  = 8;
static volatile LONG g_uMissLogged  = 3;

static void uuidLeToBe(const unsigned char *u, unsigned char *b)
{
    b[0] = u[3]; b[1] = u[2]; b[2] = u[1]; b[3] = u[0];
    b[4] = u[5]; b[5] = u[4];
    b[6] = u[7]; b[7] = u[6];
    memcpy(b + 8, u + 8, 8);
}

static int memFind(const unsigned char *buf, int len,
                   const unsigned char *pat, int plen)
{
    int i;

    for (i = 0; i + plen <= len; i++)
        if (buf[i] == pat[0] && memcmp(buf + i, pat, plen) == 0)
            return i;
    return -1;
}

static int hexNib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int uuidFromText(const wchar_t *t, unsigned char *u)
{
    /* "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" -> QUuid struct bytes */
    int i, nib, pos = 0;
    unsigned char bytes[16];

    for (i = 0; i < 16; i++) {
        nib = hexNib((char)t[pos]);
        if (nib < 0)
            return 0;
        bytes[i] = (unsigned char)(nib << 4);
        pos++;
        nib = hexNib((char)t[pos]);
        if (nib < 0)
            return 0;
        bytes[i] |= (unsigned char)nib;
        pos++;
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            if (t[pos] != L'-')
                return 0;
            pos++;
        }
    }
    u[0] = bytes[3]; u[1] = bytes[2]; u[2] = bytes[1]; u[3] = bytes[0];
    u[4] = bytes[5]; u[5] = bytes[4];
    u[6] = bytes[7]; u[7] = bytes[6];
    memcpy(u + 8, bytes + 8, 8);
    return 1;
}

static int copyWin(void *src, unsigned char *win)
{
    int ok = 0;

    __try {
        memcpy(win, src, USCAN_WIN);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ok;
}

/* search self/impl for any known eye uuid; fills u/ustr, caches g_uLoc */
static int uuidScan(void *self, unsigned long long impl,
                    unsigned char *u, char *ustr, int cap)
{
    static const char *const layN[] = { "le16", "be16", "txt16" };
    unsigned char sw[USCAN_WIN], iw[USCAN_WIN], be[16], pat[72];
    int haveS, haveI = 0, i, n = 0, off, hit = 0;
    int hlay = 0, hlevel = 0, hoff = 0;
    unsigned char hu[16];
    char s[64];

    haveS = copyWin(self, sw);
    if (impl)
        haveI = copyWin((void *)(size_t)impl, iw);
    if (!haveS && !haveI)
        return 0;

    for (i = 0; i < EYE_MAX && n < USCAN_MAX; i++) {
        unsigned char *le = g_eye[i].uuid;
        int k;

        if (!g_eye[i].seq)
            continue;
        n++;
        uuidLeToBe(le, be);
        if (haveS) {
            if ((off = memFind(sw, USCAN_WIN, le, 16)) >= 0)
                { hit = 1; hlevel = 0; hoff = off; hlay = 0; }
            else if ((off = memFind(sw, USCAN_WIN, be, 16)) >= 0)
                { hit = 1; hlevel = 0; hoff = off; hlay = 1; }
        }
        if (!hit && haveI) {
            if ((off = memFind(iw, USCAN_WIN, le, 16)) >= 0)
                { hit = 1; hlevel = 1; hoff = off; hlay = 0; }
            else if ((off = memFind(iw, USCAN_WIN, be, 16)) >= 0)
                { hit = 1; hlevel = 1; hoff = off; hlay = 1; }
        }
        if (!hit) {
            fmtUuid(le, s, sizeof s);          /* canonical text      */
            for (k = 0; k < 36; k++) {         /* UTF-16 in both objs */
                pat[2 * k]     = (unsigned char) s[k];
                pat[2 * k + 1] = 0;
            }
            if (haveS && (off = memFind(sw, USCAN_WIN, pat, 72)) >= 0)
                { hit = 1; hlevel = 0; hoff = off; hlay = 2; }
            else if (haveI && (off = memFind(iw, USCAN_WIN, pat, 72)) >= 0)
                { hit = 1; hlevel = 1; hoff = off; hlay = 2; }
            if (hit)
                memcpy(hu, le, 16);
            else
                continue;
        } else
            memcpy(hu, le, 16);
        break;
    }
    if (hit) {
        g_uLoc.level = hlevel;
        g_uLoc.off   = hoff;
        g_uLoc.lay   = hlay;
        memcpy(u, hu, 16);
        fmtUuid(u, ustr, cap);
        if (InterlockedDecrement(&g_uScanLogged) >= 0)
            g_host->log("merge_select: uuidscan: %s at %s+0x%X [%s]",
                        ustr, hlevel ? "impl" : "self", hoff, layN[hlay]);
        return 1;
    }
    if (InterlockedDecrement(&g_uMissLogged) >= 0) {
        char hs[129];
        for (i = 0; i < 32; i++)
            _snprintf_s(hs + 2 * i, 3, _TRUNCATE, "%02X",
                        haveS ? sw[i] : iw[i]);
        g_host->log("merge_select: uuidscan: no hit (targets=%d) "
                    "self[0..31]=%s self+0x38=%02X%02X%02X%02X",
                    n, hs,
                    haveS ? sw[0x38] : 0, haveS ? sw[0x39] : 0,
                    haveS ? sw[0x3A] : 0, haveS ? sw[0x3B] : 0);
    }
    return 0;
}

/* wrapper channel: the merge loop (verified in Sn3DMultiProjects
 * getMeshData) iterates items with the scan QUuid at wrapper+0x38 and
 * the HandlePrjFile pointer at wrapper+0x28, loading rcx=[rax+0x28]
 * right before `call [isGenerateCloud]`.  A hand-built thunk therefore
 * hands us rax=wrapper candidate; it is accepted only when
 * [candidate+0x28] == self, so no other call path can be fooled. */
static int wrapperUuid(void *self, void *w, unsigned char *u)
{
    void *fp = NULL;

    if (!w)
        return 0;
    __try {
        fp = *(void **)((unsigned char *)w + 0x28);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (fp != self)
        return 0;
    {
        unsigned char raw16[16], alt[16];
        int k, zero = 1;

        __try {
            memcpy(raw16, (const unsigned char *)w + 0x38, 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        uuidLeToBe(raw16, alt);          /* other byte-order candidate */
        /* QUuid is {uint,ushort,ushort,uchar[8]} and data1..data3 are
         * little-endian in memory, so the raw member bytes ARE the
         * canonical struct order the eye map uses; `alt` is the
         * text/wire order.  Live proof 2026-11-24: raw gives version-4
         * uuids (…-4EF5-…, …-4960-…, …-4D1D-…) while alt does not. */
        if (eyeLookup(alt) >= 0 && eyeLookup(raw16) < 0)
            memcpy(u, alt, 16);          /* wire order, only if it hits */
        else
            memcpy(u, raw16, 16);        /* member is already struct   */
        for (k = 0; k < 8; k++)
            if (u[k]) { zero = 0; break; }
        if (zero)
            return 0;
    }
    return 1;
}

/* rescan while new eye uuids show up: the eye map is usually empty at
 * the first gate call of a session, so one static round is useless */
static void gateEyeUuid(void *self, unsigned long long impl, void *wrapper,
                        unsigned char *u, char *ustr, int cap, int *have)
{
    unsigned char raw[16];
    static int s_lastN;
    int n = 0, i;

    *have = 0;
    ustr[0] = 0;
    memset(u, 0, 16);

    /* best case: eye state recorded in this very call */
    if (wrapperUuid(self, wrapper, u)) {
        fmtUuid(u, ustr, cap);
        *have = 1;
        return;
    }

    if (g_uLoc.level >= 0) {
        unsigned char *src = g_uLoc.level == 0
            ? (unsigned char *)self + g_uLoc.off
            : (unsigned char *)(size_t)impl + g_uLoc.off;
        if (g_uLoc.lay == 2) {
            wchar_t t[40];
            __try {
                memcpy(t, src, 72);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_uLoc.level = -1;
            }
            if (g_uLoc.level >= 0 && uuidFromText(t, u)) {
                fmtUuid(u, ustr, cap);
                *have = 1;
                return;
            }
        } else {
            __try {
                memcpy(raw, src, 16);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_uLoc.level = -1;
            }
            if (g_uLoc.level >= 0) {
                if (g_uLoc.lay == 0)
                    memcpy(u, raw, 16);
                else
                    uuidLeToBe(raw, u);
                fmtUuid(u, ustr, cap);
                *have = 1;
                return;
            }
        }
    }
    if (!g_eyeGate)
        return;
    for (i = 0; i < EYE_MAX; i++)
        if (g_eye[i].seq)
            n++;
    if (n > s_lastN
        || (n > 0 && InterlockedDecrement(&g_uScanTries) >= 0)) {
        s_lastN = n;
        if (uuidScan(self, impl, u, ustr, cap)) {
            *have = 1;
            return;
        }
    }
    __try {
        memcpy(raw, (const unsigned char *)self + 0x38, 16);
        *have = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!*have)
        return;
    if (!raw[0] && !raw[1] && !raw[2] && !raw[3]
        && !raw[4] && !raw[5] && !raw[6] && !raw[7]) {
        *have = 0;                       /* zero padding, not an id   */
        memset(u, 0, 16);
        return;
    }
    uuidLeToBe(raw, u);
    fmtUuid(u, ustr, cap);
}

static void eyeRecord(const unsigned char *u, int visible)
{
    char s[64];
    wchar_t ws[64];
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
    fmtUuid(u, s, sizeof s);
    /* persist the eye state into the shared selection .ini as a
     * [uuid] section: the same file every process with this module
     * consults at the gate (the fusion engine has no QML of its own,
     * the hub UI is the only writer). Hidden writes included = 0;
     * showing removes the section, so the file stays a clean list of
     * exclusions. NOTE: the vendor resets the tree to "all visible"
     * when a project re-opens, so these sections describe the current
     * session, like the eye icons themselves. */
    if (g_eyeGate && MultiByteToWideChar(CP_UTF8, 0, s, -1, ws,
                                         (int)(sizeof ws / sizeof ws[0])) > 0) {
        if (visible)
            WritePrivateProfileStringW(ws, NULL, NULL, g_selFile);
        else
            WritePrivateProfileStringW(ws, L"included", L"0", g_selFile);
    }
    if (g_verbose && InterlockedDecrement(&g_eyeLogged) >= 0)
        g_host->log("merge_select: eye %s -> %s",
                    s, visible ? "VISIBLE" : "hidden");
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
     * observe the vendor's show/hide slot (record-only, ini switch).
     * The icons live in the QML UI process only; arming the metacall
     * detour inside a service process would be an unnecessary risk. */
    if (!g_broken && g_eyewatch) {
        if (exeAllowed("eyewatch_exes", "EXStar Hub.exe")) {
            ULONGLONG t1 = GetTickCount64();
            while (!g_eyeDone && GetTickCount64() - t1 < 600000) {
                if (eyeTry())
                    break;
                WaitForSingleObject(g_wake, 1000);
            }
        } else {
            g_host->log("merge_select: eye watch not needed in %s "
                        "(eyewatch_exes)", g_exeName);
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

    {   /* the exe name drives the hook allowlists - resolve it before the
         * worker thread starts */
        char exe[MAX_PATH] = "?", *b;
        if (GetModuleFileNameA(NULL, exe, sizeof exe) > 0
            && (b = strrchr(exe, '\\')) != NULL)
            memmove(exe, b + 1, strlen(b + 1) + 1);
        strcpy_s(g_exeName, sizeof g_exeName, exe);
    }

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
