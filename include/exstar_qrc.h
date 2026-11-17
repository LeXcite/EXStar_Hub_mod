/*
 * exstar_qrc.h - shared machinery for modules that rewrite compressed
 * QML resources (Qt "zrc" blobs) inside a mapped vendor DLL. Used by
 * the fps and laser50 modules; popup_dim predates it and keeps its own
 * equal-length variant (do not refactor a live-verified module for
 * style points).
 *
 * The resource model (reverse-engineered, see modules/fps/README.md):
 *   read-only sections of the vendor DLL contain records laid out as
 *   [u32 BE rawSize][zlib stream]; the app's own zlib1.dll inflates
 *   them at engine startup. A module catching the DLL via
 *   LdrRegisterDllNotification can rewrite a record in the mapped image
 *   before DllMain runs, so the first QML parse already sees the new
 *   text. Files on disk are never touched.
 *
 * Hard rules baked into this header:
 *   - the NEXT record's stream boundary may never move: the level-9
 *     re-deflate must fit the ORIGINAL compressed length (lc);
 *   - the stored rawSize u32 is updated together with the stream under
 *     one VirtualProtect pair;
 *   - a byte-exact backup is taken before writing; the patched record
 *     is re-inflated FROM THE IMAGE and byte-compared - any mismatch
 *     restores the backup verbatim;
 *   - every foreign-memory dereference runs under SEH;
 *   - nothing is ever logged here: the caller maps exq_result to its
 *     own log wording (log formats are cited in module READMEs).
 *
 * Header-only on purpose: modules are single .c files built with cl;
 * all functions are static (one private copy per module DLL). zlib is
 * resolved dynamically from the application's own zlib1.dll.
 */
#ifndef EXSTAR_QRC_H
#define EXSTAR_QRC_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define EXQ_ZLIB_VER  "1.2.11"            /* prefix-checked by zlib      */
#define EXQ_MAX_RAW   0x800000u           /* inflation ceiling           */

/* ------------------------------------------------------------------ */
/* zlib (the application's own zlib1.dll)                              */
/* ------------------------------------------------------------------ */

typedef struct _EXQ_ZS {
    unsigned char *next_in;
    unsigned       avail_in;
    unsigned long  total_in;
    unsigned char *next_out;
    unsigned       avail_out;
    unsigned long  total_out;
    char          *msg;
    void          *state;
    void          *zalloc;
    void          *zfree;
    void          *opaque;
    int            data_type;
    unsigned long  adler;
    unsigned long  reserve;
} EXQ_ZS;

typedef int (__cdecl *exq_f_zinit)(EXQ_ZS *, int, const char *, int);
typedef int (__cdecl *exq_f_zdinit)(EXQ_ZS *, int, int, int, int, int,
                                    const char *, int);
typedef int (__cdecl *exq_f_zrun)(EXQ_ZS *, int);
typedef int (__cdecl *exq_f_zend)(EXQ_ZS *);

static exq_f_zinit  exq_p_inflateInit2_;
static exq_f_zrun   exq_p_inflate;
static exq_f_zend   exq_p_inflateEnd;
static exq_f_zdinit exq_p_deflateInit2_;
static exq_f_zrun   exq_p_deflate;
static exq_f_zend   exq_p_deflateEnd;
static int          exq_zlibOk;

static void exq_loadZlib(void)
{
    HMODULE z = GetModuleHandleA("zlib1.dll");

    if (!z)
        z = LoadLibraryA("zlib1.dll");
    if (!z)
        z = LoadLibraryA("zlib.dll");
    if (!z)
        return;
    exq_p_inflateInit2_  = (exq_f_zinit)
        (void *)GetProcAddress(z, "inflateInit2_");
    exq_p_inflate        = (exq_f_zrun)
        (void *)GetProcAddress(z, "inflate");
    exq_p_inflateEnd     = (exq_f_zend)
        (void *)GetProcAddress(z, "inflateEnd");
    exq_p_deflateInit2_  = (exq_f_zdinit)
        (void *)GetProcAddress(z, "deflateInit2_");
    exq_p_deflate        = (exq_f_zrun)
        (void *)GetProcAddress(z, "deflate");
    exq_p_deflateEnd     = (exq_f_zend)
        (void *)GetProcAddress(z, "deflateEnd");
    exq_zlibOk = exq_p_inflateInit2_ && exq_p_inflate && exq_p_inflateEnd
              && exq_p_deflateInit2_ && exq_p_deflate && exq_p_deflateEnd;
}

/* ------------------------------------------------------------------ */
/* byte helpers                                                        */
/* ------------------------------------------------------------------ */

static void *exq_memmem(const void *b, size_t bl, const void *n, size_t nl)
{
    const unsigned char *p = (const unsigned char *)b;
    size_t i;

    if (!nl || bl < nl)
        return NULL;
    for (i = 0; i + nl <= bl; i++)
        if (p[i] == ((const unsigned char *)n)[0] && !memcmp(p + i, n, nl))
            return (void *)(p + i);
    return NULL;
}

