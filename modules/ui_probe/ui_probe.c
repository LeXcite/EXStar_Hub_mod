/*
 * ui_probe.c - DIAGNOSTIC probe v4 (dev only; never embedded, never released).
 *
 * Goal: identify the full-screen grey "curtain" EXStar Hub 2.x (file stamp 3.1.x) paints
 * behind modal popups (loading bars, the memory "Warning", errors) so a
 * future production module can lower just its visual alpha.
 *
 * Lessons that shaped this version:
 *  - v1: NEVER inline-detour hot QQuickItem methods. Hooking
 *    QQuickItem::setVisible made the hub abort with its vendor
 *    "Software error code: 0x00000011" handler.
 *  - v2/v3: QWindow::fromWinId() creates a FRESH wrapper QWindow, it never
 *    returns the app's own window, so the HWND route sees contentItem=NULL
 *    garbage. Removed.
 *  - v3 calibration proved Qt5's QList<T> here is the LEGACY container:
 *      { Data *e; int begin; int end }            (24 bytes total)
 *      Data { int ref; int alloc; int begin; int end; qptrdiff offset; T x[] }
 *    with the data offset at Data+16 (NOT +8). Empty lists point at
 *    QListData::shared_null inside Qt5Core (ref=-1).
 *  - v3 also showed QString objectName() (8 bytes) is returned in RAX, not
 *    through a hidden pointer; the two-qword prototype handed 'this' to the
 *    wrong register, so every name printed empty.
 *
 * v4 is still hook-free and read-only; a private thread touching nothing:
 *   1) QGuiApplication::topLevelWindows() - every real QWindow, class from
 *      MSVC RTTI (vptr -> COL -> TD, pure reads); QQuickWindows get their
 *      contentItem walked as a QQuickItem tree.
 *   2) QApplication::topLevelWidgets() - the QWidget tree; fallback: the
 *      children() of QCoreApplication::instance() (parentless widgets are
 *      parented to the app object). Full-window covering visible widgets
 *      are logged when the covering set changes.
 *   3) visible widgets whose windowHandle() is a QQuickWindow (embedded
 *      QWindowContainer content) get their item tree walked too.
 *
 * Every foreign call is individually SEH-wrapped; a faulting object is
 * skipped, never fatal. Node budgets bound every sweep.
 *
 * Configuration (modules\ui_probe.ini):
 *   min_pct   = 80    cover threshold, percent of window size
 *   poll_ms   = 800   sweep interval
 *   depth     = 6     max walk depth (per tree)
 *   dump_tree = 1     log QWidget tree overviews at sweeps 1/3/10
 *   max_lines = 400   total log lines before going quiet
 *   enabled   = 1     0 parks the thread
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "user32")

#include "exstar_sdk.h"

#define MOD_VERSION "0.4.0"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "ui_probe",
    "UI Probe",
    MOD_VERSION,
    "read-only scan of the Qt widget/window/item scene for full-screen overlays"
};

static const ExHost *g_host;

static int g_min_pct = 80;
static int g_poll_ms = 800;
static int g_depth = 6;
static int g_dump_tree = 1;
static int g_max_lines = 400;
static int g_enabled = 1;

/* ------------------------------------------------------- Qt functions --
 * x64 MSVC ABI notes used below:
 *  - members: this=RCX; >8-byte non-trivial returns take a hidden pointer
 *    in RCX and 'this' slides to RDX (f_list1). <=8-byte non-trivial
 *    returns (QString) come back in RAX (f_qstr).
 *  - statics: first arg RCX (f_list0 = hidden list pointer).
 *  - const-ref returns (children(), geometry()) come back as RAX pointer.
 */
typedef void  *(*f_ptr0)(void);                    /* static -> pointer    */
typedef void  *(*f_ptr1)(const void *self);        /* member -> pointer    */
typedef void   (*f_list0)(void *ret24);            /* static -> QList      */
typedef void   (*f_list1)(void *ret24, const void *self);
typedef double (*f_getd)(const void *self);
typedef int    (*f_geti)(const void *self);
typedef char   (*f_getb)(const void *self);
typedef char   (*f_inherits)(const void *self, const char *cls);
typedef unsigned __int64 (*f_qstr)(const void *self); /* QString in RAX    */
typedef const unsigned short *(*f_u16)(const void *qstr);
typedef void   (*f_dtor)(void *qstr);

