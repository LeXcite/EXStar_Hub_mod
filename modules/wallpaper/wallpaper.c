/*
 * wallpaper - replace the EXStar Hub application wallpaper in memory.
 *
 * What it does:
 *   The vendor paints the main window background from a JPEG embedded
 *   uncompressed in the .rdata section of AppUi.dll (referenced from the
 *   QML resource tree as the `backgroundImg` / MainBackground image).
 *   When the user dropped a picture through the Mod Manager
 *   (modules\wallpaper.jpg, converted to fit the slot budget), this
 *   module overwrites the embedded JPEG in the mapped image - the moment
 *   AppUi.dll is loaded, before Qt ever decodes it. Nothing on disk
 *   changes: deleting modules\wallpaper.jpg (Manager: "Reset") restores
 *   the stock wallpaper, the DLL file itself is never modified.
 *
 * Mechanism (same shape as popup_dim):
 *   - LdrRegisterDllNotification: AppUi.dll is caught the instant it is
 *     mapped, before its DllMain; a worker thread additionally handles
 *     the already-mapped case and acts as a polling fallback;
 *   - the JPEG slot is found structurally, never by a stored offset:
 *     scan readable non-writable sections for `FF D8 FF` and take the
 *     largest JPEG whose SOI..first-`FF D9` extent is at least
 *     `min_slot` bytes - UI icons are small, the wallpaper is the
 *     dominant JPEG in the file;
 *   - the replacement must fit the slot (new size <= slot size); the
 *     Mod Manager guarantees this by re-encoding with decreasing
 *     quality, the module re-checks and refuses if it does not hold;
 *   - every dereference is SEH-guarded, every write goes through one
 *     VirtualProtect covering the whole slot; a fault simply skips.
 *
 * Configuration (modules\wallpaper.ini):
 *   target    = AppUi.dll    DLL carrying the wallpaper image
 *   file      = wallpaper.jpg  image file next to this module's folder
 *   min_slot  = 300000       smallest JPEG accepted as the wallpaper slot
 *   exe       = hub          only run in "exstar hub.exe" (skip siblings
 *                            like scanservice.exe - set empty to allow all)
 *   verbose   = 1            log details to mod.log
 * Enable/disable: [wallpaper] enabled = 0|1 in modules.ini.
 *
 * Failure modes (by design all silent for the app):
 *   - no wallpaper.jpg            -> module logs once, never patches;
 *   - image bigger than the slot  -> logged refusal, stock wallpaper;
 *   - JPEG not found in target    -> vendor moved the resource, patch
 *                                    skipped, no side effects.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exstar_sdk.h"

#define MOD_VERSION   "1.0.0"
#define MAX_FILE      (3u * 1024 * 1024)   /* hard cap on the user image */

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "wallpaper",
    "Wallpaper",
    MOD_VERSION,
    "custom application background image instead of the stock one"
};

static const ExHost *g_host;
static HMODULE       g_self;

static char  g_target[64]   = "AppUi.dll";
static char  g_fileRel[128] = "wallpaper.jpg";
static char  g_exeFilter[64] = "hub";    /* main exe must contain this  */
static DWORD g_minSlot      = 300000;
static int   g_verbose      = 1;
static int   g_armed;                    /* replacement image loaded     */

/* replacement image, padded to the slot size at patch time */
static unsigned char *g_img;
static DWORD          g_imgSize;

static int g_done;                       /* slot patched in this process */

/* ------------------------------------------------------------------ */
/* helpers (same shapes as popup_dim)                                  */
/* ------------------------------------------------------------------ */

static long findBytes(const unsigned char *p, size_t len,
                      const unsigned char *needle, size_t n)
{
    size_t i;

    if (len < n)
        return -1;
    __try {
        for (i = 0; i + n <= len; i++)
            if (p[i] == needle[0]
                && memcmp(p + i, needle, n) == 0)
                return (long)i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;                       /* unreadable section */
    }
    return -1;
}