static int exq_countBytes(const unsigned char *b, size_t n,
                          const void *pat, size_t pl)
{
    const unsigned char *p = b;
    const unsigned char *q = (const unsigned char *)pat;
    int c = 0;
    size_t i;

    for (i = 0; i + pl <= n; i++)
        if (p[i] == q[0] && !memcmp(p + i, q, pl))
            c++;
    return c;
}

/* ------------------------------------------------------------------ */
/* zlib framing                                                        */
/* ------------------------------------------------------------------ */

/* inflate the record at q ([u32 BE rawSize][stream]); returns malloc'd
 * text, *rawOut = declared size, *usedOut = consumed stream bytes */
static unsigned char *exq_inflateAt(const unsigned char *q, size_t avail,
                                    unsigned *rawOut, size_t *usedOut)
{
    unsigned raw = ((unsigned)q[0] << 24) | ((unsigned)q[1] << 16)
                 | ((unsigned)q[2] << 8) | (unsigned)q[3];
    unsigned char *buf;
    EXQ_ZS s;
    int r;

    if (raw < 64 || raw > EXQ_MAX_RAW)
        return NULL;
    buf = (unsigned char *)malloc(raw + 16);
    if (!buf)
        return NULL;
    memset(&s, 0, sizeof s);
    s.next_in   = (unsigned char *)q + 4;
    s.avail_in  = (unsigned)avail;
    s.next_out  = buf;
    s.avail_out = raw + 16;
    __try {
        if (exq_p_inflateInit2_(&s, 15, EXQ_ZLIB_VER, (int)sizeof s) != 0) {
            free(buf);
            return NULL;
        }
        r = exq_p_inflate(&s, 4);          /* Z_FINISH */
        if (r == 1 && s.total_out == raw) {
            *usedOut = (size_t)s.total_in;
            *rawOut  = raw;
            exq_p_inflateEnd(&s);
            return buf;
        }
        exq_p_inflateEnd(&s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* fall through */
    }
    free(buf);
    return NULL;
}

/* deflate rn bytes at level 9 into out[cap]; 0 = no fit / error */
static unsigned long exq_deflate9(const unsigned char *raw, size_t rn,
                                  unsigned char *out, size_t cap)
{
    EXQ_ZS d;
    unsigned long n = 0;

    memset(&d, 0, sizeof d);
    d.next_in   = (unsigned char *)raw;
    d.avail_in  = (unsigned)rn;
    d.next_out  = out;
    d.avail_out = (unsigned)cap;
    __try {
        if (exq_p_deflateInit2_(&d, 9, 8, 15, 8, 0, EXQ_ZLIB_VER,
                                (int)sizeof d) == 0) {
            if (exq_p_deflate(&d, 4) == 1)
                n = d.total_out;
            exq_p_deflateEnd(&d);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* the one safe write path                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int            ok;          /* 1 = patched and verified in place    */
    int            fits;        /* 0 = level-9 size > lc (`need` valid) */
    unsigned long  need;        /* bytes the patched stream would need  */
    unsigned long  packed;      /* new compressed size when ok          */
    DWORD          protErr;     /* VirtualProtect failure code          */
    int            verifyFail;  /* 1 = mismatch, backup was restored    */
} exq_result;

/* q points at the record's u32 rawSize header inside the mapped image,
 * lc is the ORIGINAL compressed length (space we may never exceed);
 * patched/newRaw is the intended inflated text. Never touches disk. */
static void exq_writeRecord(unsigned char *q, size_t lc,
                            const unsigned char *patched, size_t newRaw,
                            exq_result *res)
{
    unsigned char *comp, *bak;
    unsigned long nc;
    DWORD oldp;

    memset(res, 0, sizeof *res);
    res->fits = 1;

    comp = (unsigned char *)malloc(lc ? lc : 64);
    if (!comp)
        return;
    nc = exq_deflate9(patched, newRaw, comp, lc);
    if (!nc) {                            /* does not fit: measure only  */
        unsigned char *probe = (unsigned char *)malloc(lc + 0x40000);

        res->fits = 0;
        res->need = probe ? exq_deflate9(patched, newRaw, probe,
                                         lc + 0x40000) : 0;
        free(probe);
        free(comp);
        return;
    }

    bak = (unsigned char *)malloc(4 + lc);   /* byte-exact undo         */
    if (!bak) {
        free(comp);
        return;
    }
    memcpy(bak, q, 4 + lc);
    if (!VirtualProtect(q, 4 + lc, PAGE_READWRITE, &oldp)) {
        res->protErr = GetLastError();
        free(bak);
        free(comp);
        return;
    }
    {
        unsigned nr = (unsigned)newRaw;

        q[0] = (unsigned char)(nr >> 24);
        q[1] = (unsigned char)(nr >> 16);
        q[2] = (unsigned char)(nr >> 8);
        q[3] = (unsigned char)nr;
        memcpy(q + 4, comp, nc);
        memset(q + 4 + nc, 0, lc - nc);
    }
    VirtualProtect(q, 4 + lc, oldp, &oldp);

    {   /* re-inflate straight from the (write-restored) image: must now
         * decode to exactly the patched text */
        unsigned vr;
        size_t vu = 0;
        unsigned char *v = exq_inflateAt(q, 4 + lc, &vr, &vu);

        if (v) {
            res->ok = (vr == (unsigned)newRaw && memcmp(v, patched, newRaw) == 0);
            free(v);
        }
    }
    if (res->ok)
        res->packed = nc;
    if (!res->ok) {                       /* restore the record          */
        res->verifyFail = 1;
        if (VirtualProtect(q, 4 + lc, PAGE_READWRITE, &oldp)) {
            memcpy(q, bak, 4 + lc);
            VirtualProtect(q, 4 + lc, oldp, &oldp);
        }
    }
    free(bak);
    free(comp);
}

/* ------------------------------------------------------------------ */
/* image scan: every readable non-writable section, every record       */
/* ------------------------------------------------------------------ */

/* cb sees each plausible record start (rec -> u32 header); return 1
 * when it patched this record. cb must be reentrancy-aware or cheap. */
typedef int (*exq_rec_fn)(unsigned char *rec, size_t avail,
                          unsigned long recOff, void *ctx);

static LONG exq_scanBusy;                 /* loader-thread vs worker     */

static int exq_scanImage(HMODULE mod, unsigned minRaw, exq_rec_fn cb,
                         void *ctx)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int i, patched = 0;

    if (!exq_zlibOk)
        return 0;
    if (InterlockedCompareExchange(&exq_scanBusy, 1, 0))
        return 0;
    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            InterlockedExchange(&exq_scanBusy, 0);
            return 0;
        }
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96) {
            InterlockedExchange(&exq_scanBusy, 0);
            return 0;
        }
        sec = IMAGE_FIRST_SECTION(nt);
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            unsigned char *p;
            size_t len, off;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            for (off = 4; off + 6 < len; off++) {
                unsigned raw;

                if (p[off] != 0x78
                    || (((unsigned)p[off] << 8 | p[off + 1]) % 31) != 0)
                    continue;
                /* cheap pre-filter: a record of plausible size */
                raw = ((unsigned)p[off - 4] << 24)
                    | ((unsigned)p[off - 3] << 16)
                    | ((unsigned)p[off - 2] << 8)
                    | (unsigned)p[off - 1];
                if (raw < minRaw || raw > EXQ_MAX_RAW)
                    continue;
                if (cb(p + off - 4, len - (off - 4),
                       (unsigned long)(sec->VirtualAddress + off), ctx))
                    patched = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* image vanished mid-scan; keep any earlier patch */
    }
    InterlockedExchange(&exq_scanBusy, 0);
    return patched;
}

