/*
 * popup_dim.c - soften the full-screen grey curtain the vendor paints
 * behind modal popups (notifications, loading bars, errors) of
 * EXStar Hub 3.1.x.
 *
 * Where the curtain comes from (verified by live-patching a running hub):
 *   The whole UI is QML (Qt Quick). Modal popups dim the window through
 *   the `Overlay.modal` item of Qt Quick Controls 2. The vendor ships
 *   three different implementations, inside the Qt resource blobs of the
 *   UI DLLs (partly as plain text, partly zlib-compressed - see 4):
 *
 *     AppUi.dll            Overlay.modal: Item { Rectangle {
 *                              color: "#a7000000" } }         ~65% black
 *     CompositeComponents  Sn3DDialogWithOverLay.qml:
 *       (Sn3DDialog)           RecursiveBlur { source: $backgroundImg
 *                              radius: 7.5; loops: 10;
 *                              transparentBorder: true;
 *                              opacity: 1.0 }   <- the opaque blurred
 *                              background IS the curtain
 *                            BluredCoverEx.qml: blur + black rectangles
 *                              (loops1: 50 / opacity1: 0.6 variants)
 *     Sn3DCloudDisk.dll    cloud login modal: GaussianBlur{radius:16
 *                              samples:16 deviation:4} + Rectangle
 *                              color "#000000" opacity 0.5
 *
 * What this module does:
 *   1) Registers an LdrRegisterDllNotification callback, so every target
 *      DLL is patched the instant it is mapped - BEFORE its DllMain runs
 *      and long before the QML engine reads its resource blob. Already
 *      mapped modules are patched at once; a short polling loop covers
 *      the (unlikely) case where the notification cannot be registered.
 *   2) Applies a table of literal byte patches (context anchored,
 *      CRLF aware) that turn the opaque blurred backdrop into a
 *      transparent one and soften the blur radius/loops. The
 *      `dim_alpha` percentage drives the overlay opacity (quantized to
 *      0.1 steps, which is what the QML literals offer) for the modal
 *      overlays; the blur softening itself is on/off (`soft_blur`).
 *   3) Keeps the v1 pass: rewrite the ARGB alpha of pure-black
 *      `Overlay.modal: ... color: "#aa000000"` literals to dim_alpha.
 *   4) Compressed QML: AppUi/ScanPage_E7/pluginSn3DNavigation keep much
 *      of their QML zlib-compressed inside the qrc blob (records
 *      `[u32 BE rawSize][zlib stream]`), and the loading/message overlay
 *      copies of the `RecursiveBlur { source: $backgroundImg ... }`
 *      curtain live there. applyZrc() inflates each record from the
 *      mapped image, dims those blocks in the text (radius/loops per
 *      soft_blur, opacity per dim_alpha) and re-deflates into the
 *      original compressed size, header + body written under one
 *      VirtualProtect (the qrc pages are read-only; writing the header
 *      before unprotecting faults the whole pass - fixed in 1.2.2).
 *      Every rewritten record is backed up first and re-inflated from
 *      the patched memory right after the write; if it no longer decodes
 *      the bytes are restored (verified/restored counters in mod.log).
 *      A per-image guard skips the second load-notification fire so the
 *      heavy pass runs exactly once per module (1.2.3).
 *      1.2.5/1.2.6 add the SECOND overlay family: `GaussianBlur` blocks.
 *      Only the busy curtains - overlays whose blur is paired with the
 *      opaque `Rectangle { color: "#aa000000" }` black-alpha veil
 *      ("Project loading...", "Exiting...", mode switch in ScanPage_E7,
 *      raw qrc copies in Sn3DCloudDisk) - are touched, and only their
 *      veil alpha is scaled to dim_alpha.  Modal dialogs paired with
 *      `color: "#000000"; opacity: X` keep their frosted glass (user
 *      requirement: the Tips/leave dialog must stay pretty).  Editing
 *      radius/samples on a live GaussianBlur is FORBIDDEN: the shader
 *      degenerates into a solid grey slab (the 1.2.5 regression).
 *      SVG <feGaussianBlur> and bodymovin JS never match the 14-byte
 *      block key; `//`-commented copies are skipped (a guard now shared
 *      with the RecursiveBlur walker).
 *      1.2.7 targets the SOLID grey curtain at its single shared source:
 *      the RecursiveBlur blocks that blur `source: backgroundImg` - the
 *      window wallpaper image (`ApplicationWindow.background: Image
 *      { source: "qrc:/images/MainBackground.jpg" }`, a static grey
 *      vignette, mean RGB ~80,80,87, embedded in AppUi.dll).  That is
 *      the busy-overlay family ("Project loading....", "Processing....",
 *      "Exiting..." = one ProgressBar_E7 component; firmware-update
 *      overlay).  Such blocks carry no `opacity:`, while every dialog
 *      backdrop blurs the `$backgroundImg` alias AND carries `opacity:`,
 *      so the unqualified source reference with a missing opacity
 *      identifies a curtain exactly.  For those blocks the module re-
 *      points the blur to `$mainbody` (the live page container alias,
 *      the very source the kept frosted dialogs use) - same 13 bytes,
 *      length-preserving - turning the grey slab into frosted glass over
 *      the real content.  Radius/loops are still softened; dialog blocks
 *      (opacity present / `$`-qualified source) are left untouched.
 *      The application's own zlib1.dll does the (de)compression.
 *
 *   The QML disk cache (qmlcache) replays pre-compiled sources and would
 *   hide every byte patch, so the module exports QML_DISABLE_DISK_CACHE=1
 *   into the process environment before Qt initializes. Side effect:
 *   slightly slower QML startup.
 *
 * Guards:
 *   - only read-only image sections are scanned (never writable data);
 *   - every dereference is SEH-guarded; a fault simply stops that pass;
 *   - patterns are anchored by their surrounding QML text, so unrelated
 *     `radius:`/`opacity:` properties elsewhere in the DLLs cannot match.
 *
 * Configuration (modules\popup_dim.ini):
 *   dim_alpha = 0       curtain opacity for modal overlays, percent
 *                       (0..100; QML granularity is 10 percent steps)
 *   soft_blur = 1       also reduce blur radius/loops (0/1, def 1)
 *   modules   = ...     DLLs to scan (def the three vendor UI DLLs)
 *   verbose   = 0|1     log patch details to mod.log (def 1)
 * Enable/disable: [popup_dim] enabled = 0|1 in modules.ini.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "exstar_sdk.h"

#define MAX_TARGETS   8
#define MAX_PATTERNS 16
#define MOD_VERSION   "1.2.7"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "popup_dim",
    "Popup Dim",
    MOD_VERSION,
    "lighter overlay behind popups instead of the dense grey curtain"
};

static const ExHost *g_host;

static char  g_targets[MAX_TARGETS][64];
static int   g_nTargets;
static int   g_pct          = 0;      /* new curtain opacity, percent   */
static char  g_aHi, g_aLo;            /* alpha as two hex chars (v1)    */
static char  g_opStr[8];              /* "0.0".."1.0" for dim_alpha     */
static int   g_softBlur     = 1;
static int   g_verbose      = 1;

