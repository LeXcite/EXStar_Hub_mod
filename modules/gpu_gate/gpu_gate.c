/*
 * gpu_gate.c - let EXStar Hub run on integrated GPUs (e.g. Radeon 780M).
 *
 * Version 1.0.0
 *
 * The problem (verified on product 2.1.0-10; see
 * docs/research/gpu-gate-amd-support.md for the full evidence chain):
 *   The hub asks scanservice (Sn3DAlgorithmWrapper.dll) to run
 *   Sn3DSpeckleNew!Sn3DSpeckleNew::device_detect() during the splash
 *   screen. That function is a Vulkan device checker whose very first
 *   policy test is
 *      mov  eax, [rbp+0x300]   ; VkPhysicalDeviceProperties.deviceType
 *      cmp  eax, 2              ; VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
 *      jne  out_of_scope        ; everything else is refused
 *   An integrated GPU (deviceType 1, e.g. a Radeon 780M in an
 *   iGPU-only box) never reaches the vendor's own AMD/NVIDIA model
 *   checks, device_detect returns 5, the hub answers with a
 *   "requires NVIDIA GeForce RTX 3060 / Vulkan 1.2" dialog and
 *   publishes quitApp. The pipeline behind that gate is pure Vulkan;
 *   the refusal is policy, not necessity.
 *
 * What this module does:
 *   One byte, in memory, in the process-private copy of the mapped
 *   image: it turns the gate's `jne` (0F 85) into `ja` (0F 87), i.e.
 *      passes:          OTHER(0), INTEGRATED(1), DISCRETE(2)
 *      still refused:   VIRTUAL_GPU(3), CPU(4)
 *   Integrated cards then walk the vendor's own branches; the AMD
 *   model regex does not match names like "Radeon 780M", and the
 *   vendor code itself ends with "set to success out of caution"
 *   (return 1) - the same value a discrete card produces. Nothing is
 *   spoofed: the hub receives a normal, vendor-computed success.
 *
 * Anchoring (project rule: no hardcoded RVAs):
 *   The byte is located by the full 11-byte instruction sequence
 *      8B 85 00 03 00 00 83 F8 02 0F 85
 *   which is UNIQUE inside Sn3DSpeckleNew.dll (verified over the whole
 *   file). Any other finding - no match, several matches, or a mix of
 *   the strict and the relaxed form - means the vendor changed the
 *   code in a way this module does not understand: it then refuses to
 *   patch, logs why, and leaves the application stock.
 *
 * Safety:
 *   - only read-only image sections are scanned, every raw-memory
 *     access is SEH-guarded;
 *   - the write is one byte via VirtualProtect (copy-on-write private
 *     page): vendor files, signatures and Authenticode stay intact, a
 *     restart restores stock behaviour, disable + restart reverts;
 *   - runs in every injected process and patches each process that
 *     maps Sn3DSpeckleNew.dll (live 2.1.0-10 observation:
 *     scanservice.exe and the hub UI process); everywhere else the
 *     module idles;
 *   - behavior-neutral on machines that already pass: deviceType 2
 *     passes both `jne` and `ja` (self-tested on NVIDIA RTX 4090,
 *     vendor logs byte-identical);
 *   - every failure path logs and leaves the application untouched.
 *
 * History:
 *   1.0.0  initial version; pattern + gate semantics static-verified
 *          on 2.1.0-10, relaxed branch field-verified on 2.0.0-45 on a
 *          Radeon 780M, module self-tested on NVIDIA hardware.
 */

#include <windows.h>
#include <string.h>
#include <ctype.h>

#include "exstar_sdk.h"

#define MOD_VERSION   "1.0.0"
#define MAX_TARGETS   8

static const ExHost *g_host;
static int   g_verbose = 1;
static char  g_targets[MAX_TARGETS][64];  /* stored lowercase            */
static int   g_nTargets;
static volatile LONG g_done;              /* "decision made", one-shot   */

/* gate instruction: mov eax,[rbp+300h]; cmp eax,2; jne out_of_scope */
static const unsigned char kGate[] = {
    0x8B, 0x85, 0x00, 0x03, 0x00, 0x00, 0x83, 0xF8, 0x02, 0x0F, 0x85
};
#define GATE_HIT   10     /* index of the jne opcode tail (85 -> 87)     */
#define JNE_TAIL   0x85
#define JA_TAIL    0x87

