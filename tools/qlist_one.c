/* qlist_one.c - what does a ONE-element QStringList look like, and what does
 * a one-element list whose entry is an EMPTY string look like?
 *
 * The hub's `showgroups` arrives as a list this reader refused, and the
 * startup dump said ref=2 alloc=2 begin=0 size=1: one entry.  Either the
 * entry is a name this build stores differently, or it is an empty string,
 * which the reader is right to refuse.  This builds both cases with Qt's own
 * functions and prints what the reader sees.
 *
 *   cl /nologo /O2 /MT /utf-8 /I..\modules\merge_select qlist_one.c
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "qt_read.h"

typedef void *(*FnCtorS)(void *self, const char *lit);
typedef void *(*FnSplit)(const void *self, void *retOut, unsigned short sep,
                         int behavior, int cs);
typedef void *(*FnCtorQ)(void *self, const void *q);
typedef int (*FnUserType)(const void *self);

static HMODULE g_qt;

static void *imp(const char *name)
{
    void *p = (void *)GetProcAddress(g_qt, name);

    if (!p)
        printf("  ! missing export %s\n", name);
    return p;
}

static void bytes(const char *what, const unsigned char *p, int n)
{
    int i;

    printf("%-14s", what);
    for (i = 0; i < n; i++)
        printf("%s%02X", (i && i % 8 == 0) ? " " : "", p[i]);
    printf("\n");
}

static void probe(const char *what, FnCtorQ varStrList, unsigned char *sl)
{
    unsigned char qv[64] = { 0 };
    const unsigned char *d, *e;
    QLIST L;
    char how[64], nm[128];
    int i;

    varStrList(qv, sl);
    d = *(const unsigned char *const *)(qv + 0x00);
    printf("\n%s\n  userType=%d data=%p\n", what,
           varStrList ? 11 : -1, (void *)d);
    if (!plausiblePtr(d)) {
        printf("  no data pointer\n");
        return;
    }
    bytes("  data", d, 48);
    printf("  header   ref=%d alloc=%d begin=%d size=%d\n",
           *(const int *)(d + 0), *(const int *)(d + 4),
           *(const int *)(d + 8), *(const int *)(d + 12));

    e = *(const unsigned char *const *)(d + 0x10);
    printf("  entry@+10 = %p", (void *)e);
    if (plausiblePtr(e)) {
        printf("\n");
        bytes("    behind", e, 40);
        printf("    as QArrayData: size=%d offset=%lld\n",
               *(const int *)(e + 8), *(const long long *)(e + 16));
        nm[0] = 0;
        readQName5(e, nm, (int)sizeof nm);
        printf("    readQName5 -> \"%s\"\n", nm);
    } else {
        printf(" (not a pointer)\n");
    }

    printf("  qlistRead -> %s", qlistRead(qv, &L, how, (int)sizeof how)
           ? "decoded" : "refused");
    if (L.size)
        printf(" (%d entries, %s)\n", L.size, how);
    else
        printf("\n");
    for (i = 0; i < L.size; i++) {
        nm[0] = 0;
        readQName5(qlistElem(&L, i), nm, (int)sizeof nm);
        printf("      [%d] \"%s\"\n", i, nm);
    }
}

int main(int argc, char **argv)
{
    const char *qtPath = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\Qt5Core.dll";
    FnCtorS  strLit;
    FnSplit  strSplit;
    FnCtorQ  varStrList;
    unsigned char s1[64], l1[64], s0[64], l0[64], se[64], le[64];

    g_qt = LoadLibraryExA(qtPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_qt) {
        printf("cannot load %s (err %u)\n", qtPath, (unsigned)GetLastError());
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    strLit     = (FnCtorS)imp("??0QString@@QEAA@PEBD@Z");
    strSplit   = (FnSplit)imp("?split@QString@@QEBA?AVQStringList@@VQChar@@"
                              "W4SplitBehavior@1@W4CaseSensitivity@Qt@@@Z");
    varStrList = (FnCtorQ)imp("??0QVariant@@QEAA@AEBVQStringList@@@Z");
    if (!strLit || !strSplit || !varStrList)
        return 2;

    memset(s1, 0, sizeof s1);
    strLit(s1, "Project_1111");
    memset(l1, 0, sizeof l1);
    strSplit(s1, l1, ',', 1, 0);
    probe("one entry: \"Project_1111\".split(',')", varStrList, l1);

    memset(s0, 0, sizeof s0);
    strLit(s0, "");
    memset(le, 0, sizeof le);
    strSplit(s0, le, ',', 0, 0);           /* KeepEmptyParts */
    probe("one entry, empty: \"\".split(',') keep-empty", varStrList, le);

    memset(se, 0, sizeof se);
    strLit(se, ",");
    memset(l0, 0, sizeof l0);
    strSplit(se, l0, ',', 0, 0);           /* two empty entries */
    probe("two empty entries: \",\".split(',')", varStrList, l0);
    return 0;
}