static const char g_hexd[] = "0123456789abcdef";

static const char kAnchor[] = "Overlay.modal";
static const char kColor[]  = "color:";
#define ANCHOR_LEN (sizeof kAnchor - 1)
#define COLOR_LEN  (sizeof kColor - 1)

/* ------------------------------------------------------------------ */
/* literal patch table                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *modsub;              /* DLL name substring              */
    const char *pat;                 /* exact bytes to find             */
    char        rep[192];            /* replacement, built at runtime   */
    int         len;                 /* byte length of both             */
    int         blur;                /* only when soft_blur = 1         */
    int         done;                /* patched at least once           */
} LitPatch;

static LitPatch g_lit[MAX_PATTERNS];
static int      g_nLit;

/* `transparentBorder: true\r\n            opacity: 1.0`
 *  -> same with the dim_alpha opacity; Sn3DDialog modal + modeless */
static const char kTbPat[] =
    "transparentBorder: true\r\n            opacity: 1.0";
/* `source: $mainbody\r\n ... GaussianBlur radius/samples/deviation */
static const char kGbPat[] =
    "source: $mainbody\r\n            radius: 16\r\n"
    "            samples: 16\r\n            deviation: 4";
static const char kGbRep[] =
    "source: $mainbody\r\n            radius: 1 \r\n"
    "            samples: 1 \r\n            deviation: 0";
/* cloud modal black veil `color: "#000000"\r\n            opacity: 0.5` */
static const char kVeilHead[] = "color: \"#000000\"\r\n            opacity: ";
/* Qt Quick Controls 2 default (Basic) style: every Popup/Dialog/Drawer/
 * Frame/menu template without a custom overlay dims the whole window with
 * `Color.transparent(control.palette.shadow, 0.5)` - 50% black behind ANY
 * modal. Lives in qtquickcontrols2plugin.dll's embedded qml, outside the
 * vendor UI DLLs, so earlier passes never saw it. */
static const char kStylePat[] = "palette.shadow, 0.5)";
#define STYLE_TARGET "qtquickcontrols2plugin.dll"

static void addLit(const char *modsub, const char *pat, const char *rep,
                   int blur)
{
    LitPatch *e;
    size_t plen = strlen(pat), rlen;

    if (g_nLit >= MAX_PATTERNS)
        return;
    rlen = rep ? strlen(rep) : plen;
    /* rep needs one spare byte for the NUL at rep[rlen] */
    if (plen == 0 || rlen != plen || plen >= (int)sizeof e->rep)
        return;
    e = &g_lit[g_nLit++];
    e->modsub = modsub;
    e->pat    = pat;
    if (rep)
        memcpy(e->rep, rep, rlen);
    e->rep[rlen] = 0;
    e->len   = (int)plen;
    e->blur  = blur;
    e->done  = 0;
}

