/* qlist_lab.c - prove the module's list reader against the Qt this product
 * ships, before the module ever has to guess.
 *
 * selective_mesh reads a scene command's `names` list straight out of memory
 * (it must not call Qt from inside the hook), so it needs to know where Qt
 * keeps the count and the entries of a QStringList.  qt_read.h contains the
 * reader; this tool contains the proof: it loads the app's OWN Qt5Core.dll,
 * builds a real QStringList with Qt's own QString::split(), puts it in a
 * real QVariant and runs the shipped reader on it.  Nothing here is linked
 * against Qt: the containers are made through exported functions, exactly
 * as the vendor's code makes them.
 *
 * Calling convention (measured, see qv_layout.c): this Qt5Core uses the
 * Microsoft ABI, but a member function returning a non-trivially-destructible
 * object takes `this` in RCX and the hidden return slot in RDX, and returns
 * the slot in RAX.  So every such call below is Fn(self, &result, ...).
 *
 *   cl /nologo /O2 /MT /utf-8 /I..\modules\selective_mesh qlist_lab.c
 *   qlist_lab.exe "C:\Program Files\Shining3d\EXStar Hub\Qt5Core.dll"
 *
 * Exit code 0 = the reader decoded the expected names.  Development tool;
 * nothing here runs in the product.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "qt_read.h"          /* the code under test, verbatim */

typedef void *(*FnCtorS)(void *self, const char *lit);
typedef void *(*FnSplit)(const void *self, void *retOut, unsigned short sep,
                         int behavior, int cs);
typedef void *(*FnCtorQ)(void *self, const void *q);
typedef int (*FnUserType)(const void *self);
typedef void *(*FnCtorBA)(void *self, const char *data, int len);
typedef void (*FnParse)(void *doc, const void *qba, void *errOut);
typedef void *(*FnRet)(const void *self, void *retOut);

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

