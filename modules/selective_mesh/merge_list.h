/*
 * merge_list.h - read the container the vendor's own merge loop walks.
 *
 * Selective Mesh has to answer "does this scan take part in the common
 * mesh?" once per scan, from inside the vendor's merge loop, and the
 * question "is any scan left at all?" has to be answered at the FIRST
 * call of the run.  Only the vendor has that scan set - and it is holding
 * it in a register at the very moment it asks (verified in
 * Sn3DMultiProjects, both senders at +0x7E3BB and +0x83097):
 *
 *     lea  rdi, [param_1+0x70]          ; the scan list
 *     ...  QListData::begin / end
 *     mov  rax, [rbx]                   ; item i
 *     mov  rcx, [rax+0x28]              ; HandlePrjFile*  (= self)
 *     call [HandlePrjFile::isGenerateCloud]
 *
 * So the module reads that QList as data.  It must not call Qt to do it:
 * 1.5.0 asked a QVariant what it was and the hub faulted in Qt5Core on
 * every start (docs/research/merge-common-mesh.md).  The header layout is
 * the one this product's Qt 5.15.10 actually uses for a list of
 * pointer-sized elements, MEASURED against the shipped Qt5Core and
 * re-checked by tools\merge_list_lab.c (a QList<QString> is the same
 * container - one 8-byte value per slot):
 *
 *     QListData  ->  d                        (one pointer)
 *     d + 0x00     int  ref                   (Qt's refcount)
 *     d + 0x04     int  alloc                 (slots allocated)
 *     d + 0x08     int  begin                 (first used slot)
 *     d + 0x0C     int  end                   (one past the last)
 *     d + 0x10     the slots; element i of the list is slot (begin + i)
 *
 * so the count is `end - begin` and the used slots start at
 * d + 0x10 + 8*begin: a QList keeps room at both ends and Qt's own
 * begin()/end() walk is [begin, end).  This is NOT the QString/QArrayData
 * layout qt_read.h reads - a string's header holds a size and a data
 * offset, a list's holds begin and end.  The scene reader that is already
 * live in the hub agrees: it found the count at d+0x0C and the slots at
 * d+0x10.
 *
 * Nothing here is a layout guess that only the product could try:
 * tools\merge_list_lab.c builds a REAL QStringList with the shipped
 * Qt5Core's own code, drops fake scan items into its slots and runs these
 * functions on it - including a container whose begin is not 0, and the
 * register scan the module does.
 *
 * Every function is read-only, bounded and SEH-guarded.  Anything that is
 * not the shape it expects yields -1 or 0: never a fault, never a value
 * that could be mistaken for a real scan.
 *
 * Needs <windows.h>, <string.h> and plausiblePtr() (qt_read.h) from the
 * includer.
 */

#ifndef EXSTAR_MERGE_LIST_H
#define EXSTAR_MERGE_LIST_H

/* the scan list inside the merge sender's argument struct */
#define ML_LIST_OFF   0x70
/* the list header, and the slots right behind it */
#define ML_HDR        16
#define ML_REF        0x00
#define ML_ALLOC      0x04
#define ML_BEGIN      0x08
#define ML_END        0x0C
/* a scan list above this is not read: a project that big is not something
 * to walk per call, and "I do not know" is always the safe answer */
#define ML_MAX        4096
/* the item fields the vendor's loop uses (measured, see the disassembly
 * above and the wrapper notes in selective_mesh.c) */
#define ML_ITEM_SELF  0x28
#define ML_ITEM_ID    0x38

/* Reads the QList at `list`.  On success returns the element count and sets
 * *arrOut (the FIRST USED slot) and *allocOut (for the log); any doubt at
 * all gives -1 and untouched outputs. */
static int mlHeader(const void *list, const void *const **arrOut,
                    int *allocOut)
{
    const unsigned char *d = NULL;
    int ref = 0, alloc = 0, begin = 0, end = 0, n;

    if (arrOut)
        *arrOut = NULL;
    if (allocOut)
        *allocOut = -1;
    if (!plausiblePtr(list))
        return -1;
    __try {
        d = *(const unsigned char *const *)list;
        if (d) {
            ref   = *(const int *)(d + ML_REF);
            alloc = *(const int *)(d + ML_ALLOC);
            begin = *(const int *)(d + ML_BEGIN);
            end   = *(const int *)(d + ML_END);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    /* Qt's own invariants.  A wrong pointer shows nonsense here: end before
     * begin, a count no project has, used slots past the block, or a header
     * that is not shared/referenced at all. */
    if (!plausiblePtr(d) || ref < 1)
        return -1;
    n = end - begin;
    if (n < 1 || n > ML_MAX || begin < 0 || alloc < n || alloc > ML_MAX * 2)
        return -1;
    if (begin + n > alloc)
        return -1;
    if (arrOut)
        *arrOut = (const void *const *)(d + ML_HDR + (size_t)begin * 8);
    if (allocOut)
        *allocOut = alloc;
    return n;
}

/* slot index of the item whose `selfOff` field is `self`, or -1.  This is
 * what makes a found list trustworthy: a random register cannot hold the
 * scan the vendor is asking about. */
static int mlFindItem(const void *const *arr, int n, int selfOff,
                      const void *self)
{
    int i;

    if (!plausiblePtr(arr) || n < 1 || n > ML_MAX)
        return -1;
    __try {
        for (i = 0; i < n; i++) {
            const unsigned char *it = (const unsigned char *)arr[i];

            if (plausiblePtr(it)
                && *(const void *const *)(it + selfOff) == self)
                return i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return -1;
}

/* the 16 QUuid bytes at slot `i`'s item + idOff; 0 when the slot, the item
 * or the id is not readable, or the id is all zero (padding, not an id) */
static int mlItemId(const void *const *arr, int i, int idOff,
                    unsigned char *out)
{
    const unsigned char *it = NULL;
    int k, zero = 1;

    if (!plausiblePtr(arr) || i < 0)
        return 0;
    __try {
        it = (const unsigned char *)arr[i];
        if (!plausiblePtr(it))
            return 0;
        memcpy(out, it + idOff, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    for (k = 0; k < 8; k++)
        if (out[k]) { zero = 0; break; }
    return zero ? 0 : 1;
}

#endif /* EXSTAR_MERGE_LIST_H */