static void buildTable(void)
{
    static char tbRep[128];
    static char stRep[64];
    static char veilPat[64], veilRep[64];
    size_t n;

    /* Sn3DDialog (CompositeComponents.dll) ---------------------------- */
    addLit("compositecomponents", "radius: 7.5", "radius: 0.1", 1);
    addLit("compositecomponents", "loops: 10",   "loops: 1 ",   1);
    addLit("compositecomponents", "loops1: 50",  "loops1: 1 ",  1);
    addLit("compositecomponents", "opacity1: 0.6", "opacity1: 0.0", 1);

    /* BluredCoverEx.qml keeps an OPAQUE black base rectangle and a 40%
     * veil while any modal dialog is up (visible: isVague). Even with
     * the blur neutralized that is a full-screen grey cover - switch
     * all three layers (base, blur, veil) off. 16-byte literal ->
     * "visible: false" padded to the same length. */
    addLit("compositecomponents", "visible: isVague", "visible: false  ", 0);

    /* modal blur overlay: keep the anchor, swap the opacity value */
    n = strlen(kTbPat);
    memcpy(tbRep, kTbPat, n);
    tbRep[n - 3] = g_opStr[0];
    tbRep[n - 2] = g_opStr[1];
    tbRep[n - 1] = g_opStr[2];
    addLit("compositecomponents", kTbPat, tbRep, 0);

    /* cloud login modal (Sn3DCloudDisk.dll) --------------------------- */
    addLit("sn3dclouddisk", kGbPat, kGbRep, 1);

    n = sizeof kVeilHead - 1;            /* ends right after "opacity: " */
    memcpy(veilPat, kVeilHead, n);
    veilPat[n]     = '0';
    veilPat[n + 1] = '.';
    veilPat[n + 2] = '5';
    veilPat[n + 3] = 0;
    memcpy(veilRep, veilPat, n + 4);
    veilRep[n]     = g_opStr[0];
    veilRep[n + 1] = g_opStr[1];
    veilRep[n + 2] = g_opStr[2];
    addLit("sn3dclouddisk", veilPat, veilRep, 0);

    /* Qt Quick Controls 2 Basic style (qtquickcontrols2plugin.dll) ------
     * swap the "0.5" of "palette.shadow, 0.5)" for the dim_alpha value;
     * 5 hits: Popup, Dialog, Drawer, Frame and the menu template */
    n = sizeof kStylePat - 1;
    memcpy(stRep, kStylePat, n);
    stRep[n - 4] = g_opStr[0];
    stRep[n - 3] = g_opStr[1];
    stRep[n - 2] = g_opStr[2];
    addLit("qtquickcontrols2plugin", kStylePat, stRep, 0);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int hexVal(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* find needle in [p, p+len); returns offset or -1 (or -2 when
 * unreadable and the caller should stop scanning). SEH-guarded. */
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
        out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* literal patch application                                           */
/* ------------------------------------------------------------------ */

/* scan [base, base+size) for one pattern and replace in place.
 * Returns number of patches made (0: pattern simply absent). */
static int applyLit(unsigned char *base, size_t size, LitPatch *e)
{
    size_t off = 0;
    int hits = 0;

    if (!e->pat || !e->len)
        return 0;
    while (off + (size_t)e->len <= size) {
        long o = findBytes(base + off, size - off,
                           (const unsigned char *)e->pat, (size_t)e->len);
        if (o < 0)
            break;                       /* absent / unreadable */
        {
            DWORD oldp;
            unsigned char *q = base + off + (size_t)o;
            __try {
                if (VirtualProtect(q, (SIZE_T)e->len, PAGE_READWRITE, &oldp)) {
                    memcpy(q, e->rep, (size_t)e->len);
                    VirtualProtect(q, (SIZE_T)e->len, oldp, &oldp);
                    hits++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        }
        off += (size_t)o + (size_t)e->len;
    }
    return hits;
}

/* walk read-only sections of a mapped image, apply all literals whose
 * modsub matches `who` (already lowercase). Returns total patches. */
static int applyLits(HMODULE mod, const char *whoLower)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int i, k, total = 0;

    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96)
            return 0;
        sec = IMAGE_FIRST_SECTION(nt);

        for (k = 0; k < g_nLit; k++) {
            LitPatch *e = &g_lit[k];
            int hits = 0;

            if (!e->modsub || !strstr(whoLower, e->modsub))
                continue;
            if (e->blur && !g_softBlur)
                continue;
            if (e->done)
                continue;
            sec = IMAGE_FIRST_SECTION(nt);
            for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
                unsigned char *p;
                size_t len;
                int n;

                if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                    || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                    continue;
                p   = base + sec->VirtualAddress;
                len = sec->Misc.VirtualSize;
                if (len < (size_t)e->len)
                    continue;
                n = applyLit(p, len, e);
                hits  += n;
                total += n;
            }
            if (hits)
                e->done = 1;             /* idempotent: pattern is gone  */
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* image vanished mid-scan: keep what was patched */
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* v1 pass: black ARGB Overlay.modal colors (AppUi.dll)                */
/* ------------------------------------------------------------------ */

static int patchColorAt(unsigned char *base, long off, const char *who)
{
    unsigned char *c = base + off;
    int h[8];
    int i;
    DWORD old;

    __try {
        if (c[0] != '#')
            return -1;
        for (i = 0; i < 8; i++) {
            h[i] = hexVal(c[1 + i]);
            if (h[i] < 0)
                return -1;
        }
        /* pure black RGB only: the stock curtains */
        if (h[2] | h[3] | h[4] | h[5] | h[6] | h[7])
            return -1;
        if (c[1] == (unsigned char)g_aHi && c[2] == (unsigned char)g_aLo)
            return 0;                    /* already at target */

        if (!VirtualProtect(c + 1, 2, PAGE_READWRITE, &old))
            return -1;
        c[1] = (unsigned char)g_aHi;
        c[2] = (unsigned char)g_aLo;
        VirtualProtect(c + 1, 2, old, &old);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    if (g_verbose)
        g_host->log("popup_dim: %s Overlay.modal curtain alpha -> %d%%",
                    who, g_pct);
    return 1;
}

static int patchArgb(HMODULE mod, const char *who)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int i, patched = 0;

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
            size_t len, done;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            if (len < sizeof kAnchor)
                continue;

            done = 0;
            while (done + ANCHOR_LEN <= len) {
                unsigned char *a;
                size_t win, avail;
                long o, co;

                o = findBytes(p + done, len - done,
                              (const unsigned char *)kAnchor, ANCHOR_LEN);
                if (o < 0)
                    break;               /* none left / unreadable */
                a = p + done + o + ANCHOR_LEN;
                avail = len - (size_t)(a - p);
                win = avail < 400 ? avail : 400;
                co = findBytes(a, win, (const unsigned char *)kColor,
                               COLOR_LEN);
                if (co >= 0) {
                    unsigned char *q = a + co + COLOR_LEN;
                    unsigned char *end = a + win;
                    long k;

                    while (q < end && (*q == ' ' || *q == '\t'))
                        q++;
                    if (q < end && (*q == '"' || *q == '\''))
                        q++;
                    k = q - p;
                    if (patchColorAt(p, k, who) > 0)
                        patched++;
                }
                done += (size_t)o + ANCHOR_LEN;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return patched;
}

/* ------------------------------------------------------------------ */
/* zlib qrc pass                                                       */
/*                                                                     */
/* Some UI DLLs (AppUi.dll, ScanPage_E7.dll, pluginSn3DNavigation.dll) */
/* keep their QML compressed inside the qrc blob instead of plain      */
/* text: every entry is [u32 BE rawSize][zlib stream]. The grey        */
/* curtain blocks (RecursiveBlur over $backgroundImg - messages,       */
/* loading and dialog overlays) live in those compressed streams, so   */
/* the literal table above can never see them. This pass inflates      */
/* each stream from the MAPPED image, dims every curtain block with    */
/* equal-length edits (radius -> 0.1, loops -> 1, opacity -> 0), then  */
/* re-deflates and writes the result back into the process-private     */
/* copy of the image, never exceeding the original compressed size.    */
/* Files on disk are not touched; a restart restores stock behaviour.  */
/* The application's own zlib1.dll performs the actual (de)compression.*/
/* ------------------------------------------------------------------ */

/* z_stream ABI mirror - layout stable since zlib 1.2.x (win-x64) */
typedef struct _PD_ZS {
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
} PD_ZS;

typedef int (__cdecl *f_zinit)(PD_ZS *, int, const char *, int);
typedef int (__cdecl *f_zdinit)(PD_ZS *, int, int, int, int, int,
                                const char *, int);
typedef int (__cdecl *f_zrun)(PD_ZS *, int);
typedef int (__cdecl *f_zend)(PD_ZS *);

static f_zinit p_inflateInit2_;
static f_zrun  p_inflate;
static f_zend  p_inflateEnd;
static f_zdinit p_deflateInit2_;
static f_zrun  p_deflate;
static f_zend  p_deflateEnd;
static int     g_zlib;                    /* zlib entry points ready */
#define PD_ZLIB_VER "1.2.11"              /* prefix-checked by zlib  */

static void loadZlib(void)
{
    HMODULE z = GetModuleHandleA("zlib1.dll");
    if (!z)
        z = LoadLibraryA("zlib1.dll");
    if (!z)
        z = LoadLibraryA("zlib.dll");
    if (!z)
        return;
    p_inflateInit2_  = (f_zinit) (void *) GetProcAddress(z, "inflateInit2_");
    p_inflate        = (f_zrun)  (void *) GetProcAddress(z, "inflate");
    p_inflateEnd     = (f_zend)  (void *) GetProcAddress(z, "inflateEnd");
    p_deflateInit2_  = (f_zdinit)(void *) GetProcAddress(z, "deflateInit2_");
    p_deflate        = (f_zrun)  (void *) GetProcAddress(z, "deflate");
    p_deflateEnd     = (f_zend)  (void *) GetProcAddress(z, "deflateEnd");
    g_zlib = p_inflateInit2_ && p_inflate && p_inflateEnd
          && p_deflateInit2_ && p_deflate && p_deflateEnd;
}
/* dim one numeric property inside [b, b+len): kind 0 = opacity -> 0.0,
 * 1 = radius -> 0.1, 2 = loops -> 1. All edits keep the byte length. */
static int dimProp(unsigned char *b, size_t len, const char *name, int kind)
{
    size_t nl = strlen(name);
    long   o;
    unsigned char *v, *q, *end = b + len;

    o = findBytes(b, len, (const unsigned char *)name, nl);
    if (o < 0)
        return 0;
    v = b + o + (long)nl;
    while (v < end && *v == ' ')
        v++;
    if (v >= end || *v < '0' || *v > '9')
        return 0;
    q = v;
    while (q < end && ((*q >= '0' && *q <= '9') || *q == '.'))
        q++;
    if (kind == 0) {                      /* opacity -> g_opStr ("0.0".."1.0") */
        size_t vl = (size_t)(q - v), ol = strlen(g_opStr);
        unsigned char *r;

        if (ol <= vl) {                   /* fits: overwrite, blank pad  */
            for (r = v; r < q; r++)
                *r = ' ';
            memcpy(v, g_opStr, ol);
        } else if (g_pct == 0) {          /* "opacity: 1" -> "opacity: 0" */
            for (r = v; r < q; r++)
                if (*r >= '0' && *r <= '9')
                    *r = '0';
        } else
            return 0;                     /* cannot express, leave it    */
    } else if (kind == 1) {               /* "7.5" -> "0.1" */
        unsigned char *r;
        int afterDot = 0, frac = 0;
        for (r = v; r < q; r++) {
            if (*r == '.') { afterDot = 1; continue; }
            if (r == v)            *r = '0';
            else if (afterDot && !frac) { *r = '1'; frac = 1; }
            else                   *r = '0';
        }
    } else {                              /* loops -> 1 + blanks */
        unsigned char *r;
        for (r = v; r < q; r++)
            *r = (r == v) ? '1' : ' ';
    }
    return 1;
}

/* 1 when the line holding offset b carries a `//` comment marker in
 * front of it - the vendor ships whole curtain copies behind `///`,
 * dimming those would only burn rewrite budget */
static int lineCommented(const unsigned char *t, size_t b)
{
    size_t s = b;

    while (s > 0 && t[s - 1] != '\n' && b - s < 160)
        s--;
    return findBytes(t + s, b - s, (const unsigned char *)"//", 2) >= 0;
}

/* 1 when c points at the opening quote of a pure-black-with-alpha
 * literal `"#aa000000"`; *dig returns the two alpha digits. */
static int isAlphaBlack(const unsigned char *c, size_t room,
                        unsigned char **dig)
{
    if (room < 11 || c[0] != '"' || c[1] != '#' || c[10] != '"')
        return 0;
    if (hexVal(c[2]) < 0 || hexVal(c[3]) < 0)
        return 0;
    if (memcmp(c + 4, "000000", 6) != 0)
        return 0;                            /* not black */
    if (dig)
        *dig = (unsigned char *)c + 2;
    return 1;
}

/* 1 when c points at the opening quote of a plain `"#000000"` */
static int isPlainBlack(const unsigned char *c, size_t room)
{
    int k;

    if (room < 9 || c[0] != '"' || c[1] != '#' || c[8] != '"')
        return 0;
    for (k = 2; k < 8; k++)
        if (c[k] != '0')
            return 0;
    return 1;
}

/* The vendor ships two visually different GaussianBlur overlays and
 * they must be treated differently (user requirement 2026-11-14):
 *
 *   G_V_CURTAIN - busy/loader curtains ("Project loading...",
 *                 "Exiting...", mode switch): the blur is paired with an
 *                 opaque `Rectangle { color: "#aa000000" }` black veil
 *                 (alpha inside the color literal) in front of or after
 *                 the block.  Only this kind gets dimmed, and ONLY the
 *                 veil alpha is rewritten - setting `radius: 0`/
 *                 `samples: 1` on a live GaussianBlur makes its shader
 *                 render a solid grey slab instead of the source, which
 *                 is what produced the "more aggressive grey" in 1.2.5.
 *
 *   G_V_DIALOG  - modal dialogs (Tips/leave-confirm/save): the blur is
 *                 paired with `Rectangle { color: "#000000";
 *                 opacity: X }`, or the blur sits inside a popup's own
 *                 `background: Rectangle { color: "#99000000" ... }`.
 *                 That frosted glass is wanted - leave every byte as-is.
 *
 *   G_V_NONE    - pure blur, no darkening companion: keep.
 */
#define G_V_NONE    0
#define G_V_DIALOG  1
#define G_V_CURTAIN 2

static int gaussVeilKind(unsigned char *t, size_t e, size_t n, size_t b,
                         unsigned char **lit)
{
    static const unsigned char kRect[]   = "Rectangle {";
    static const unsigned char kColor[]  = "color";
    static const unsigned char kBgRect[] = "background: Rectangle";
    size_t ve = e + 260 < n ? e + 260 : n;
    size_t bs0, bs;
    long   ro, co, qo;
    unsigned char *c;
    size_t cl;

    if (lit)
        *lit = NULL;
    ro = findBytes(t + e, ve - e, kRect, sizeof kRect - 1);
    if (ro >= 0) {
        co = findBytes(t + e + ro, ve - (e + (size_t)ro),
                       kColor, sizeof kColor - 1);
        if (co >= 0) {
            c  = t + e + (size_t)ro + (size_t)co;
            cl = ve - (size_t)(c - t);
            qo = findBytes(c, cl, (const unsigned char *)"\"", 1);
            if (qo >= 0) {
                c  += qo;
                cl  = ve - (size_t)(c - t);
                if (isAlphaBlack(c, cl, lit))
                    return G_V_CURTAIN;      /* opaque loading veil */
                if (isPlainBlack(c, cl))
                    return G_V_DIALOG;       /* frosted dialog glass */
            }
        }
    }
    /* backward: `background: Rectangle { color: "#99000000" GaussianBlur
     * { ... } }` - dark glass belongs to the popup itself: dialog. */
    bs0 = b > 220 ? b - 220 : 0;
    for (bs = bs0; bs + 11 <= b; bs++) {
        unsigned char *d;
        if (t[bs] != '"' || !isAlphaBlack(t + bs, b - bs, &d))
            continue;
        if (findBytes(t + bs0, b - bs0,
                      kBgRect, sizeof kBgRect - 1) >= 0)
            return G_V_DIALOG;
        if (lit)
            *lit = t + bs;
        return G_V_CURTAIN;
    }
    return G_V_NONE;
}

/* Walk every `GaussianBlur { ... }` QML block.  Blocks are curtains
 * only when their companion veil is the opaque "#aa000000" color (see
 * gaussVeilKind); for those the veil alpha is scaled to dim_alpha and
 * NOTHING else is edited - the blur itself must keep its radius/samples
 * or Qt renders a solid grey slab.  SVG <feGaussianBlur> and bodymovin
 * JS never match the 14-byte key, the always-on wallpaper blur carries
 * no `radius:`, and blocks behind `//` comments are skipped. */
static int dimGaussBlocks(unsigned char *t, size_t n)
{
    static const unsigned char kGb[]  = "GaussianBlur {";
    static const unsigned char kDev[] = "deviation:";
    static const unsigned char kRad[] = "radius:";
    size_t i = 0;
    int    dimmed = 0;

    while (i + (sizeof kGb - 1) < n) {
        long o = findBytes(t + i, n - i, kGb, sizeof kGb - 1);
        size_t b, e;
        int depth = 1;

        if (o < 0)
            break;
        b = i + (size_t)o;
        for (e = b + (sizeof kGb - 1); e < n && e - b < 4096; e++) {
            if (t[e] == '{')
                depth++;
            else if (t[e] == '}' && --depth == 0) {
                e++;                         /* one past the block */
                break;
            }
        }
        if (depth != 0) {                    /* unbalanced window */
            i = b + (sizeof kGb - 1);
            continue;
        }
        i = e;
        if (findBytes(t + b, e - b, kDev, sizeof kDev - 1) < 0 ||
            findBytes(t + b, e - b, kRad, sizeof kRad - 1) < 0 ||
            lineCommented(t, b))
            continue;
        {
            unsigned char *lit = NULL;
            int hi, lo, v;

            if (gaussVeilKind(t, e, n, b, &lit) != G_V_CURTAIN || !lit)
                continue;                    /* dialog glass: keep    */
            hi = hexVal(lit[0]);
            lo = hexVal(lit[1]);
            v  = (hi * 16 + lo) * g_pct / 100;   /* veil -> dim_alpha  */
            lit[0] = (unsigned char)g_hexd[(v >> 4) & 15];
            lit[1] = (unsigned char)g_hexd[v & 15];
            dimmed++;
        }
    }
    return dimmed;
}

/* Raw (uncompressed) QML copies: Sn3DCloudDisk keeps its GaussianBlur
 * curtains plain in .rdata (the qrc blob is stored without zlib), so the
 * zrc walker never sees them - run the same gate and edits directly on
 * the read-only sections.  All writes are length-preserving and happen
 * under VirtualProtect; the 14-byte key + radius/deviation block gate
 * keeps unrelated text (SVG, JS, code) untouched. */
static int dimRawGauss(HMODULE mod, const char *who)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int k, dimmed = 0;

    (void)who;
    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96)
            return 0;

        for (k = 0, sec = IMAGE_FIRST_SECTION(nt);
             k < nt->FileHeader.NumberOfSections; k++, sec++) {
            unsigned char *p;
            size_t len;
            DWORD oldp;
            int n;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;
            if (len < 32)
                continue;
            if (findBytes(p, len,
                          (const unsigned char *)"GaussianBlur {", 14) < 0)
                continue;                  /* no curtain text here */
            if (!VirtualProtect(p, len, PAGE_READWRITE, &oldp))
                continue;
            n = dimGaussBlocks(p, len);
            VirtualProtect(p, len, oldp, &oldp);
            dimmed += n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        /* unreadable page: keep what got dimmed so far */
    }
    return dimmed;
}

 /* Redirect one curtain block's blur source from the static wallpaper
  * image to the live page container: `source: backgroundImg` ->
  * `source: $mainbody   ` (13 -> 13 bytes, QML ignores the trailing
  * blanks).  The `$mainbody` context property is the window's
  * `mainbody` alias (windowPageContainer) and is already consumed that
  * way by the kept GaussianBlur dialog frosts.  Only the exact
  * unqualified reference matches; `$backgroundImg` dialog backdrops are
  * never touched.  Returns 1 when the block was redirected. */
static int swapSourceToMainbody(unsigned char *b, size_t len)
{
    unsigned char *v, *r;
    long o = findBytes(b, len, (const unsigned char *)"source:", 7);

    if (o < 0)
        return 0;
    v = b + o + 7;
    while (v < b + len && (*v == ' ' || *v == '\t'))
        v++;
    if ((size_t)(v - b) + 13 > len ||
        memcmp(v, "backgroundImg", 13) != 0 ||
        (v != b && v[-1] == '$'))
        return 0;
    for (r = v; r < v + 13; r++)
        *r = ' ';
    memcpy(v, "$mainbody   ", 12);
    return 1;
}

/* Walk every `RecursiveBlur { ... }` block; if the block references
 * backgroundImg (curtain over a window screenshot) dim it.  Blocks that
 * blur the plain (unqualified) `backgroundImg` and carry no `opacity:`
 * are the solid busy-overlay curtains: their source is redirected to
 * the live page instead (1.2.7), everything else only gets dimmed.
 * Returns the number of dimmed/redirected blocks.
 * Length of the text never changes. */
static int dimBlurBlocks(unsigned char *t, size_t n)
{
    static const unsigned char kRb[] = "RecursiveBlur";
    static const unsigned char kBg[] = "backgroundImg";
    static const unsigned char kOp[] = "opacity:";
    size_t i = 0;
    int    dimmed = 0;

    while (i < n) {
        long o = findBytes(t + i, n - i, kRb, sizeof kRb - 1);
        size_t b, e;
        int depth = 0;

        if (o < 0)
            break;
        b = i + (size_t)o;
        for (e = b + (sizeof kRb - 1); e < n && t[e] != '{'; e++)
            if (e - b > 64)
                break;
        if (e >= n || e - b > 64) { i = b + (sizeof kRb - 1); continue; }
        for (; e < n && e - b < 4096; e++) {
            if (t[e] == '{')
                depth++;
            else if (t[e] == '}')
                if (--depth == 0)
                    break;
        }
        if (e >= n || e - b >= 4096) { i = b + (sizeof kRb - 1); continue; }

        if (findBytes(t + b, e - b, kBg, sizeof kBg - 1) >= 0 &&
            !lineCommented(t, b)) {
            if (findBytes(t + b, e - b, kOp, sizeof kOp - 1) < 0) {
                if (g_verbose)
                    printf("  [dbg] curtain@%zu noopacity swap=%d\n",
                           b, swapSourceToMainbody(t + b, e - b));
            }
            dimProp(t + b, e - b, "radius:",  1);
            dimProp(t + b, e - b, "loops:",   2);
            dimProp(t + b, e - b, "opacity:", 0);
            dimmed++;
        }
        i = e;
    }
    return dimmed;
}

/* re-inflate one patched record straight from the (mapped, write
 * restored) image; 1 = the stream still decodes to exactly raw bytes.
 * avail_in is the original record size, which the new (shorter or equal)
 * stream fits into, so the read can never leave the record. */
static int checkRecord(const unsigned char *q, unsigned raw,
                       unsigned csize)
{
    unsigned char *buf;
    PD_ZS s;
    int r = -1;

    buf = (unsigned char *)malloc(raw + 16);
    if (!buf)
        return 1;                    /* cannot verify: keep the write */
    memset(&s, 0, sizeof s);
    s.next_in   = (unsigned char *)(q + 4);
    s.avail_in  = csize;
    s.next_out  = buf;
    s.avail_out = raw + 16;
    __try {
        if (p_inflateInit2_(&s, 15, PD_ZLIB_VER, (int)sizeof s) == 0) {
            r = p_inflate(&s, 4);    /* Z_FINISH */
            p_inflateEnd(&s);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    free(buf);
    return (r == 1 && s.total_out == raw) ? 1 : 0;
}

/* inflate -> dim -> re-deflate every compressed QML stream of a module.
 * Returns number of rewritten streams. */
static int applyZrc(HMODULE mod, const char *who)
{
    unsigned char *base = (unsigned char *)mod;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_SECTION_HEADER sec;
    int k, streams = 0, blocks = 0, cand = 0, okInf = 0, faulted = 0;
    int verified = 0, restored = 0;
    static const unsigned char kBg[]  = "backgroundImg";
    static const unsigned char kGb[]  = "GaussianBlur {";

    if (!g_zlib)
        return 0;
    __try {
        dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->FileHeader.NumberOfSections > 96)
            return 0;

        for (k = 0, sec = IMAGE_FIRST_SECTION(nt);
             k < nt->FileHeader.NumberOfSections; k++, sec++) {
            unsigned char *p;
            size_t len, i;

            if (!(sec->Characteristics & IMAGE_SCN_MEM_READ)
                || (sec->Characteristics & IMAGE_SCN_MEM_WRITE))
                continue;
            p   = base + sec->VirtualAddress;
            len = sec->Misc.VirtualSize;

            for (i = 0; i + 8 <= len; i++) {
                unsigned cmf = p[i + 4], flg = p[i + 5];
                unsigned raw;
                unsigned char *buf;
                PD_ZS s;
                int r;
                size_t lc;

                if (cmf != 0x78 || (((cmf << 8) | flg) % 31) != 0)
                    continue;
                raw = ((unsigned)p[i] << 24) | ((unsigned)p[i + 1] << 16)
                    | ((unsigned)p[i + 2] << 8) | (unsigned)p[i + 3];
                if (raw < 64 || raw > 0x400000)
                    continue;
                cand++;

                buf = (unsigned char *)malloc(raw + 16);
                if (!buf)
                    return streams;
                memset(&s, 0, sizeof s);
                s.next_in  = p + i + 4;
                s.avail_in = (unsigned)(len - i - 4);
                s.next_out = buf;
                s.avail_out = raw + 16;
                if (p_inflateInit2_(&s, 15, PD_ZLIB_VER,
                                    (int)sizeof s) != 0) {
                    free(buf);
                    continue;
                }
                r  = p_inflate(&s, 4);           /* Z_FINISH */
                lc = (size_t)s.total_in;
                p_inflateEnd(&s);
                if (r != 1 || s.total_out != raw ||
                    (findBytes(buf, raw, kBg, sizeof kBg - 1) < 0 &&
                     findBytes(buf, raw, kGb, sizeof kGb - 1) < 0)) {
                    free(buf);
                    i += 3;
                    continue;
                }
                okInf++;

                {   /* rewrite if any curtain block got dimmed */
                    int ed = dimBlurBlocks(buf, raw)
                           + dimGaussBlocks(buf, raw);
                    if (ed > 0) {
                        unsigned char *out =
                            (unsigned char *)malloc(lc ? lc : 64);
                        unsigned long nc = 0;
                        int lvl;

                        /* try max compression first, fall back to the
                         * qrc default (6); must fit into the original
                         * compressed size, the stream boundary of the
                         * next record must not move */
                        for (lvl = 9; out && nc == 0 && lvl >= 6;
                             lvl -= 3) {
                            PD_ZS d;
                            memset(&d, 0, sizeof d);
                            d.next_in   = buf;
                            d.avail_in  = (unsigned)raw;
                            d.next_out  = out;
                            d.avail_out = (unsigned)lc;
                            if (p_deflateInit2_(&d, lvl, 8, 15, 8, 0,
                                                PD_ZLIB_VER,
                                                (int)sizeof d) == 0) {
                                if (p_deflate(&d, 4) == 1
                                    && d.total_out <= (unsigned long)lc)
                                    nc = d.total_out;
                                else if (lvl == 6 && g_verbose)
                                    g_host->log("popup_dim: zrc %s stream "
                                                "@0x%X recompress %lu>%lu, "
                                                "skipped", who,
                                                (unsigned)(sec->VirtualAddress
                                                           + i),
                                                (unsigned long)d.total_out,
                                                (unsigned long)lc);
                                p_deflateEnd(&d);
                            }
                        }
                        if (nc) {
                            /* header AND body go in under one
                             * VirtualProtect: the qrc pages are
                             * PAGE_READONLY in the mapped image, touching
                             * them first would fault and abort the whole
                             * pass (v1.2.1 bug) */
                            DWORD oldp;
                            unsigned char *q  = p + i;
                            unsigned char *bk =
                                (unsigned char *)malloc(lc);
                            int ok = 0;

                            if (bk && VirtualProtect(q, 4 + lc,
                                                     PAGE_READWRITE, &oldp)) {
                                memcpy(bk, q + 4, lc);   /* rollback copy */
                                q[0] = (unsigned char)(raw >> 24);
                                q[1] = (unsigned char)(raw >> 16);
                                q[2] = (unsigned char)(raw >> 8);
                                q[3] = (unsigned char)(raw);
                                memcpy(q + 4, out, nc);
                                memset(q + 4 + nc, 0, lc - nc);
                                VirtualProtect(q, 4 + lc, oldp, &oldp);
                                /* proof the patched record still decodes:
                                 * re-inflate it straight from memory */
                                ok = checkRecord(q, raw, (unsigned)lc);
                                if (!ok) {
                                    if (VirtualProtect(q, 4 + lc,
                                                       PAGE_READWRITE,
                                                       &oldp)) {
                                        memcpy(q + 4, bk, lc);
                                        VirtualProtect(q, 4 + lc,
                                                       oldp, &oldp);
                                    }
                                    restored++;
                                    if (g_verbose)
                                        g_host->log("popup_dim: zrc %s "
                                                    "stream @0x%X verify "
                                                    "FAILED, restored",
                                                    who,
                                                    (unsigned)(sec->VirtualAddress
                                                               + i));
                                }
                            }
                            free(bk);
                            if (ok) {
                                streams++;
                                blocks += ed;
                                verified++;
                            }
                        }
                        free(out);
                    }
                }
                free(buf);
                if (lc >= 4)
                    i += lc - 4;             /* step past this stream */
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = 1;                     /* unreadable page mid-scan */
    }
    if (g_verbose)
        g_host->log("popup_dim[%lu]: %s zrc zlib=%d cand=%d qml=%d rewritten=%d "
                    "blocks=%d verified=%d restored=%d fault=%d",
                    (unsigned long)GetCurrentProcessId(),
                    who, g_zlib, cand, okInf, streams, blocks,
                    verified, restored, faulted);
    return streams;
}

/* ------------------------------------------------------------------ */
/* apply everything to one module                                      */
/* ------------------------------------------------------------------ */

/* the load notification fires twice per mapping; the zrc pass does real
 * work (inflate/deflate), so run each module image exactly once */
static HMODULE g_seen[24];
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
    char low[128];
    int nLit, nArgb, nRaw, nZrc, i, mine = 0;

    lowerCopy(name, low, sizeof low);
    for (i = 0; i < g_nTargets; i++) {
        char tlow[72];
        lowerCopy(g_targets[i], tlow, sizeof tlow);
        if (strstr(low, tlow)) { mine = 1; break; }
    }
    if (!mine || seenMod(mod))
        return;                          /* not a target / second fire */
    nLit  = applyLits(mod, low);
    nArgb = patchArgb(mod, name);
    nRaw  = dimRawGauss(mod, name);
    nZrc  = applyZrc(mod, name);
    if (g_verbose && (nLit || nArgb || nRaw))
        g_host->log("popup_dim[%lu]: %s patched (%d literal, %d argb, "
                    "%d raw-gauss), overlay opacity %s",
                    (unsigned long)GetCurrentProcessId(),
                    name, nLit, nArgb, nRaw, g_opStr);
}

/* ------------------------------------------------------------------ */
/* DLL load notification: patch every target the moment it maps,       */
/* i.e. before its DllMain and long before the QML engine reads its    */
/* embedded sources. This is what makes the patch beat the QML engine: */
/* dialog QML is compiled within milliseconds of the DLL mapping.      */
/* ------------------------------------------------------------------ */

/* UNICODE_STRING is not visible from <windows.h> here - mirror it */
typedef struct _PD_USTR {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} PD_USTR;
typedef struct _PD_NOTIF {
    ULONG    Flags;
    PD_USTR *FullDllName;
    PD_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} PD_NOTIF;

static void *g_notifCookie;

static void WINAPI pd_notify(unsigned reason, const PD_NOTIF *d, void *ctx)
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
    return reg(0, (void *)pd_notify, NULL, &g_notifCookie) == 0;
}

/* ------------------------------------------------------------------ */
/* worker: catch already-mapped modules + polling fallback             */
/* ------------------------------------------------------------------ */

static int parseTargets(const char *list)
{
    char buf[512];
    char *tok, *save = NULL;

    g_nTargets = 0;
    strncpy(buf, list, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (tok = strtok_s(buf, ",;", &save);
         tok && g_nTargets < MAX_TARGETS;
         tok = strtok_s(NULL, ",;", &save)) {
        while (*tok == ' ')
            tok++;
        if (!*tok)
            continue;
        strncpy(g_targets[g_nTargets], tok, sizeof g_targets[0] - 1);
        g_targets[g_nTargets][sizeof g_targets[0] - 1] = 0;
        g_nTargets++;
    }
    return g_nTargets;
}

static unsigned WINAPI worker(LPVOID arg)
{
    int done[MAX_TARGETS];
    int notif;
    ULONGLONG t0 = GetTickCount64();
    int i, left;

    (void)arg;
    memset(done, 0, sizeof done);
    notif = armNotify();

    left = g_nTargets;
    while (left > 0) {
        for (i = 0; i < g_nTargets; i++) {
            HMODULE h;

            if (done[i])
                continue;
            h = GetModuleHandleA(g_targets[i]);   /* case-insensitive */
            if (!h)
                continue;
            patchModule(h, g_targets[i]);
            done[i] = 1;
            left--;
        }
        if (left == 0)
            break;
        if (GetTickCount64() - t0 > 300000) {
            g_host->log("popup_dim: %d module(s) never loaded", left);
            break;
        }
        /* with the load notification this loop is only a safety net:
         * back off quickly */
        Sleep(GetTickCount64() - t0 < 10000 ? 20 : 500);
    }
    if (notif)
        g_host->log("popup_dim: load-time patching armed "
                    "(dll notification active)");
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
    char list[512];
    unsigned tid;
    HANDLE th;
    int a, d;

    g_host = host;

    g_pct = host->cfg_int(host, "dim_alpha", 0);
    if (g_pct < 0)
        g_pct = 0;
    if (g_pct > 100)
        g_pct = 100;
    /* v1 ARGB alpha chars */
    a = (g_pct * 255 + 50) / 100;
    g_aHi = g_hexd[a >> 4];
    g_aLo = g_hexd[a & 15];
    /* QML opacity with 0.1 granularity: 0..95 -> 0.0..1.0 step .1,
     * 100 keeps full stock opacity */
    d = (g_pct + 5) / 10;
    if (d >= 10)
        strcpy_s(g_opStr, sizeof g_opStr, "1.0");
    else {
        g_opStr[0] = (char)('0' + d);
        strcpy_s(g_opStr + 1, sizeof g_opStr - 1, ".0");
    }
    g_softBlur = host->cfg_int(host, "soft_blur", 1);
    g_verbose  = host->cfg_int(host, "verbose", 1);

    if (!host->cfg_string(host, "modules", list, sizeof list))
        strcpy_s(list, sizeof list,
                 "AppUi.dll,CompositeComponents.dll,Sn3DCloudDisk.dll");
    if (parseTargets(list) == 0) {
        host->log("popup_dim: no target modules configured");
        return 1;
    }
    {   /* framework-level targets beyond the vendor ini list: the Qt
         * Controls 2 style plugin and the two compressed-QML UI DLLs
         * that host the real curtain (RecursiveBlur over $backgroundImg) */
        static const char *extra[] = {
            STYLE_TARGET, "ScanPage_E7.dll", "pluginSn3DNavigation.dll"
        };
        size_t x;
        int i, has;
        for (x = 0; x < sizeof extra / sizeof extra[0]; x++) {
            has = 0;
            for (i = 0; i < g_nTargets; i++)
                if (_stricmp(g_targets[i], extra[x]) == 0)
                    has = 1;
            if (!has && g_nTargets < MAX_TARGETS) {
                strcpy_s(g_targets[g_nTargets], sizeof g_targets[0], extra[x]);
                g_nTargets++;
            }
        }
    }
    loadZlib();
    if (g_verbose)
        g_host->log("popup_dim: zlib %s (zstream=%d bytes)",
                    g_zlib ? "loaded" : "NOT LOADED", (int)sizeof(PD_ZS));
    buildTable();

    /* the patched QML source must be recompiled, not replayed from
     * %LOCALAPPDATA%\...\qmlcache */
    SetEnvironmentVariableW(L"QML_DISABLE_DISK_CACHE", L"1");

    th = CreateThread(NULL, 0, worker, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);

    host->log("popup_dim v%s armed in pid=%lu: overlay opacity %s (%d%%), "
              "blur soft %s, %d target module(s), qmlcache disabled",
              MOD_VERSION, (unsigned long)GetCurrentProcessId(),
              g_opStr, g_pct,
              g_softBlur ? "on" : "off", g_nTargets);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* nothing to undo: the patch lives in the mapped DLL images and
     * dies with the process */
}