/* Qt5Gui */
static f_geti        p_ww, p_wh;
static f_getb        p_winVisible;
static f_geti        p_modality;
static f_list0       p_topWindows;
/* Qt5Widgets */
static f_list0       p_topWidgets;
static f_ptr1        p_parentWidget;
static f_getb        p_wVisible, p_isWindow;
static f_ptr1        p_geometry;          /* -> const QRect& {l,t,w,h}     */
static f_ptr1        p_windowHandle;      /* -> QWindow* or NULL           */
/* Qt5Quick */
typedef void  *(*f_itemfn)(const void *self);
static f_itemfn      p_contentItem;
static f_list1       p_childItems;
static f_getb        p_iVisible;
static f_getd        p_iw, p_ih, p_ix, p_iy, p_opac;
/* Qt5Core */
static f_ptr0        p_appInstance;
static f_inherits    p_inherits;
static f_ptr1        p_childrenRef;        /* -> const QList<QObject*>&    */
static f_qstr        p_objname;
static f_u16         p_unicode;
static f_dtor        p_qdtor;
typedef void      *(*f_parentfn)(const void *self);
static f_parentfn    p_parent;

/* ------------------------------------------------------- module state -- */
static int   g_lines_left;
static int   g_tree_dumps;
static int   g_sweep_no;
static int   g_prev_count = -1;
static LONG64 g_prev_fp;
static int   g_nodes_left;
static int   g_calib_qwin, g_calib_qwid, g_calib_child;

#define MAX_COVERS 48
typedef struct {
    void  *item;
    char   kind;                 /* 'i'=QQuickItem 'w'=QWidget            */
    char   cls[72];
    char   path[96];
    double op, x, y, w, h;
} Cover;
static Cover g_covers[MAX_COVERS];
static int   g_ncover;

/* ------------------------------------------------------ safe helpers -- */
static void plog(const char *fmt, ...)
{
    va_list ap;

    if (g_lines_left <= 0)
        return;
    g_lines_left--;
    va_start(ap, fmt);
    {
        char buf[1024];
        _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
        g_host->log("%s", buf);
    }
    va_end(ap);
}

