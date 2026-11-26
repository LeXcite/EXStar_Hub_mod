/* merge_list_lab.c - prove merge_list.h before Selective Mesh leans on it.

 * selective_mesh 1.7.0 answers "is any scan left in this merge?" by reading
 * the container the vendor's own merge loop walks, out of the caller's
 * saved registers (docs\research\merge-common-mesh.md section 20).  It must
 * not call Qt from there - 1.5.0 did and the hub faulted in Qt5Core on
 * every start - so merge_list.h reads that QList as bytes, and this tool is
 * the proof that its layout is the layout of the shipped Qt:
 *
 *   1. a REAL QStringList is built with the app's own Qt5Core (through
 *      exported functions, exactly as the vendor's code builds one), and
 *      mlHeader() has to report its element count and point at its slots -
 *      checked by decoding the names with readQName5(), the reader that is
 *      already live-verified in the hub;
 *   2. the same reader then walks a container of FAKE scan items in the
 *      vendor's own shape ([item+0x28] = HandlePrjFile, QUuid at
 *      [item+0x38]) and finds the right slot for the right scan - with the
 *      block's rcx holding a CODE address, because that is what the hub
 *      showed live on 2026-11-26 and the module's acceptance rule is the
 *      list-content one, not "rcx is self";
 *   3. and it refuses everything that is not such a container: nulls,
 *      garbage, an empty list, an item that cannot be read, an item with no
 *      id - because the module only forces a merge open when the list is
 *      certain.
 *
 * Calling convention (measured, see qv_layout.c): this Qt5Core uses the
 * Microsoft ABI, and a member function returning a non-trivially-destructible
 * object takes `this` in RCX with the hidden return slot in RDX.
 *
 *   cl /nologo /O2 /MT /utf-8 /I..\modules\selective_mesh merge_list_lab.c
 *   merge_list_lab.exe "C:\Program Files\Shining3d\EXStar Hub\Qt5Core.dll"
 *
 * Exit code 0 = every check passed.  Development tool; nothing here runs in
 * the product.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "qt_read.h"          /* plausiblePtr + readQName5 */
#include "merge_list.h"       /* the code under test, verbatim */

typedef void *(*FnCtorS)(void *self, const char *lit);
typedef void *(*FnSplit)(const void *self, void *retOut, unsigned short sep,
                         int behavior, int cs);

static HMODULE g_qt;
static int g_failed;

static void *imp(const char *name)
{
    void *p = (void *)GetProcAddress(g_qt, name);

    if (!p)
        printf("  ! missing export %s\n", name);
    return p;
}

static void expect(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        g_failed++;
}

/* the register block buildThunk leaves on the stack, read low address to
 * high: the first pushed register (rax) is at the lowest address.  Same
 * layout as the module's RegCtx; the live dumps of 2026-11-26 confirm it
 * (the merge's list arrives in r9, `self` in none of these) */
typedef struct {
    ULONG64 rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, rflags;
} RegCtx;

/* one fake vendor scan item: only the two fields the merge loop touches */
static unsigned char itemBuf[4][0x80];

static void mkItem(int i, void *self, unsigned char first)
{
    int k;

    memset(itemBuf[i], 0, sizeof itemBuf[i]);
    *(void **)(itemBuf[i] + ML_ITEM_SELF) = self;
    for (k = 0; k < 16; k++)
        itemBuf[i][ML_ITEM_ID + k] = (unsigned char)(first + k);
}

/* a QList header (ref, alloc, begin, end) followed by `slots` 8-byte slots;
 * the used elements are slots [begin, end), exactly as Qt lays them out */
static unsigned char hdrA[ML_HDR + 4 * 8];
static unsigned char hdrB[ML_HDR + 4 * 8];

static void mkHdr(unsigned char *h, int alloc, int begin, int end, int slots)
{
    memset(h, 0, (size_t)ML_HDR + (size_t)slots * 8);
    *(int *)(h + ML_REF)  = 1;
    *(int *)(h + ML_ALLOC) = alloc;
    *(int *)(h + ML_BEGIN) = begin;
    *(int *)(h + ML_END)   = end;
}

