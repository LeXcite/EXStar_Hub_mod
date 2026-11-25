/*
 * qt_read.h - read the Qt 5 containers this product ships, as data.
 *
 * Selective Mesh answers inside the vendor's own calls, and it must not
 * call Qt from there: v1.5.0 asked QVariant what a value was, and the hub
 * died in Qt5Core on every start (docs\research\merge-common-mesh.md).  So
 * the module inspects the containers instead.  These readers are the whole
 * of that layout knowledge, and they live in a header for one reason:
 * tools\qlist_lab.c builds real containers with the shipped Qt5Core's own
 * functions and runs THIS code on them outside the product.  What is proven
 * there is what runs here; nothing in the module is a layout guess that
 * only the module could try.
 *
 * Every function is read-only, bounded and SEH-guarded.  A value that is
 * not the shape it expects yields 0 or NULL - never a fault, and never a
 * half-read that could be mistaken for a real name.
 *
 * Needs <windows.h>, <string.h> and <stdio.h> from the includer.
 */

#ifndef EXSTAR_QT_READ_H
#define EXSTAR_QT_READ_H

/* cheap pre-filter before a guarded deref: a Qt object pointer is never a
 * small integer, so garbage arguments do not even reach the SEH path
 * (exceptions are slow, and this runs inside the vendor's own calls) */
static int plausiblePtr(const void *p)
{
    ULONG_PTR v = (ULONG_PTR)p;

    return v >= 0x10000 && v < (ULONG_PTR)0x00007FFFFFFF0000ULL
           && (v & 7) == 0;
}

/* Qt5 QString read as ASCII (command names, model names and uuid strings
 * are all ASCII).  The object is one pointer to QArrayData
 * { ref(4) alloc(4) size(4) pad(4) offset(8) } and the payload starts at
 * (char*)d + offset, an offset of 0 meaning "directly behind the header".
 * A short string built by QString's small-string path counts its
 * terminator in `size`, so the copy stops at the first NUL as well -
 * otherwise keys arrive as "cmd\0" and a strcmp against "cmd" fails.
 * Anything else that is not printable is shown as ~XX so the log says
 * what was really there.  Both reads are guarded and size-checked: a wrong
 * guess yields "" and never a fault. */
