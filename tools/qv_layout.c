/* qv_layout.c - what a QVariant actually looks like in the Qt build this
 * product ships.
 *
 * selective_mesh reads scene command arguments straight out of memory (it
 * must not call into Qt from inside a hook), so it needs to know where Qt
 * keeps a value's type tag and where the payload starts.  This tool asks no
 * questions: it loads the app's own Qt5Core.dll, builds real QVariants and
 * real lists through exported functions and prints the bytes.
 *
 * Calling convention note (measured, see qmap_lab.c too): this Qt5Core was
 * built with the Microsoft ABI but member functions take `this` in RCX and
 * the hidden return slot in RDX, and hand the finished object back in RAX -
 * the opposite of the documented MSVC layout for by-value returns.  Every
 * call below is FnRet(self, &result).
 *
 *   cl /nologo /O2 /MT /utf-8 qv_layout.c
 *   qv_layout.exe "C:\Program Files\Shining3d\EXStar Hub\Qt5Core.dll"
 *
 * Development tool; nothing here runs in the product.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef void *(*FnCtorB)(void *self, char v);
typedef void *(*FnCtorQ)(void *self, const void *q);
typedef void *(*FnCtorS)(void *self, const char *lit);
typedef const char *(*FnTypeName)(const void *self);
typedef int (*FnUserType)(const void *self);
typedef const unsigned short *(*FnUtf16)(const void *self);
typedef void *(*FnCtorBA)(void *self, const char *data, int len);
typedef void (*FnParse)(void *doc, const void *qba, void *errOut);
typedef void *(*FnRet)(const void *self, void *retOut);
/* QString::split(QChar, Qt::SplitBehavior, Qt::CaseSensitivity) const:
 * `this` in RCX, the hidden return slot in RDX (this Qt build's ABI, see
 * the note at the top), then QChar, behavior, case sensitivity. */
typedef void *(*FnSplit)(const void *self, void *retOut, unsigned short sep,
                         int behavior, int cs);

static HMODULE g_qt;

static void *imp(const char *name)
{
    void *p = (void *)GetProcAddress(g_qt, name);

    if (!p)
        printf("  ! missing export %s\n", name);
    return p;
}

static void dump(const char *what, const unsigned char *p, int n)
{
    int i;

    printf("%-22s", what);
    for (i = 0; i < n; i++)
        printf("%s%02X", (i && i % 8 == 0) ? " " : "", p[i]);
    printf("\n");
}

/* one QString exactly as selective_mesh reads it: one pointer to QArrayData */
static void showQStringObj(const char *what, const unsigned char *d)
{
    int size;
    long long off;

    if (!d) {
        printf("%-22s (null)\n", what);
        return;
    }
    size = *(const int *)(d + 8);
    off  = *(const long long *)(d + 16);
    printf("%-22s d=%p size=%d offset=%lld data=%ls\n", what, (void *)d, size,
           off, (const wchar_t *)(d + (off ? off : 24)));
}

/* QListData::Data: measure it, do not assume it.  The dwords are printed
 * first so the ref/alloc/size trio can be found wherever it lives, then
 * every qword of the block is interpreted as a QArrayData pointer, because
 * a list of strings holds one QString (one pointer) per element. */