static void lowerCopy(const char *s, char *out, int cap)
{
    int i;
    for (i = 0; s[i] && i < cap - 1; i++)
        out[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    out[i] = 0;
}

/*
 * Slot size of the JPEG starting at SOI: distance to the first `FF D9`.
 * JPEG entropy-coded segments byte-stuff every 0xFF with 0x00, so an
 * unescaped 0xFF inside scan data is always a marker - the first D9 is
 * the true end (verified against the vendor wallpaper blob: the size it
 * yields equals the resource size exactly).
 */
static DWORD jpegSlotSize(const unsigned char *p, size_t cap)
{
    long o = findBytes(p + 3, cap - 3, (const unsigned char *)"\xFF\xD9", 2);

    if (o < 0)
        return 0;
    return (DWORD)(3 + o + 2);           /* from SOI through the EOI pair */
}

/* ------------------------------------------------------------------ */
/* slot search + patch                                                 */
/* ------------------------------------------------------------------ */

/* one pass over the image sections: find the largest JPEG slot >=
 * g_minSlot and overwrite it with the replacement. 1 = patched. */
static int patchImage(HMODULE mod, const char *who)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int i, patched = 0;
    unsigned char *best = NULL;
    DWORD bestSize = 0;

    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96)
            return 0;
        sec = IMAGE_FIRST_SECTION(nt);

        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            unsigned char *p;
            size_t len, off = 0;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            while (off + 4 <= len) {
                long o = findBytes(p + off, len - off,
                                   (const unsigned char *)"\xFF\xD8\xFF", 3);
                DWORD slot;
                unsigned char *cand;

                if (o < 0)
                    break;               /* absent / unreadable */
                cand = p + off + (size_t)o;      /* the SOI itself */
                slot = jpegSlotSize(cand, len - off - (size_t)o);
                if (slot >= g_minSlot && slot > bestSize) {
                    best     = cand;     /* cand == SOI start */
                    bestSize = slot;
                }
                off += (size_t)o + 3;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;                        /* image vanished mid-scan */
    }

    if (!best) {
        if (g_verbose)
            g_host->log("wallpaper: no JPEG slot >= %lu found in %s",
                        (unsigned long)g_minSlot, who);
        return 0;
    }
    if (g_imgSize > bestSize) {
        g_host->log("wallpaper: image %lu B does not fit slot %lu B in %s"
                    " - keeping stock",
                    (unsigned long)g_imgSize, (unsigned long)bestSize, who);
        return 0;
    }

    {   /* one protection change for header + body (the qrc lesson) */
        DWORD oldp;
        int   ok = 0;
        int   vp = 0, same = 0, av = 0;

        __try {
            vp = VirtualProtect(best, bestSize, PAGE_READWRITE, &oldp);
            if (vp) {
                if (memcmp(best, "\xFF\xD8\xFF", 3) != 0) {
                    av = 0;
                    same = -1;             /* SOI vanished mid-scan     */
                } else {
                    same = (memcmp(best + 3, g_img + 3, g_imgSize - 3) == 0);
                    if (!same) {
                        memcpy(best + 3, g_img + 3, g_imgSize - 3);
                        memset(best + g_imgSize, 0, bestSize - g_imgSize);
                        ok = 1;            /* replaced in this pass     */
                    }
                }
                VirtualProtect(best, bestSize, oldp, &oldp);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            av = 1;
        }
        if (ok) {
            patched = 1;
            g_host->log("wallpaper: %s slot +%lu x %lu B <- %lu B image (ok)",
                        who, (unsigned long)(best - base),
                        (unsigned long)bestSize, (unsigned long)g_imgSize);
        } else {
            g_host->log("wallpaper: %s slot +%lu x %lu B skipped "
                        "(vp=%d same=%d av=%d)",
                        who, (unsigned long)(best - base),
                        (unsigned long)bestSize, vp, same, av);
        }
    }
    return patched;
}

static HMODULE g_seen[8];
static int     g_nSeen;

static int seenMod(HMODULE h)
{
    int i;

    for (i = 0; i < g_nSeen; i++)
        if (g_seen[i] == h)
            return 1;
    if (g_nSeen < (int)(sizeof g_seen / sizeof g_seen[0]))
        g_seen[g_nSeen++] = h;
    return 0;
}

static void patchModule(HMODULE mod, const char *name)
{
    char low[128], tl[64];

    if (g_done || !g_armed)
        return;
    lowerCopy(name, low, (int)sizeof low);
    lowerCopy(g_target, tl, (int)sizeof tl);
    if (!strstr(low, tl) || seenMod(mod))
        return;
    if (patchImage(mod, name))
        g_done = 1;
}

/* ------------------------------------------------------------------ */
/* DLL load notification (mirror of the popup_dim shapes)              */
/* ------------------------------------------------------------------ */

typedef struct _WP_USTR {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} WP_USTR;
typedef struct _WP_NOTIF {
    ULONG    Flags;
    WP_USTR *FullDllName;
    WP_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} WP_NOTIF;

static void *g_notifCookie;

static void WINAPI wp_notify(unsigned reason, const WP_NOTIF *d, void *ctx)
{
    WCHAR *wn;
    char   name[260];
    int    j;

    (void)ctx;
    if (reason != 1 || !d || !d->DllBase || !d->BaseDllName)
        return;
    wn = d->BaseDllName->Buffer;
    for (j = 0; wn && (j * 2) < (int)d->BaseDllName->Length && j < 259; j++)
        name[j] = (char)wn[j];
    name[j] = 0;
    patchModule((HMODULE)d->DllBase, name);
}