static const ExModuleInfo g_info = {
    EX_ABI_VERSION, "gpu_gate", "GPU Gate", MOD_VERSION,
    "integrated GPUs pass the vendor startup GPU check (memory gate byte)"
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* find needle in [p, p+len); offset, -1 absent, -2 unreadable. SEH. */
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
        return -2;
    }
    return -1;
}

static void lowerCopy(const char *s, char *out, int cap)
{
    int i;
    for (i = 0; s[i] && i < cap - 1; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* the patch                                                           */
/* ------------------------------------------------------------------ */

/* count matches of one variant in [p, p+len), stop after 2 (enough to
 * decide); sets *first to the first offset when found. */
static int countIn(const unsigned char *p, size_t len, int relaxed,
                   size_t *first)
{
    unsigned char pat[sizeof kGate];
    int n = 0;
    size_t off = 0;

    memcpy(pat, kGate, sizeof pat);
    pat[GATE_HIT] = (unsigned char)(relaxed ? JA_TAIL : JNE_TAIL);
    while (off < len) {
        long o = findBytes(p + off, len - off, pat, sizeof pat);
        if (o < 0)
            break;                        /* o == -2: unreadable section */
        if (!n)
            *first = off + (size_t)o;
        n++;
        off += (size_t)o + sizeof pat;    /* instructions never overlap  */
        if (n >= 2)
            break;
    }
    return n;
}

/* One module image: count both variants (stock `jne` and relaxed `ja`)
 * across all read-only sections, then relax the stock byte only when it
 * is the single, unambiguous match in the whole image. */
static void relaxGate(HMODULE mod, const char *who)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int i, nStrict = 0, nRelaxed = 0;
    size_t hit = 0, secVa = 0;

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
            size_t len, f = 0;
            int c;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;                 /* scan read-only code/rodata  */
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            if (len < sizeof kGate)
                continue;
            c = countIn(p, len, 0, &f);
            if (c) {
                nStrict += c;
                hit = f; secVa = sec->VirtualAddress;
            }
            c = countIn(p, len, 1, &f);
            nRelaxed += c;
            if (nStrict + nRelaxed > 4)
                break;                    /* something is very odd       */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                           /* image unreadable            */
    }

    if (!nStrict) {
        if (nRelaxed && g_verbose)
            g_host->log("gpu_gate: %s gate already relaxed (before us) - "
                        "nothing to do", who);
        else if (g_verbose)
            g_host->log("gpu_gate: %s mapped, gate pattern absent "
                        "(vendor code changed?) - staying out of it", who);
        return;
    }
    if (nStrict > 1 || nRelaxed) {
        g_host->log("gpu_gate: %s has %d stock + %d relaxed gate matches "
                    "- refusing to guess, app stays stock",
                    who, nStrict, nRelaxed);
        return;
    }

    /* exactly one stock hit: flip 0F 85 (jne) -> 0F 87 (ja) */
    {
        DWORD oldp;
        unsigned char *q = base + secVa + hit + GATE_HIT;

        __try {
            if (VirtualProtect(q, 1, PAGE_READWRITE, &oldp)) {
                *q = JA_TAIL;
                VirtualProtect(q, 1, oldp, &oldp);
                FlushInstructionCache(GetCurrentProcess(), q, 1);
                g_host->log("gpu_gate: %s gate relaxed at RVA 0x%X "
                            "(jne -> ja: integrated GPUs now reach the "
                            "vendor's own AMD/NVIDIA checks)",
                            who, (unsigned)(secVa + hit));
            } else if (g_verbose) {
                g_host->log("gpu_gate: %s gate found but the page refused "
                            "the write (err=%lu) - app stays stock",
                            who, (unsigned long)GetLastError());
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_host->log("gpu_gate: %s gate write faulted - app stays stock",
                        who);
        }
    }
}

static int isTarget(const char *nameLower)
{
    int i;
    for (i = 0; i < g_nTargets; i++)
        if (strstr(nameLower, g_targets[i]))
            return 1;
    return 0;
}

/* The loader-lock callback and the polling worker race to claim the
 * target; only the winner ever reaches relaxGate, so the byte is
 * examined and flipped exactly once per process. */
static void patchOnce(HMODULE mod, const char *name)
{
    char low[260];

    if (g_done)
        return;
    lowerCopy(name, low, sizeof low);
    if (!isTarget(low))
        return;
    if (InterlockedExchange(&g_done, 1))
        return;                           /* lost the claim              */
    relaxGate(mod, name);
}

/* ------------------------------------------------------------------ */
/* DLL load notification: patch the image the moment it maps, before   */
/* anyone can call device_detect. This also beats the wrapper plugin   */
/* that imports it statically.                                         */
/* ------------------------------------------------------------------ */

#define LDR_DLL_LOADED  1   /* LDR_DLL_NOTIFICATION_REASON_LOADED        */

/* LDR_DLL_NOTIFICATION_DATA mirrored here: <windows.h> does not
 * expose it. Field sizes/order are ABI; do not touch. */
typedef struct _GG_USTR {
    unsigned short Length;            /* bytes, not characters           */
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} GG_USTR;
typedef struct _GG_NOTIF {
    ULONG    Flags;
    GG_USTR *FullDllName;
    GG_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} GG_NOTIF;
typedef NTSTATUS (NTAPI *GG_REGNOTIF)(ULONG flags, PVOID fn, PVOID ctx,
                                      PVOID *cookie);

static void *g_notifCookie;             /* unregister is pointless: the  */
                                        /* callback dies with the process */

static void WINAPI gg_notify(unsigned reason, const GG_NOTIF *d, void *ctx)
{
    WCHAR *wn;
    char   name[260];
    int    j;

    (void)ctx;
    if (reason != LDR_DLL_LOADED || !d || !d->DllBase || !d->BaseDllName)
        return;
    /* narrow the wide basename; non-ASCII names cannot match an ASCII
     * target anyway, truncating them is harmless */
    wn = d->BaseDllName->Buffer;
    for (j = 0; wn && (j * 2) < (int)d->BaseDllName->Length && j < 259; j++)
        name[j] = (char)wn[j];
    name[j] = 0;
    patchOnce((HMODULE)d->DllBase, name);
}

static int armNotify(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    GG_REGNOTIF reg;

    if (!ntdll)
        return 0;
    reg = (GG_REGNOTIF)(void *)
              GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg)
        return 0;
    return reg(0, (void *)gg_notify, NULL, &g_notifCookie) == 0;
}