int main(int argc, char **argv)
{
    const char *qtPath = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\Qt5Core.dll";
    static const char *want[3]
        = { "Project_1111", "Project_2", "Project_1000" };
    FnCtorS    strLit;
    FnSplit    strSplit;
    FnCtorQ    varStrList;
    FnUserType varUt;
    unsigned char src[64], sl[64], qv[64];
    unsigned char *r;
    QLIST L;
    char how[64], nm[128];
    int i, got = 0, hits = 0;

    g_qt = LoadLibraryExA(qtPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_qt) {
        printf("cannot load %s (err %u)\n", qtPath, (unsigned)GetLastError());
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Qt5Core at %p\n\n", (void *)g_qt);

    strLit     = (FnCtorS)imp("??0QString@@QEAA@PEBD@Z");
    strSplit   = (FnSplit)imp("?split@QString@@QEBA?AVQStringList@@VQChar@@"
                              "W4SplitBehavior@1@W4CaseSensitivity@Qt@@@Z");
    varStrList = (FnCtorQ)imp("??0QVariant@@QEAA@AEBVQStringList@@@Z");
    varUt      = (FnUserType)imp("?userType@QVariant@@QEBAHXZ");
    if (!strLit || !strSplit || !varStrList)
        return 2;

    /* a real QStringList, built by Qt from a real QString */
    memset(src, 0, sizeof src);
    strLit(src, "Project_1111,Project_2,Project_1000");
    memset(sl, 0, sizeof sl);
    r = (unsigned char *)strSplit(src, sl, ',', 0, 0);
    printf("QString::split(',') -> QStringList at %p, data %p\n",
           (void *)r, *(void **)sl);

    /* the list as the command argument carries it: inside a QVariant */
    memset(qv, 0, sizeof qv);
    varStrList(qv, sl);
    printf("QVariant(QStringList): userType=%d, payload=%p, tag=%u\n\n",
           varUt ? varUt(qv) : -1, *(void **)(qv + 0x00),
           *(const unsigned *)(qv + 0x08) & 0x3FFFFFFFu);

    printf("reader answer: ");
    if (!qlistRead(qv, &L, how, (int)sizeof how)) {
        printf("NOT DECODED\n");
        g_failed++;
    } else {
        printf("%d entries via %s\n", L.size, how);
        got = L.size;
        for (i = 0; i < L.size; i++) {
            nm[0] = 0;
            readQName5(qlistElem(&L, i), nm, (int)sizeof nm);
            printf("      [%d] \"%s\"\n", i, nm);
            if (i < 3 && strcmp(nm, want[i]) == 0)
                hits++;
        }
    }

    printf("\nchecks\n");
    expect("all three entries decoded", got == 3);
    expect("and they are the model names that went in", hits == 3);

    /* an empty list must be refused, not decoded as garbage: the hub's
     * SetCurrentEnv arrives this way at every start.  Built the same real
     * way - splitting an empty string with Qt::SkipEmptyParts (1) gives a
     * list Qt constructed itself, size 0. */
    {
        unsigned char src2[64], sl2[64];
        const unsigned char *d2;

        memset(src2, 0, sizeof src2);
        strLit(src2, "");
        memset(sl2, 0, sizeof sl2);
        strSplit(src2, sl2, ',', 1, 0);
        memset(qv, 0, sizeof qv);
        varStrList(qv, sl2);
        d2 = *(const unsigned char *const *)(qv + 0x00);
        printf("\nempty QStringList: userType=%d, data=%p, header words"
               " ref=%d alloc=%d +8=%d size=%d\n",
               varUt ? varUt(qv) : -1, (void *)d2,
               d2 ? *(const int *)(d2 + 0x00) : -1,
               d2 ? *(const int *)(d2 + 0x04) : -1,
               d2 ? *(const int *)(d2 + 0x08) : -1,
               d2 ? *(const int *)(d2 + 0x0C) : -1);
        expect("an empty QStringList is not decoded",
               qlistRead(qv, &L, how, (int)sizeof how) == 0);
    }

    /* garbage must be refused too (a QVariant is 16 bytes; read one bool) */
    {
        void (*varBool)(void *self, char v)
            = (void (*)(void *, char))imp("??0QVariant@@QEAA@_N@Z");

        if (varBool) {
            memset(qv, 0, sizeof qv);
            varBool(qv, 1);
            expect("a bool value is not decoded as a list",
                   qlistRead(qv, &L, how, (int)sizeof how) == 0);
        }
    }

    /* and a QVariant whose payload is not a container at all */
    memset(qv, 0xAA, sizeof qv);
    expect("uninitialised memory is refused, not decoded",
           qlistRead(qv, &L, how, (int)sizeof how) == 0);

    /* a real command may carry an empty name next to a real one: the empty
     * entry is valid and must not make the whole list unreadable, while
     * `named` says how many entries actually name something */
    {
        unsigned char src3[64], sl3[64], qv3[64];
        int ok;

        memset(src3, 0, sizeof src3);
        strLit(src3, "Project_2,");
        memset(sl3, 0, sizeof sl3);
        strSplit(src3, sl3, ',', 0, 0);           /* Qt::KeepEmptyParts */
        memset(qv3, 0, sizeof qv3);
        varStrList(qv3, sl3);
        ok = qlistRead(qv3, &L, how, (int)sizeof how);
        printf("\n\"Project_2,\".split(keep-empty): %s",
               ok ? how : "refused");
        for (i = 0; ok && i < L.size; i++) {
            nm[0] = 0;
            readQName5(qlistElem(&L, i), nm, (int)sizeof nm);
            printf("  [%d]\"%s\"", i, nm);
        }
        printf("\n");
        expect("a name next to an empty entry still decodes",
               ok && L.size == 2 && L.named == 1);
    }

    printf("\n%s\n", g_failed ? "FAILED" : "the shipped reader matches the shipped Qt");
    return g_failed ? 1 : 0;
}