static int armNotify(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NTSTATUS (__stdcall *reg)(unsigned, void *, void *, void **);

    if (!ntdll)
        return 0;
    reg = (NTSTATUS (__stdcall *)(unsigned, void *, void *, void **))
              (void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg)
        return 0;
    return reg(0, (void *)wp_notify, NULL, &g_notifCookie) == 0;
}

/* ------------------------------------------------------------------ */
/* worker: catch an already-mapped target + polling fallback           */
/* ------------------------------------------------------------------ */

static unsigned WINAPI worker(LPVOID arg)
{
    int notif;
    ULONGLONG t0 = GetTickCount64();

    (void)arg;
    notif = armNotify();
    while (!g_done) {
        HMODULE h = GetModuleHandleA(g_target);   /* case-insensitive */

        if (h)
            patchModule(h, g_target);
        if (g_done || GetTickCount64() - t0 > 300000)
            break;
        Sleep(GetTickCount64() - t0 < 10000 ? 20 : 500);
    }
    if (notif && g_verbose && !g_done)
        g_host->log("wallpaper: armed, target still unmapped after 5 min");
    return 0;
}

/* ------------------------------------------------------------------ */
/* replacement image load                                              */
/* ------------------------------------------------------------------ */

static int loadImage(void)
{
    char dir[MAX_PATH], slash, path[MAX_PATH];
    HANDLE f;
    DWORD size, rd;
    unsigned char *buf;
    int rc = 0;

    /* the image lives next to the module DLL (the host loads modules
     * from <app>\modules\, so the module file IS in that folder) */
    if (!GetModuleFileNameA(g_self, dir, sizeof dir))
        return 0;
    {
        char *p = strrchr(dir, '\\');
        if (!p)
            return 0;
        slash = *p;
        *p = 0;
    }
    _snprintf(path, sizeof path - 1, "%s%c%s", dir, slash, g_fileRel);
    path[sizeof path - 1] = 0;

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        if (g_verbose)
            g_host->log("wallpaper: no %s - stock wallpaper kept", path);
        return 0;
    }
    size = GetFileSize(f, NULL);
    if (!size || size > MAX_FILE
        || (buf = (unsigned char *)malloc(size)) == NULL) {
        CloseHandle(f);
        g_host->log("wallpaper: %s unusable (size %lu)", path,
                    (unsigned long)size);
        return 0;
    }
    if (ReadFile(f, buf, size, &rd, NULL) && rd == size
        && size > 4 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
        g_img     = buf;
        g_imgSize = size;
        rc        = 1;
    } else {
        g_host->log("wallpaper: %s is not a JPEG", path);
    }
    if (!rc)
        free(buf);
    CloseHandle(f);
    return rc;
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
    unsigned tid;
    HANDLE th;
    char buf[128];

    g_host = host;

    if (host->cfg_string(host, "target", buf, sizeof buf))
        strncpy(g_target, buf, sizeof g_target - 1);
    if (host->cfg_string(host, "file", buf, sizeof buf))
        strncpy(g_fileRel, buf, sizeof g_fileRel - 1);
    g_minSlot = (DWORD)host->cfg_int(host, "min_slot", 300000);
    g_verbose = host->cfg_int(host, "verbose", 1);
    if (host->cfg_string(host, "exe", buf, sizeof buf))
        strncpy(g_exeFilter, buf, sizeof g_exeFilter - 1);

    {   /* UI-process gate: the wallpaper lives in the hub window, a
         * sibling process (scanservice.exe) must not carry the module */
        char exe[MAX_PATH], low[MAX_PATH];

        if (g_exeFilter[0]) {
            if (!GetModuleFileNameA(NULL, exe, sizeof exe))
                return 3;
            lowerCopy(exe, low, (int)sizeof low);
            if (!strstr(low, g_exeFilter)) {
                if (g_verbose)
                    host->log("wallpaper: not the UI process (%s) - idle",
                              exe);
                return 3;
            }
        }
    }

    if (!loadImage())
        return 1;                        /* nothing to do: disable       */
    g_armed = 1;

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("wallpaper v%s armed in pid=%lu: %lu B image, target %s",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              (unsigned long)g_imgSize, g_target);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* nothing to undo: the image lives in the mapped DLL view and dies
     * with the process */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)reason;
    (void)r;
    if (reason == DLL_PROCESS_ATTACH)
        g_self = (HMODULE)h;             /* pure capture, no work */
    return TRUE;                         /* never fail */
}