/* ------------------------------------------------------------------ */
/* DLL map notification                                                */
/* ------------------------------------------------------------------ */

typedef struct _EXQ_USTR {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} EXQ_USTR;
typedef struct _EXQ_NOTIF {
    ULONG    Flags;
    EXQ_USTR *FullDllName;
    EXQ_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} EXQ_NOTIF;

static void *exq_notifCookie;

/* fn sees every mapped DLL (base + ASCII base name); run the patch
 * inside it to land before the target's DllMain and before Qt parses
 * the QML */
typedef void (*exq_map_fn)(HMODULE base, const char *baseName, void *ctx);

static exq_map_fn exq_mapCb;
static void      *exq_mapCtx;

static void WINAPI exq_notifyThunk(unsigned reason, const EXQ_NOTIF *d,
                                   void *ctx)
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
    if (exq_mapCb)
        exq_mapCb((HMODULE)d->DllBase, name, exq_mapCtx);
}

static int exq_onMapped(exq_map_fn fn, void *ctx)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NTSTATUS (__stdcall *reg)(unsigned, void *, void *, void **);

    if (!ntdll)
        return 0;
    reg = (NTSTATUS (__stdcall *)(unsigned, void *, void *, void **))
              (void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg)
        return 0;
    exq_mapCb  = fn;
    exq_mapCtx = ctx;
    return reg(0, (void *)exq_notifyThunk, NULL, &exq_notifCookie) == 0;
}

/* case-insensitive substring test for target DLL name matching */
static int exq_ciContains(const char *hay, const char *needle)
{
    char low[128], tl[80];
    int i;

    for (i = 0; hay[i] && i < 127; i++)
        low[i] = (char)((hay[i] >= 'A' && hay[i] <= 'Z')
                        ? hay[i] + 32 : hay[i]);
    low[i] = 0;
    for (i = 0; needle[i] && i < 79; i++)
        tl[i] = (char)((needle[i] >= 'A' && needle[i] <= 'Z')
                       ? needle[i] + 32 : needle[i]);
    tl[i] = 0;
    return *tl && strstr(low, tl) != NULL;
}

#endif /* EXSTAR_QRC_H */