static int readQName5(const void *qstr, char *out, int cap)
{
    const unsigned char *d = NULL, *pl = NULL;
    const unsigned short *u;
    long long off = 0;
    int size = -1, i, n = 0;

    out[0] = 0;
    __try {
        if (qstr) {
            d = *(const unsigned char *const *)qstr;
            if (d) {
                size = *(const int *)(d + 8);
                off  = *(const long long *)(d + 16);
                pl   = d + (off ? off : 24);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        size = -1;
    }
    if (size <= 0 || size > 4096 || !pl)
        return 0;
    if (size > cap / 2 - 1)
        size = cap / 2 - 1;
    u = (const unsigned short *)pl;
    __try {
        for (i = 0; i < size && n < cap - 4; i++) {
            unsigned short c = u[i];

            if (c == 0)
                break;                    /* terminator counted in size */
            if (c >= 0x20 && c < 0x7F)
                n += _snprintf_s(out + n, cap - n, _TRUNCATE, "%c", (char)c);
            else
                n += _snprintf_s(out + n, cap - n, _TRUNCATE, "~%02X",
                                 c & 0xFF);
            if (n < 0)
                break;
        }
        out[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return 0;
    }
    return n > 0;
}

/* is this the address of a QString, and how long is it?  Validates the
 * QArrayData header instead of the text, so an EMPTY string - which
 * readQName5() cannot tell apart from garbage - is recognised as the valid
 * entry it is: a shared or freshly built one has ref >= -1, a size of zero
 * and an offset of either 0 (payload behind the header) or a real offset
 * into a detached buffer. */
static int qStrSize5(const void *qstr, int *sizeOut)
{
    const unsigned char *d = NULL;
    long long off;
    int size, ref;

    if (sizeOut)
        *sizeOut = -1;
    if (!plausiblePtr(qstr))
        return 0;
    __try {
        d = *(const unsigned char *const *)qstr;
        if (!plausiblePtr(d))
            return 0;
        ref  = *(const int *)(d + 0);
        size = *(const int *)(d + 8);
        off  = *(const long long *)(d + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (ref < -1 || ref > 1000000)
        return 0;
    if (size < 0 || size > 4096)
        return 0;
    if (off != 0 && (off < 24 || off > 0x100000))
        return 0;
    if (sizeOut)
        *sizeOut = size;
    return 1;
}

/* A list of names, as found inside a QVariant: where the entries start, how
 * many there are, whether each entry IS a QString or POINTS at one, and how
 * many of the entries actually hold text (`named`). */
typedef struct {
    const unsigned char *arr;
    int size;
    int hop;
    int named;
} QLIST;

/* Locate the elements of a name list stored in a QVariant.
 *
 * MEASURED against the Qt5Core this product ships, with tools\qlist_lab.c,
 * which builds the containers with Qt's own functions and then reads them
 * with the functions in this header:
 *
 *   QString("Project_1111,Project_2,Project_1000").split(',') -> QStringList
 *     QVariant  +0x00 -> the list's Data;  +0x08 tag = 11
 *     Data      +0x00 ref=1  +0x04 alloc=6  +0x08 begin=0  +0x0C size=3
 *               +0x10, +0x18, +0x20 = three QStrings (one pointer each),
 *               +0x28 = 0
 *
 * so the count is the dword at +0x0C (this build keeps `begin` at +0x08,
 * where an empty or freshly built list reads as zero), and a list of
 * pointer-sized items - which a QString is - lies INLINE from +0x10.  A list
 * of bigger items, measured on a QList<QVariant> built from JSON, keeps a
 * NODE ARRAY instead: the entry points at the value, one hop further.  The
 * two differ only in that hop, so both are searched and named.
 *
 * Nothing is trusted because it was measured once.  A combination is
 * accepted only when the header words are sane AND every element up to the
 * count parses as a non-empty printable string; the caller reads the list
 * again and refuses to decide on a partial read.  Anything else yields 0,
 * which means stock behaviour.  `howOut` (optional) names the winning
 * combination so the log can show it once and the reading can be retired
 * from the log of a normal start. */
static int qlistRead(const void *qv, QLIST *L, char *howOut, int howCap)
{
    static const int sizeAt[3] = { 0x0C, 0x08, 0x04 };
    static const int baseAt[3] = { 0x10, 0x18, 0x20 };
    static const int hops[2]   = { 0, 1 };   /* inline first: that is Qt */
    unsigned char *d;
    int z, b, h;

    memset(L, 0, sizeof *L);
    if (howOut && howCap > 0)
        howOut[0] = 0;
    if (!plausiblePtr(qv))
        return 0;

    __try {
        d = *(unsigned char **)(void *)qv;
        if (!plausiblePtr(d))
            return 0;
        if (*(int *)(d + 0) < 1 || *(int *)(d + 0) > 4096)
            return 0;                 /* not a shared header at all */
        if (*(int *)(d + 4) < 0 || *(int *)(d + 4) > 4096)
            return 0;                 /* the reserved count is nonsense */

        for (z = 0; z < 3; z++) {
            int size = *(int *)(d + sizeAt[z]);

            if (size < 1 || size > 64)
                continue;
            for (b = 0; b < 3; b++) {
                const unsigned char *arr = d + baseAt[b];

                for (h = 0; h < 2; h++) {
                    int i, named = 0;

                    for (i = 0; i < size; i++) {
                        const unsigned char *slot = arr + (size_t)i * 8;
                        const void *at = slot;
                        int len = -1;

                        if (hops[h]) {
                            at = *(const void *const *)slot;
                            if (!plausiblePtr(at))
                                break;
                        }
                        /* an empty name is a real entry (a command may well
                         * carry one), so validity is judged on the QString,
                         * not on its text - but at least one entry must
                         * hold text, or nothing was read */
                        if (!qStrSize5(at, &len))
                            break;      /* a gap: this is not the array */
                        if (len > 0)
                            named++;
                    }
                    if (i < size || named < 1)
                        continue;
                    L->arr = arr;
                    L->size = size;
                    L->hop = hops[h];
                    L->named = named;
                    if (howOut && howCap > 0)
                        _snprintf_s(howOut, howCap, _TRUNCATE,
                                    "size@+0x%02X entries@+0x%02X hop%d",
                                    sizeAt[z], baseAt[b], hops[h]);
                    return 1;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(L, 0, sizeof *L);
    }
    return 0;
}

/* element `i` as the address a QString sits at, ready for readQName5(), or
 * NULL.  Reads through the hop the search chose, so the whole list is always
 * interpreted the same way. */
static const void *qlistElem(const QLIST *L, int i)
{
    const unsigned char *slot;

    if (!L || !L->arr || i < 0 || i >= L->size)
        return NULL;
    slot = L->arr + (size_t)i * 8;
    if (!L->hop)
        return slot;
    __try {
        return *(const void *const *)slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

#endif /* EXSTAR_QT_READ_H */