static void showListData(const char *what, unsigned char *lst, int itemSize)
{
    int a, ref = *(int *)(lst + 0), alloc = *(int *)(lst + 4);
    int s8 = *(int *)(lst + 8), s12 = *(int *)(lst + 12);

    printf("%-22s ref=%d alloc=%d dword+8=%d dword+C=%d\n", what, ref, alloc,
           s8, s12);
    dump("  header bytes", lst, 48);
    for (a = 0x10; a <= 0x28; a += 8) {
        unsigned char *v = *(unsigned char **)(lst + a);
        int i, n = (s8 > 0 && s8 < 16) ? s8 : ((s12 > 0 && s12 < 16) ? s12 : 3);

        printf("  qword +0x%02X = %p", a, (void *)v);
        if (!v || (uintptr_t)v < 0x10000) {
            printf("\n");
            continue;
        }
        __try {
            for (i = 0; i < n && i < 4; i++) {
                unsigned char *el = v + (size_t)i * itemSize;

                printf("\n      +0x%02X[%d] @%p tag=%u ", a, i, (void *)el,
                       itemSize == 16 ? (*(unsigned *)(el + 8)
                                        & 0x3FFFFFFFu) : 0u);
                showQStringObj("", *(unsigned char **)el);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  (unreadable)\n");
        }
        if (i == 0)
            printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *qtPath = (argc > 1) ? argv[1]
        : "C:\\Program Files\\Shining3d\\EXStar Hub\\Qt5Core.dll";
    FnCtorB    varBool;
    FnCtorQ    varStr, varList, varFromList;
    FnCtorS    strLit;
    FnTypeName varType;
    FnUserType varUt;
    FnUtf16    strU16;
    FnCtorBA   baLit;
    FnParse    jsonParse;
    FnRet      docArray, arrToVarList, varToStrList;
    FnSplit    strSplit;
    unsigned char vt[64], vb[64], vs[64], vl[64], qs[64], single[32];
    unsigned char doc[64], jarr[32], varr[32], qba[64], vq[64];
    unsigned char *lst = NULL, *obj = NULL;
    int i;

    g_qt = LoadLibraryExA(qtPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_qt) {
        printf("cannot load %s (err %u)\n", qtPath, (unsigned)GetLastError());
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);      /* a fault must not eat the log */
    printf("Qt5Core at %p\n\n", (void *)g_qt);

    varBool    = (FnCtorB)imp("??0QVariant@@QEAA@_N@Z");
    varStr     = (FnCtorQ)imp("??0QVariant@@QEAA@AEBVQString@@@Z");
    varList    = (FnCtorQ)imp("??0QVariant@@QEAA@AEBVQStringList@@@Z");
    strLit     = (FnCtorS)imp("??0QString@@QEAA@PEBD@Z");
    varType    = (FnTypeName)imp("?typeName@QVariant@@QEBAPEBDXZ");
    varUt      = (FnUserType)imp("?userType@QVariant@@QEBAHXZ");
    strU16     = (FnUtf16)imp("?utf16@QString@@QEBAPEBGXZ");
    baLit      = (FnCtorBA)imp("??0QByteArray@@QEAA@PEBDH@Z");
    jsonParse  = (FnParse)imp("?fromJson@QJsonDocument@@SA?AV1@AEBVQByteArr"
                             "ay@@PEAUQJsonParseError@@@Z");
    docArray   = (FnRet)imp("?array@QJsonDocument@@QEBA?AVQJsonArray@@XZ");
    arrToVarList = (FnRet)imp("?toVariantList@QJsonArray@@QEBA?AV?$QList@V"
                             "QVariant@@@@XZ");
    varToStrList = (FnRet)imp("?toStringList@QVariant@@QEBA?AVQStringList@@"
                             "XZ");

    strSplit   = (FnSplit)imp("?split@QString@@QEBA?AVQStringList@@VQChar@@"
                              "W4SplitBehavior@1@W4CaseSensitivity@Qt@@@Z");

    /* ---- the two shapes the commands use: a bool and a string -------- */
    memset(vt, 0, sizeof vt);
    if (varBool)
        varBool(vt, 1);
    memset(vb, 0, sizeof vb);
    if (varBool)
        varBool(vb, 0);
    memset(qs, 0, sizeof qs);
    if (strLit)
        strLit(qs, "Project_2");

    printf("QVariant(true)        typeName=%s userType=%d\n",
           varType ? varType(vt) : "?", varUt ? varUt(vt) : -1);
    dump("  true", vt, 16);
    printf("QVariant(false)       typeName=%s userType=%d\n",
           varType ? varType(vb) : "?", varUt ? varUt(vb) : -1);
    dump("  false", vb, 16);
    printf("\ndifferences true vs false:\n");
    for (i = 0; i < 16; i++)
        if (vt[i] != vb[i])
            printf("    byte +0x%X: %02X vs %02X\n", i, vt[i], vb[i]);

    printf("\nQString               utf16=%ls\n",
           strU16 ? (const wchar_t *)strU16(qs) : L"?");
    memset(vs, 0, sizeof vs);
    if (varStr)
        varStr(vs, qs);
    printf("\nQVariant(QString)     typeName=%s userType=%d\n",
           varType ? varType(vs) : "?", varUt ? varUt(vs) : -1);
    dump("  variant", vs, 16);
    printf("  payload +0x00 = %p, and the QString's own QArrayData is %p\n",
           *(void **)(vs + 0x00), *(void **)(void *)qs);
    printf("  => a QString value stores the QArrayData directly, so\n"
           "     readQName5(qvariant) is correct as written: %s\n",
           *(void **)(vs + 0x00) == *(void **)(void *)qs ? "YES" : "no");

    if (!jsonParse || !docArray || !arrToVarList || !varToStrList || !baLit) {
        printf("\n! the JSON chain is incomplete, lists not measured\n");
        return 1;
    }

    /* ---- the real thing: a three-element QStringList from Qt's own
     * QString::split().  This is exactly the type a command's `names`
     * carries, built without touching any hand-made container. --------- */
    if (strLit && strSplit) {
        unsigned char s3[64], sl3[32];
        unsigned char *d3;

        memset(s3, 0, sizeof s3);
        strLit(s3, "Project_1111,Project_2,Project_1000");
        memset(sl3, 0, sizeof sl3);
        __try {
            obj = (unsigned char *)strSplit(s3, sl3, ',', 0, 0);
            printf("\nQString::split(\",\") -> QStringList\n");
            d3 = *(unsigned char **)sl3;
            printf("  returned %p, QList.d = %p\n", (void *)obj, (void *)d3);
            if (d3)
                showListData("  Data", d3, 8);

            /* and the same list as a command value would store it */
            if (varList) {
                memset(vl, 0, sizeof vl);
                varList(vl, sl3);
                printf("\nQVariant(QStringList) typeName=%s userType=%d\n",
                       varType ? varType(vl) : "?", varUt ? varUt(vl) : -1);
                dump("  variant", vl, 16);
                printf("  payload +0x00 = %p ; QList.d = %p ; same: %s\n",
                       *(void **)(vl + 0x00), (void *)d3,
                       *(void **)(vl + 0x00) == (void *)d3 ? "YES" : "no");
            }
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                        ? EXCEPTION_EXECUTE_HANDLER
                        : EXCEPTION_CONTINUE_SEARCH) {
            printf("  ! access violation in the split chain\n");
        }
    }

    /* ---- a NON-EMPTY real QStringList: JSON -> QVariantList ->
     * toStringList().  Nothing below is hand-built; this is Qt's own
     * container, laid out the way a command's `names` value is. -------- */
    memset(vb, 0, sizeof vb);
    memset(qba, 0, sizeof qba);
    baLit(qba, "[\"Project_1111\",\"Project_2\",\"Project_1000\"]", -1);
    memset(doc, 0, sizeof doc);
    __try {
        jsonParse(doc, qba, vb);
        printf("\nJSON parse error = %d\n", *(int *)vb);

        memset(jarr, 0, sizeof jarr);
        obj = (unsigned char *)docArray(doc, jarr);
        memset(varr, 0, sizeof varr);
        obj = (unsigned char *)arrToVarList(obj, varr);
        lst = *(unsigned char **)varr;
        printf("QList<QVariant> (3 strings) .d = %p\n", (void *)lst);
        if (lst) {
            int size = *(int *)(lst + 8);

            showListData("  Data", lst, 16);
            for (i = 0; i < size && i < 3; i++) {
                unsigned char *el = lst + 0x18 + i * 8;

                /* a QVariant element: its payload is the QArrayData of the
                 * string, which is what readQName5() reads */
                printf("  variant elem %d       tag=%u  ", i,
                       *(unsigned *)(el + 8) & 0x3FFFFFFFu);
                showQStringObj("", *(unsigned char **)el);
            }
        }

        /* the same items as a QStringList, converted by Qt itself */
        memset(single, 0, sizeof single);
        obj = (unsigned char *)varToStrList(varr, single);
        printf("\nQVariant(QVariantList).toStringList()\n");
        printf("  userType=%d typeName=%s\n", varUt ? varUt(single) : -1,
               varType ? varType(single) : "?");
        lst = *(unsigned char **)single;
        printf("  QList<QString>.d = %p\n", (void *)lst);
        if (lst)
            showListData("  Data", lst, 8);
        memset(vl, 0, sizeof vl);
        if (varList)
            varList(vl, single);
        printf("\nQVariant(QStringList) typeName=%s userType=%d\n",
               varType ? varType(vl) : "?", varUt ? varUt(vl) : -1);
        dump("  variant", vl, 16);
        printf("  payload +0x00 = %p ; QList's Data = %p ; same: %s\n",
               *(void **)(vl + 0x00), (void *)lst,
               *(void **)(vl + 0x00) == (void *)lst ? "YES" : "no");
        printf("  => qlistElems() reading the payload at +0x00 and element 0\n"
               "     at Data+0x18 is %s\n",
               lst && *(void **)(vl + 0x00) == (void *)lst
                   && *(int *)(lst + 8) == 3 ? "CONFIRMED" : "NOT confirmed");
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        printf("  ! access violation in the list chain\n");
    }
    return 0;
}