static void *s_ptr1(f_ptr1 fn, const void *self)
{
    if (!fn || !self)
        return NULL;
    __try {
        return fn(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

static void *s_ptr0(f_ptr0 fn)
{
    if (!fn)
        return NULL;
    __try {
        return fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

static double s_getd(f_getd fn, const void *self)
{
    if (!fn || !self)
        return -1.0;
    __try {
        return fn(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0;
    }
}

static int s_geti(f_geti fn, const void *self)
{
    if (!fn || !self)
        return -1;
    __try {
        return fn(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int s_getb(f_getb fn, const void *self)
{
    if (!fn || !self)
        return 0;
    __try {
        return fn(self) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int s_inherits(const void *self, const char *cls)
{
    if (!p_inherits || !self)
        return 0;
    __try {
        return p_inherits(self, cls) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void s_list0(f_list0 fn, unsigned __int64 *box /*[4]*/)
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!fn)
        return;
    __try {
        fn(box);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        box[0] = box[1] = box[2] = box[3] = 0;
    }
}

static void s_list1(f_list1 fn, unsigned __int64 *box /*[4]*/,
                    const void *self)
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!fn || !self)
        return;
    __try {
        fn(box, self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        box[0] = box[1] = box[2] = box[3] = 0;
    }
}

/*
 * MSVC RTTI: obj -> vptr; vptr[-1] -> CompleteObjectLocator { u32 sig=1,
 * u32 offset, u32 cdOffset, u32 rvaTypeDescriptor, ... } (RVAs against the
 * module owning the vtable); TypeDescriptor { void* spare[2]; char name[] }
 * with name = ".?AVClass@@". Pure reads, everything under SEH.
 */
static int rtti_name(const void *obj, char *out, int cap)
{
    out[0] = 0;
    __try {
        void **vtab = *(void ***)obj;
        unsigned *col;
        HMODULE base = NULL;
        char *td, *nm;
        int i = 0;

        col = (unsigned *)vtab[-1];
        if (col[0] != 1)
            return 0;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)vtab, &base) || !base)
            return 0;
        td = (char *)base + col[3];
        nm = td + 16;
        if (nm[0] != '.' || nm[1] != '?')
            return 0;
        nm += 4;                       /* skip ".?AV" (also ".?AU")        */
        while (i < cap - 1 && nm[i] && nm[i] != '@' && nm[i] != '$' &&
               nm[i] != '.') {
            int c = (unsigned char)nm[i];
            out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            i++;
        }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return 0;
    }
    return out[0] != 0;
}

/* QString objectName(): 8-byte value in RAX; unicode()/~QString() take
 * &value as 'this'. */
static void qname(const void *obj, char *out, int cap)
{
    out[0] = 0;
    if (!obj || !p_objname || !p_unicode)
        return;
    __try {
        unsigned __int64 qs = p_objname(obj);
        const unsigned short *u = p_unicode(&qs);
        int i = 0;

        if (u) {
            for (; i < cap - 1 && u[i]; i++) {
                int c = u[i];
                out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
        }
        out[i] = 0;
        if (p_qdtor)
            p_qdtor(&qs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

static int elem_ok(void *p)
{
    __try {
        void *vtab = *(void **)p;
        return (vtab != NULL && *(void **)vtab != NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/*
 * Decode a Qt5 QList<T> (legacy container proven by v3 calibration):
 *   container: { Data *e; int begin; int end }        24 bytes
 *   Data:      { int ref; int alloc; int begin; int end; qptrdiff offset;
 *                T elems[] }                          elems at e+offset
 * Empty lists reference QListData::shared_null (ref=-1, begin==end).
 * QVector-style fallback (size at Data+4) is also tried. box: 4 qwords.
 * *calib logs the raw header once per call-site tag.
 */
static int read_list(const void *boxv, void **out, int cap, int *calib)
{
    const unsigned __int64 *box = (const unsigned __int64 *)boxv;
    unsigned __int64 e = box[0];
    int begin = (int)(box[1] & 0xFFFFFFFFu);
    int end = (int)(box[1] >> 32);
    const unsigned char *data;
    INT_PTR off;
    int ref, alloc, n, i, k;

    if (!e)
        return 0;
    __try {
        ref = *(const int *)e;
        alloc = *(const int *)(e + 4);
        off = *(const INT_PTR *)(e + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (calib && !*calib) {
        *calib = 1;
        plog("uiprobe: QList e=%p ref=%d alloc=%d begin=%d end=%d off=%lld",
             (void *)e, ref, alloc, begin, end, (long long)off);
    }
    if (ref < 0 && begin == 0 && end == 0)   /* shared_null / empty */
        return 0;
    if (off <= 0 || off > 0x10000)
        return 0;
    data = (const unsigned char *)e + off;

    n = 0;
    __try {
        /* legacy QList: elements [begin, end) */
        if (begin >= 0 && end > begin && end - begin <= cap &&
            (alloc == 0 || end <= alloc)) {
            k = 0;
            for (i = begin; i < end && k < 3; i++, k++)
                if (!elem_ok(*(void **)(data + (SIZE_T)i * 8)))
                    break;
            if (k >= (end - begin < 3 ? end - begin : 3)) {
                for (i = begin; i < end; i++)
                    out[i - begin] = *(void **)(data + (SIZE_T)i * 8);
                n = end - begin;
            }
        }
        /* QVector fallback: size at Data+4 */
        if (n == 0 && alloc > 0 && alloc <= cap && alloc < 4096) {
            k = 0;
            for (i = 0; i < alloc && k < 3; i++, k++)
                if (!elem_ok(*(void **)(data + (SIZE_T)i * 8)))
                    break;
            if (k >= (alloc < 3 ? alloc : 3)) {
                for (i = 0; i < alloc; i++)
                    out[i] = *(void **)(data + (SIZE_T)i * 8);
                n = alloc;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

static void build_parents(const void *item, char *out, int cap)
{
    const void *cur;
    int used = 0, depth = 0;

    out[0] = 0;
    cur = p_parent ? s_ptr1(p_parent, item) : NULL;
    while (cur && depth++ < 8) {
        char nm[64], cl[72];

        nm[0] = 0;
        qname(cur, nm, sizeof nm);
        if (!rtti_name(cur, cl, sizeof cl))
            strcpy_s(cl, sizeof cl, "?");
        {
            int k = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                                used ? ">%s" : "%s", nm[0] ? nm : cl);
            if (k < 0 || used + k >= cap - 2)
                break;
            used += k;
        }
        cur = s_ptr1(p_parent, cur);
    }
}

/* ------------------------------------------------------- item (QML) ---- */
static void walk_item(const void *item, double px, double py, int depth,
                      const char *path, double rw, double rh, char kind)
{
    void *kids[64];
    unsigned __int64 box[4];
    double x, y, w, h, op;
    int n, i;

    if (!item || g_nodes_left <= 0 || depth > g_depth)
        return;
    g_nodes_left--;

    if (!s_getb(p_iVisible, item))
        return;

    x = s_getd(p_ix, item);
    y = s_getd(p_iy, item);
    w = s_getd(p_iw, item);
    h = s_getd(p_ih, item);
    op = s_getd(p_opac, item);
    if (w < 0 || h < 0)
        return;
    x += px;
    y += py;

    if (depth > 0 && rw > 0 && rh > 0 &&
        w >= rw * g_min_pct / 100.0 && h >= rh * g_min_pct / 100.0 &&
        x <= 4.0 && y <= 4.0 && op > 0.002 && g_ncover < MAX_COVERS) {
        Cover *c = &g_covers[g_ncover++];
        c->item = (void *)item;
        c->kind = kind;
        if (!rtti_name(item, c->cls, sizeof c->cls))
            strcpy_s(c->cls, sizeof c->cls, "?");
        c->op = op; c->x = x; c->y = y; c->w = w; c->h = h;
        strncpy(c->path, path, sizeof c->path - 1);
        c->path[sizeof c->path - 1] = 0;
    }

    s_list1(p_childItems, box, item);
    n = read_list(box, kids, 64, NULL);
    for (i = 0; i < n; i++) {
        char cp[96];
        _snprintf_s(cp, sizeof cp, _TRUNCATE, "%sc[%d]", path, i);
        walk_item(kids[i], x, y, depth + 1, cp, rw, rh, kind);
    }
}

/* ---------------------------------------------------------- widgets ---- */
static void widget_abs(const void *w, int *ax, int *ay)
{
    const void *cur = w;
    int x = 0, y = 0, hop = 0;

    while (cur && hop++ < 14) {
        const int *r;
        if (s_getb(p_isWindow, cur)) {
            *ax = x; *ay = y;          /* window geometry is global enough */
            return;
        }
        if (!s_inherits(cur, "QWidget"))
            break;
        r = (const int *)s_ptr1(p_geometry, cur);
        if (!r)
            break;
        x += r[0];
        y += r[1];
        cur = s_ptr1(p_parentWidget, cur);
    }
    *ax = x;
    *ay = y;
}

static void walk_widget(const void *w, int depth, const char *path,
                        int ww, int wh, int overview)
{
    void *kids[64];
    const void *listp;
    unsigned __int64 box[4];
    int n = 0, i;

    if (!w || g_nodes_left <= 0 || depth > g_depth)
        return;
    g_nodes_left--;

    if (s_inherits(w, "QWidget")) {
        const int *r = (const int *)s_ptr1(p_geometry, w);
        int vis = s_getb(p_wVisible, w);

        if (r) {
            if (overview && g_lines_left > 40) {
                char nm[64], cls[72];
                int x = 0, y = 0;

                qname(w, nm, sizeof nm);
                if (!rtti_name(w, cls, sizeof cls))
                    strcpy_s(cls, sizeof cls, "?");
                widget_abs(w, &x, &y);
                plog("uiprobe: WID d%d '%s' name='%s' geo=%d,%d %dx%d vis=%d",
                     depth, cls, nm, x, y, r[2], r[3], vis);
            }
            if (!overview && vis && ww > 0 && wh > 0 &&
                r[2] >= ww * g_min_pct / 100.0 &&
                r[3] >= wh * g_min_pct / 100.0 &&
                g_ncover < MAX_COVERS) {
                Cover *c = &g_covers[g_ncover++];
                int x = 0, y = 0;

                c->item = (void *)w;
                c->kind = 'w';
                if (!rtti_name(w, c->cls, sizeof c->cls))
                    strcpy_s(c->cls, sizeof c->cls, "?");
                c->op = 1.0;
                widget_abs(w, &x, &y);
                c->x = x; c->y = y;
                c->w = r[2]; c->h = r[3];
                strncpy(c->path, path, sizeof c->path - 1);
                c->path[sizeof c->path - 1] = 0;
            }
            /* embedded QML surface (QWindowContainer style hosting) */
            if (vis && p_windowHandle && depth >= 2) {
                void *wh2 = s_ptr1(p_windowHandle, w);
                if (wh2 && s_inherits(wh2, "QQuickWindow")) {
                    void *root = s_ptr1(p_contentItem, wh2);
                    double rw = s_getd(p_iw, root);
                    double rh = s_getd(p_ih, root);
                    if (rw > 100 && rh > 100)
                        walk_item(root, 0, 0, 2, path, rw, rh, 'i');
                }
            }
        }
    }

    /* children(): const QList<QObject*>& - reference in place (24 bytes) */
    listp = s_ptr1(p_childrenRef, w);
    if (listp) {
        box[0] = ((const unsigned __int64 *)listp)[0];
        box[1] = ((const unsigned __int64 *)listp)[1];
        n = read_list(box, kids, 64, &g_calib_child);
    }
    for (i = 0; i < n; i++) {
        char cp[96];

        _snprintf_s(cp, sizeof cp, _TRUNCATE, "%sc%d.", path, i);
        walk_widget(kids[i], depth + 1, cp, ww, wh, overview);
    }
}

/* --------------------------------------------------------- sweep ------ */
static void note_window(void *win)
{
    static void *seen[32];
    static int seen_n;
    char cls[72] = "?";
    int i;

    for (i = 0; i < seen_n; i++)
        if (seen[i] == win)
            return;
    if (seen_n < (int)(sizeof seen / sizeof seen[0]))
        seen[seen_n++] = win;
    if (!rtti_name(win, cls, sizeof cls))
        strcpy_s(cls, sizeof cls, "?");
    plog("uiprobe: QWIN[%d] win=%p class='%s' %dx%d vis=%d modal=%d "
         "content=%p",
         seen_n, win, cls,
         s_geti(p_ww, win), s_geti(p_wh, win),
         s_getb(p_winVisible, win), s_geti(p_modality, win),
         s_inherits(win, "QQuickWindow") ? s_ptr1(p_contentItem, win)
                                         : NULL);
}

static void emit_snapshot(void)
{
    LONG64 fp = 1469598103934665603ULL;
    int k;

    for (k = 0; k < g_ncover; k++) {
        fp = (fp ^ (LONG64)(UINT_PTR)g_covers[k].item) * 1099511628211ULL;
        fp = (fp ^ (LONG64)(g_covers[k].op * 50.0)) * 1099511628211ULL;
        fp = (fp ^ (LONG64)(g_covers[k].w)) * 1099511628211ULL;
    }
    fp = (fp ^ g_ncover) * 1099511628211ULL;

    if (g_prev_count >= 0 && g_ncover == g_prev_count && fp == g_prev_fp)
        return;

    plog("uiprobe: SNAP covers: %d -> %d",
         g_prev_count < 0 ? -1 : g_prev_count, g_ncover);
    for (k = 0; k < g_ncover; k++) {
        char nm[96], par[256];

        qname(g_covers[k].item, nm, sizeof nm);
        build_parents(g_covers[k].item, par, sizeof par);
        plog("uiprobe: COVER[%c] item=%p class='%s' name='%s' op=%.2f "
             "pos=%.0f,%.0f size=%.0fx%.0f path='%s' par='%s'",
             g_covers[k].kind, g_covers[k].item, g_covers[k].cls, nm,
             g_covers[k].op, g_covers[k].x, g_covers[k].y,
             g_covers[k].w, g_covers[k].h, g_covers[k].path, par);
    }
    g_prev_count = g_ncover;
    g_prev_fp = fp;
}

static void sweep(void)
{
    unsigned __int64 box[4];
    void *objs[24];
    int n, i, tree_now;

    g_sweep_no++;
    tree_now = g_dump_tree && g_tree_dumps < 3 &&
               (g_sweep_no == 1 || g_sweep_no == 3 || g_sweep_no == 10);

    g_ncover = 0;
    g_nodes_left = 2500;

    /* 1) every real QWindow in the process (topLevelWindows) */
    s_list0(p_topWindows, box);
    n = read_list(box, objs, 24, &g_calib_qwin);
    for (i = 0; i < n && i < 24; i++) {
        void *root = NULL;
        double rw = -1, rh = -1;

        note_window(objs[i]);
        if (!s_inherits(objs[i], "QQuickWindow"))
            continue;
        root = s_ptr1(p_contentItem, objs[i]);
        rw = s_getd(p_iw, root);
        rh = s_getd(p_ih, root);
        if (rw > 100 && rh > 100)
            walk_item(root, 0, 0, 1, "w", rw, rh, 'i');
    }

    /* 2) the QWidget tree */
    s_list0(p_topWidgets, box);
    n = read_list(box, objs, 24, &g_calib_qwid);
    if (n == 0 && p_appInstance) {
        /* fallback: parentless widgets are children of the app object */
        const void *app = s_ptr0(p_appInstance);
        const void *listp = app ? s_ptr1(p_childrenRef, app) : NULL;

        if (listp) {
            box[0] = ((const unsigned __int64 *)listp)[0];
            box[1] = ((const unsigned __int64 *)listp)[1];
            n = read_list(box, objs, 24, NULL);
            /* keep only top-level QWidgets */
            for (i = 0, g_nodes_left = 2500; i < n;) {
                if (!s_inherits(objs[i], "QWidget") ||
                    !s_getb(p_isWindow, objs[i])) {
                    objs[i] = objs[--n];
                    continue;
                }
                i++;
            }
            if (g_sweep_no == 1)
                plog("uiprobe: topWidgets empty, app-children fallback: %d "
                     "top-level widgets", n);
        }
    }
    for (i = 0; i < n && i < 24; i++) {
        const int *r;
        int ww, wh;

        if (!s_inherits(objs[i], "QWidget"))
            continue;
        r = (const int *)s_ptr1(p_geometry, objs[i]);
        ww = r ? r[2] : 0;
        wh = r ? r[3] : 0;
        if (tree_now && ww > 600 && wh > 400) {
            plog("uiprobe: WIDGET TREE #%d of top-level %dx%d (sweep %d):",
                 g_tree_dumps + 1, ww, wh, g_sweep_no);
            g_tree_dumps++;
            walk_widget(objs[i], 0, "", ww, wh, 1);
            plog("uiprobe: (tree end; nodes left %d)", g_nodes_left);
            g_nodes_left = 2500;
        }
        walk_widget(objs[i], 0, "", ww, wh, 0);
    }

    emit_snapshot();
}

/* --------------------------------------------------------- poller ----- */
static HMODULE wait_module(const wchar_t *name)
{
    HMODULE h;
    int i;

    for (i = 0; i < 1800; i++) {
        h = GetModuleHandleW(name);
        if (h)
            return h;
        Sleep(50);
    }
    return NULL;
}

static unsigned WINAPI poller(LPVOID arg)
{
    HMODULE quick, core, gui, widgets;

    (void)arg;
    quick   = wait_module(L"Qt5Quick.dll");
    core    = wait_module(L"Qt5Core.dll");
    gui     = wait_module(L"Qt5Gui.dll");
    widgets = wait_module(L"Qt5Widgets.dll");
    if (!quick || !core || !gui || !widgets) {
        g_host->log("uiprobe: Qt modules not found (q=%p c=%p g=%p w=%p)",
                    (void *)quick, (void *)core, (void *)gui,
                    (void *)widgets);
        return 0;
    }

    p_ww           = (f_geti)      GetProcAddress(gui,     "?width@QWindow@@QEBAHXZ");
    p_wh           = (f_geti)      GetProcAddress(gui,     "?height@QWindow@@QEBAHXZ");
    p_winVisible   = (f_getb)      GetProcAddress(gui,     "?isVisible@QWindow@@QEBA_NXZ");
    p_modality     = (f_geti)      GetProcAddress(gui,     "?modality@QWindow@@QEBA?AW4WindowModality@Qt@@XZ");
    p_topWindows   = (f_list0)     GetProcAddress(gui,     "?topLevelWindows@QGuiApplication@@SA?AV?$QList@PEAVQWindow@@@@XZ");
    p_topWidgets   = (f_list0)     GetProcAddress(widgets, "?topLevelWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
    p_parentWidget = (f_ptr1)      GetProcAddress(widgets, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
    p_wVisible     = (f_getb)      GetProcAddress(widgets, "?isVisible@QWidget@@QEBA_NXZ");
    p_isWindow     = (f_getb)      GetProcAddress(widgets, "?isWindow@QWidget@@QEBA_NXZ");
    p_geometry     = (f_ptr1)      GetProcAddress(widgets, "?geometry@QWidget@@QEBAAEBVQRect@@XZ");
    p_windowHandle = (f_ptr1)      GetProcAddress(widgets, "?windowHandle@QWidget@@QEBAPEAVQWindow@@XZ");
    p_contentItem  = (f_itemfn)    GetProcAddress(quick,   "?contentItem@QQuickWindow@@QEBAPEAVQQuickItem@@XZ");
    p_childItems   = (f_list1)     GetProcAddress(quick,   "?childItems@QQuickItem@@QEBA?AV?$QList@PEAVQQuickItem@@@@XZ");
    p_iVisible     = (f_getb)      GetProcAddress(quick,   "?isVisible@QQuickItem@@QEBA_NXZ");
    p_iw           = (f_getd)      GetProcAddress(quick,   "?width@QQuickItem@@QEBANXZ");
    p_ih           = (f_getd)      GetProcAddress(quick,   "?height@QQuickItem@@QEBANXZ");
    p_ix           = (f_getd)      GetProcAddress(quick,   "?x@QQuickItem@@QEBANXZ");
    p_iy           = (f_getd)      GetProcAddress(quick,   "?y@QQuickItem@@QEBANXZ");
    p_opac         = (f_getd)      GetProcAddress(quick,   "?opacity@QQuickItem@@QEBANXZ");
    p_appInstance  = (f_ptr0)      GetProcAddress(core,    "?instance@QCoreApplication@@SAPEAV1@XZ");
    p_inherits     = (f_inherits)  GetProcAddress(core,    "?inherits@QObject@@QEBA_NPEBD@Z");
    p_childrenRef  = (f_ptr1)      GetProcAddress(core,    "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
    p_objname      = (f_qstr)      GetProcAddress(core,    "?objectName@QObject@@QEBA?AVQString@@XZ");
    p_unicode      = (f_u16)       GetProcAddress(core,    "?unicode@QString@@QEBAPEBVQChar@@XZ");
    p_qdtor        = (f_dtor)      GetProcAddress(core,    "??1QString@@QEAA@XZ");
    p_parent       = (f_parentfn)  GetProcAddress(core,    "?parent@QObject@@QEBAPEAV1@XZ");

    g_host->log("uiprobe v%s armed: topWin=%p topWid=%p children=%p geom=%p "
                "winHandle=%p objname=%p inherits=%p rtti=1",
                MOD_VERSION, (void *)p_topWindows, (void *)p_topWidgets,
                (void *)p_childrenRef, (void *)p_geometry,
                (void *)p_windowHandle, (void *)p_objname,
                (void *)p_inherits);
    if (!p_topWidgets && !p_topWindows) {
        g_host->log("uiprobe: nothing to walk, going idle");
        return 0;
    }

    while (g_lines_left > 0) {
        Sleep((DWORD)g_poll_ms);
        if (!g_enabled)
            continue;
        __try {
            sweep();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            static int once;
            if (!once) {
                once = 1;
                g_host->log("uiprobe: sweep faulted (app unaffected)");
            }
        }
    }
    g_host->log("uiprobe: log budget used, going quiet");
    return 0;
}

/* ---------------------------------------------------- entry points ---- */
EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    HANDLE th;
    unsigned tid;

    g_host = host;
    g_min_pct = host->cfg_int(host, "min_pct", 80);
    if (g_min_pct < 10)  g_min_pct = 10;
    if (g_min_pct > 100) g_min_pct = 100;
    g_poll_ms = host->cfg_int(host, "poll_ms", 800);
    if (g_poll_ms < 200) g_poll_ms = 200;
    g_depth = host->cfg_int(host, "depth", 6);
    if (g_depth < 1)  g_depth = 1;
    if (g_depth > 12) g_depth = 12;
    g_dump_tree = host->cfg_int(host, "dump_tree", 1);
    g_max_lines = host->cfg_int(host, "max_lines", 400);
    g_enabled = host->cfg_int(host, "enabled", 1);
    g_lines_left = g_max_lines;

    th = CreateThread(NULL, 0, poller, NULL, 0, &tid);
    if (!th)
        return 2;
    CloseHandle(th);
    host->log("uiprobe v%s: module start (read-only dev diagnostic)",
              MOD_VERSION);
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
}