int main(int argc, char **argv)
{
    const char *qtPath = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\Qt5Core.dll";
    static const char *want[4]
        = { "Project_1", "Project_2", "Project_3", "Project_4" };
    FnCtorS  strLit;
    FnSplit  strSplit;
    unsigned char src[64], sl[64];
    unsigned char *r;
    const void *const *arr = NULL;
    int alloc = -1, n, i, hits, okAll;
    unsigned char prjFile[16], uuid[16];
    RegCtx ctx;
    unsigned char args[ML_LIST_OFF + 8];
    const void *list = NULL;
    int accepted = 0;

    g_qt = LoadLibraryExA(qtPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_qt) {
        printf("cannot load %s (err %u)\n", qtPath, (unsigned)GetLastError());
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Qt5Core at %p\n\n", (void *)g_qt);

    strLit   = (FnCtorS)imp("??0QString@@QEAA@PEBD@Z");
    strSplit = (FnSplit)imp("?split@QString@@QEBA?AVQStringList@@VQChar@@"
                            "W4SplitBehavior@1@W4CaseSensitivity@Qt@@@Z");
    if (!strLit || !strSplit)
        return 2;

    /* ---- 1. the reader against a real Qt container -------------------- */
    memset(src, 0, sizeof src);
    strLit(src, "Project_1,Project_2,Project_3,Project_4");
    memset(sl, 0, sizeof sl);
    r = (unsigned char *)strSplit(src, sl, ',', 0, 0);
    printf("QString::split(',') -> QStringList at %p, header %p\n",
           (void *)r, *(void **)sl);
    if (*(void **)sl) {
        const unsigned char *d = *(const unsigned char *const *)sl;

        printf("      header: ref=%d alloc=%d size=%d offset=%lld\n\n",
               *(const int *)(d + 0), *(const int *)(d + 4),
               *(const int *)(d + 8),
               (long long)*(const long long *)(d + 16));
    }

    n = mlHeader(sl, &arr, &alloc);
    printf("mlHeader() -> %d entries, alloc %d, slots at %p\n", n, alloc,
           (void *)arr);
    expect("a real QStringList decodes with 4 entries", n == 4);

    hits = 0;
    for (i = 0; i < n && i < 4; i++) {
        char nm[128];
        int keep;

        nm[0] = 0;
        keep = readQName5(&arr[i], nm, (int)sizeof nm);
        printf("      [%d] \"%s\"\n", i, nm);
        if (keep && strcmp(nm, want[i]) == 0)
            hits++;
    }
    expect("and its slots are where the reader says they are"
           " (the names decode)", hits == 4);

    /* ---- 2. the vendor's own shape, in the same container ------------- */
    memset(prjFile, 0xA7, sizeof prjFile);
    for (i = 0; i < 4; i++)
        mkItem(i, (i == 2) ? (void *)prjFile : (void *)(size_t)(0x3000
                                                                + i * 8),
               (unsigned char)(0x10 + i * 0x11));
    if (n == 4 && arr) {
        void **w = (void **)arr;

        for (i = 0; i < 4; i++)
            w[i] = itemBuf[i];
        expect("the scan asked about is found at its own slot",
               mlFindItem(arr, n, ML_ITEM_SELF, prjFile) == 2);
        expect("a scan that is not in the list is not found",
               mlFindItem(arr, n, ML_ITEM_SELF, src) == -1);
        okAll = 1;
        for (i = 0; i < 4; i++) {
            memset(uuid, 0, sizeof uuid);
            if (!mlItemId(arr, i, ML_ITEM_ID, uuid)
                || uuid[0] != (unsigned char)(0x10 + i * 0x11)
                || uuid[15] != (unsigned char)(0x10 + i * 0x11 + 15))
                okAll = 0;
        }
        expect("every slot's 16 id bytes come back unchanged", okAll);
    }

    /* ---- 3. the container the merge loop really shows ----------------- */
    /* args+0x70 holds the QList; four items, all used, begin at 0 */
    mkHdr(hdrA, 4, 0, 4, 4);
    for (i = 0; i < 4; i++)
        *(void **)(hdrA + ML_HDR + i * 8) = itemBuf[i];
    memset(args, 0, sizeof args);
    *(void **)(args + ML_LIST_OFF) = hdrA;
    n = mlHeader(args + ML_LIST_OFF, &arr, &alloc);
    expect("a four-scan list reads as four scans (alloc 4)",
           n == 4 && alloc == 4
           && mlFindItem(arr, n, ML_ITEM_SELF, prjFile) == 2);

    /* and the same list with the vendor's own slack: room at both ends, so
     * the used elements start at slot 2 - Qt's begin()/end() walk, and the
     * reason the count is end - begin rather than a stored size */
    mkHdr(hdrB, 4, 2, 4, 4);
    for (i = 0; i < 4; i++)
        *(void **)(hdrB + ML_HDR + i * 8) = itemBuf[i];
    memset(args, 0, sizeof args);
    *(void **)(args + ML_LIST_OFF) = hdrB;
    n = mlHeader(args + ML_LIST_OFF, &arr, &alloc);
    expect("a list that starts at slot 2 reads its TWO scans, not four",
           n == 2 && mlFindItem(arr, n, ML_ITEM_SELF, prjFile) == 0);
    expect("and it hands back the USED slot, holding item 2",
           n == 2 && *(const void *const *)arr == (const void *)itemBuf[2]);

    /* ---- 4. what the register scan accepts ---------------------------- */
    /* the module tries every saved register and takes the ONE that holds
     * this merge's list: a candidate is only accepted when the list it
     * points at contains the scan being asked about.  Fill the block with
     * realistic garbage - real addresses of unrelated memory - and the
     * list must be the only hit.  rcx deliberately holds a CODE address:
     * the 2026-11-26 live run of 1.7.0 proved that at these two call sites
     * the vendor's rcx is not `self`, and the shipping acceptance rule is
     * the list-content one, which has to work in exactly this shape. */
    memset(args, 0, sizeof args);
    *(void **)(args + ML_LIST_OFF) = hdrA;
    memset(&ctx, 0, sizeof ctx);
    ctx.rflags = 0x202;
    ctx.r11    = (ULONG64)(ULONG_PTR)&want[0];
    ctx.r10    = 0;
    ctx.r9     = (ULONG64)(ULONG_PTR)itemBuf[1];
    ctx.r8     = (ULONG64)(ULONG_PTR)src;
    ctx.rdi    = (ULONG64)(ULONG_PTR)(args + ML_LIST_OFF);
    ctx.rsi    = (ULONG64)(ULONG_PTR)hdrA;
    ctx.rdx    = (ULONG64)(ULONG_PTR)&ctx;
    ctx.rcx    = (ULONG64)((ULONG_PTR)&main + 0x3BB);   /* code, not self */
    ctx.rbx    = (ULONG64)(ULONG_PTR)itemBuf[2];
    ctx.rax    = (ULONG64)(ULONG_PTR)itemBuf[2];

    for (i = 0; i < (int)(sizeof(ctx) / sizeof(ULONG64)); i++) {
        const void *cand = (const void *)(ULONG_PTR)
                           ((const ULONG64 *)&ctx)[i];

        n = mlHeader(cand, &arr, &alloc);
        if (n < 0)
            continue;
        if (mlFindItem(arr, n, ML_ITEM_SELF, prjFile) < 0)
            continue;
        accepted++;
        list = cand;
    }
    expect("exactly one of the eleven registers is accepted as the list",
           accepted == 1);
    expect("and it is the one that really holds it (rdi here, r9 live)",
           accepted == 1 && list == args + ML_LIST_OFF);
    expect("the scan is still found although rcx is NOT self"
           " (the hub shows a code address in that slot)",
           ctx.rcx != (ULONG64)(ULONG_PTR)prjFile && accepted == 1);

    /* ---- 5. what it refuses ------------------------------------------- */
    expect("null is not a list", mlHeader(NULL, &arr, &alloc) == -1);
    expect("a small integer is not a list",
           mlHeader((const void *)0x40, &arr, &alloc) == -1);
    expect("a stack of zeros is not a list",
           mlHeader(src, &arr, &alloc) == -1);
    expect("a buffer whose header words are all zero is refused",
           mlHeader(itemBuf[0], &arr, &alloc) == -1);

    {   /* an empty real QStringList: Qt's own size 0 container */
        unsigned char src2[64], sl2[64];

        memset(src2, 0, sizeof src2);
        strLit(src2, "");
        memset(sl2, 0, sizeof sl2);
        strSplit(src2, sl2, ',', 1, 0);
        expect("an empty QStringList is refused, not guessed at",
               mlHeader(sl2, &arr, &alloc) == -1);
    }

    {   /* an item the module must not fault on: plausible list, unreadable
         * item (the reader refuses the pointer before it is ever touched) */
        static unsigned char badHdr[ML_HDR + 8];
        static unsigned char badList[8];

        mkHdr(badHdr, 1, 0, 1, 1);
        *(void **)(badHdr + ML_HDR) = (void *)0x1000;   /* unreadable item */
        *(void **)badList = badHdr;
        n = mlHeader(badList, &arr, &alloc);
        expect("a list is still read when an item is unreadable", n == 1);
        expect("but the unreadable item yields no id, and no fault",
               mlItemId(arr, 0, ML_ITEM_ID, uuid) == 0);
        expect("and an unreadable item can never be mistaken for self",
               mlFindItem(arr, n, ML_ITEM_SELF, prjFile) == -1);
    }

    {   /* an item whose id field is all zero = padding, not an id */
        static unsigned char zeroItem[0x80];

        memset(zeroItem, 0, sizeof zeroItem);
        *(void **)(zeroItem + ML_ITEM_SELF) = prjFile;
        memset(itemBuf[3], 0, 0x80);
        n = mlHeader(sl, &arr, &alloc);          /* reuse the 4-slot list */
        if (n == 4) {
            ((void **)arr)[0] = zeroItem;
            expect("an all-zero id is reported as no id",
                   mlItemId(arr, 0, ML_ITEM_ID, uuid) == 0);
            expect("while the item itself is still found by its self link",
                   mlFindItem(arr, n, ML_ITEM_SELF, prjFile) == 0);
        }
    }

    printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL CHECKS PASSED",
           g_failed);
    return g_failed ? 1 : 0;
}