/* ------------------------------------------------------------------ */
/* worker: arm the load notification, and poll as a safety net for a   */
/* target that was somehow already mapped before ex_init() ran         */
/* ------------------------------------------------------------------ */

static unsigned WINAPI worker(LPVOID arg)
{
    ULONGLONG t0 = GetTickCount64();
    int notif;

    (void)arg;
    notif = armNotify();
    if (g_verbose)
        g_host->log("gpu_gate: %s", notif
            ? "watch armed (dll load notification + polling fallback)"
            : "dll notification unavailable, polling fallback only");

    while (!g_done) {
        int i;
        for (i = 0; i < g_nTargets; i++) {
            HMODULE h = GetModuleHandleA(g_targets[i]); /* case-insens. */
            if (h) {
                patchOnce(h, g_targets[i]);
                if (g_done)
                    break;
            }
        }
        if (g_done)
            break;
        if (GetTickCount64() - t0 > 300000) {
            /* hub and scanservice map the target within seconds; any
             * other process never will */
            if (g_verbose)
                g_host->log("gpu_gate: target DLL never mapped in this "
                            "process - nothing to do here");
            break;
        }
        Sleep(GetTickCount64() - t0 < 10000 ? 20 : 500);
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
    char list[256];
    char *tok, *save = NULL;
    unsigned tid;
    HANDLE th;

    g_host = host;
    g_verbose = host->cfg_int(host, "verbose", 1);

    if (!host->cfg_string(host, "modules", list, sizeof list))
        strcpy_s(list, sizeof list, "sn3dspecklenew.dll");
    for (tok = strtok_s(list, ",;", &save);
         tok && g_nTargets < MAX_TARGETS;
         tok = strtok_s(NULL, ",;", &save)) {
        while (*tok == ' ')
            tok++;
        if (!*tok)
            continue;
        lowerCopy(tok, g_targets[g_nTargets],
                  (int)sizeof g_targets[0]);
        g_nTargets++;
    }
    if (!g_nTargets) {
        host->log("gpu_gate: no target modules configured");
        return 1;
    }

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("gpu_gate v%s armed in pid=%lu: %d target module(s)",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_nTargets);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* nothing to undo: the byte lives in this process's private image
     * pages and dies with the process */
}
