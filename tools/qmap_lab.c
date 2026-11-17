/* Offline lab: build a real QMap<QString,QVariant> with exported Qt5Core
 * functions (QJsonDocument -> toVariantMap), then empirically verify the
 * tree/node layout our probe relies on. No app, no injection. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef int                 (*fn_userType)(const void *);
typedef const void         *(*fn_constData)(const void *);
typedef int                 (*fn_strSize)(const void *);
typedef const unsigned short *(*fn_strUtf16)(const void *);
typedef void                (*fn_qbaCtor)(void *, const char *, int);
typedef void                (*fn_fromJson)(void *, const void *, void *);
typedef void                (*fn_object)(void *, const void *);
typedef void                (*fn_toVariantMap)(void *, const void *);

static fn_userType     p_userType;
static fn_constData    p_constData;
static fn_strSize      p_strSize;
static fn_strUtf16     p_strUtf16;
static fn_qbaCtor      p_qbaCtor;
static fn_fromJson     p_fromJson;
static fn_object       p_object;
static fn_toVariantMap p_toVariantMap;

static void hdump(const char *tag, const unsigned char *p, int n)
{
    unsigned char buf[0x80];
    int i;

    if (n > (int) sizeof buf)
        n = (int) sizeof buf;
    __try {
        memcpy(buf, p, (size_t) n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("%-14s(UNREADABLE)\n", tag);
        return;
    }
    printf("%-14s", tag);
    for (i = 0; i < n; i++)
        printf("%02x ", buf[i]);
    printf("\n");
}

static int try_str(const void *obj, char *out, int cap)
{
    int n = 0;
    const unsigned short *u = NULL;
    __try {
        n = p_strSize(obj);
        if (n <= 0 || n >= cap)
            return -1;
        u = p_strUtf16(obj);
        if (!u)
            return -1;
        for (int i = 0; i < n; i++) {
            if (u[i] < 0x20 || u[i] > 0x7E)
                return -1;
            out[i] = (char) u[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    out[n] = 0;
    return n;
}

static int try_type(const void *v)
{
    int t = -1;
    __try { t = p_userType(v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -999; }
    return t;
}

static int g_nodes;

static void walk(const void *node, const void *header, int depth)
{
    char s[64];
    const void *l = NULL, *r = NULL;
    uintptr_t ptag = 0;

    if (!node || node == header || depth > 32)
        return;
    __try {
        ptag = *(const uintptr_t volatile *) node;
        l = *(const void *volatile *) ((const char *) node + 8);
        r = *(const void *volatile *) ((const char *) node + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("node @%p UNREADABLE\n", node);
        return;
    }
    walk(l, header, depth + 1);

    printf("node %d @%p parent=%p(+0) left=%p(+8) right=%p(+16)\n",
           ++g_nodes, node, (void *) (ptag & ~(uintptr_t) 3), (void *) l,
           (void *) r);
    hdump("   raw", (const unsigned char *) node, 0x70);
    for (int koff = 24; koff <= 32; koff += 8) {
        int n = try_str((const char *) node + koff, s, (int) sizeof s);
        printf("   key@+%d -> %s\n", koff, n >= 0 ? s : "(no)");
    }
    for (int voff = 32; voff <= 48; voff += 8)
        printf("   val@+%d -> userType=%d\n", voff,
               try_type((const char *) node + voff));
    walk(r, header, depth + 1);
}

int main(void)
{
    HMODULE qt = LoadLibraryW(L"Qt5Core.dll");
    unsigned char doc[64], obj[64], map[64];
    unsigned char ba[64];
    unsigned char *d, *h;
    unsigned char *root = NULL;
    static const char json[] =
        "{\"alpha\":1,\"beta\":\"two\",\"gamma\":3.5,\"delta\":\"four\"}";
    char parseErr[16];

    memset(parseErr, 0, sizeof parseErr);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!qt) { printf("no Qt5Core\n"); return 1; }
    p_userType     = (fn_userType) GetProcAddress(qt, "?userType@QVariant@@QEBAHXZ");
    p_constData    = (fn_constData) GetProcAddress(qt, "?constData@QVariant@@QEBAPEBXXZ");
    p_strSize      = (fn_strSize) GetProcAddress(qt, "?size@QString@@QEBAHXZ");
    p_strUtf16     = (fn_strUtf16) GetProcAddress(qt, "?utf16@QString@@QEBAPEBGXZ");
    p_qbaCtor      = (fn_qbaCtor) GetProcAddress(qt, "??0QByteArray@@QEAA@PEBDH@Z");
    p_fromJson     = (fn_fromJson) GetProcAddress(qt,
        "?fromJson@QJsonDocument@@SA?AV1@AEBVQByteArray@@PEAUQJsonParseError@@@Z");
    p_object       = (fn_object) GetProcAddress(qt,
        "?object@QJsonDocument@@QEBA?AVQJsonObject@@XZ");
    p_toVariantMap = (fn_toVariantMap) GetProcAddress(qt,
        "?toVariantMap@QJsonObject@@QEBA?AV?$QMap@VQString@@VQVariant@@@@XZ");
    if (!p_userType || !p_constData || !p_strSize || !p_strUtf16 ||
        !p_qbaCtor || !p_fromJson || !p_object || !p_toVariantMap) {
        printf("resolve failed\n");
        return 1;
    }

    memset(doc, 0, sizeof doc);
    memset(obj, 0, sizeof obj);
    memset(map, 0, sizeof map);
    printf("stage ctor\n");
    p_qbaCtor(ba, json, -1);
    hdump("ba[0..8]", ba, 8);
    printf("stage fromJson\n");
    p_fromJson(doc, ba, parseErr);          /* static: hidden sret first */
    printf("parseError=%d offset=%d\n", *(int *) parseErr, *(int *) (parseErr + 4));
    hdump("doc[0..16]", doc, 16);
    printf("stage object\n");
    /* NOTE: this Qt5Core build is GCC/MinGW MS-ABI: member functions take
       `this` in RCX and the hidden return-slot in RDX (verified from the
       object()/toVariantMap prologues), the opposite of documented MSVC. */
    p_object(doc, obj);                     /* this first, sret second */
    hdump("obj[0..16]", obj, 16);
    printf("stage toVariantMap\n");
    p_toVariantMap(obj, map);               /* this first, sret second */
    hdump("map[0..8]", map, 8);
    printf("stage done\n");

    d = NULL;
    __try {
        d = *(unsigned char ** volatile *) map;
    } __except (EXCEPTION_EXECUTE_HANDLER) { d = NULL; }
    printf("d=%p\n", (void *) d);
    if (!d) { printf("null d\n"); return 1; }
    hdump("d[0..0x28]", d, 0x28);
    h = d + 8;
    __try {
        printf("header=%p d+8=%p d+10=%p d+18=%p mostLeft@+0x20=%p\n",
               (void *) h, *(void *volatile *)(d + 8),
               *(void *volatile *)(d + 0x10), *(void *volatile *)(d + 0x18),
               *(void *volatile *)(d + 0x20));
        root = (unsigned char *) (((uintptr_t) *(void *volatile *)(d + 0x10))
                                  & ~(uintptr_t) 3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("d deref fault\n");
        return 1;
    }
    printf("untagged root=%p\n", (void *) root);
    if (root)
        hdump("root[0..0x40]", root, 0x40);
    walk(root, h, 0);
    printf("walked %d nodes (expect 4)\n", g_nodes);
    return 0;
}
