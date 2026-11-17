/*
 * probe_ext.c - standalone READ-ONLY diagnostic probe (dev tool, never shipped).
 *
 * Loaded into the running EXStar Hub with CreateRemoteThread + LoadLibraryW
 * straight from a temp directory: no vendor file is touched, nothing is
 * detoured, and FreeLibrary unloads it cleanly. It only ever calls const Qt
 * accessors from its own thread (the v1 lesson still stands: never inline-
 * hook hot Qt methods - that made the hub abort with error code 0x00000011).
 *
 * Why a fresh probe instead of modules\ui_probe: the vendor install dir is
 * write-protected, so swapping a module file requires closing the hub and an
 * elevated copy. This thread loads from temp and needs neither.
 *
 * Decoding fixes proven by the ui_probe v4 calibration:
 *   - QList<T> here is Qt5's LEGACY container: { Data *e; int i }, with the
 *     elements stored in BLOCKS behind Data, so a hand-rolled linear read
 *     always failed. Qt decodes it for us via two exported const members,
 *     QListData::size() and QListData::at(int) - 'this' is the container
 *     address, and QList<T> embeds QListData at offset 0.
 *   - QString objectName() (8 bytes) is returned in RAX, not through a
 *     hidden pointer; unicode()/~QString() then take &value as 'this'.
 *
 * Command file (probe_ext.cfg) drives experiments without re-injecting:
 *   patch <module-substring> <ascii pattern> <replacement>   ; \NN = hex byte
 *   dump_now
 *   stop
 * "patch" edits bytes in the process-private MAPPED IMAGE copy (VirtualProtect
 * + write, copy-on-write pages): the DLL file on disk is never modified - the
 * same mechanism popup_dim uses live.
 *
 * Log: %TEMP%\probe_ext.log (append)
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* scratch files live under %TEMP% (dev tool, never shipped) */
static void tmpPath(const char *name, char *out, size_t n)
{
    DWORD k = GetTempPathA((DWORD)n, out);
    if (k == 0 || k >= n - 1)
        out[0] = 0;
    strncat_s(out, n, name, _TRUNCATE);
}

#define PROBE_VERSION "0.9.0"

static HMODULE          g_self;
static FILE            *g_log;
static CRITICAL_SECTION g_loglock;
static volatile LONG    g_stop;

static void plog(const char *fmt, ...)
{
    va_list ap;
    char ts[32], buf[1024];
    SYSTEMTIME st;

    GetLocalTime(&st);
    _snprintf_s(ts, sizeof ts, _TRUNCATE, "%02d:%02d:%02d",
                st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_loglock);
    if (g_log) {
        fprintf(g_log, "%s %s\n", ts, buf);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_loglock);
}

/* -------------------------------------------------------- Qt handles -- */
typedef void               *(*f_ptr0)(void);
typedef void               *(*f_ptr1)(const void *self);
typedef const void          *(*f_metafn)(const void *self);
typedef const char          *(*f_clsname)(const void *meta);
typedef void                (*f_list0)(void *ret16);
typedef void                (*f_list1)(void *ret16, const void *self);
typedef double              (*f_getd)(const void *self);
typedef int                 (*f_geti)(const void *self);
typedef char                (*f_getb)(const void *self);
typedef char                (*f_inherits)(const void *self, const char *cls);
typedef unsigned __int64    (*f_qstr)(const void *self);      /* RAX       */
typedef const unsigned short *(*f_u16)(const void *qstr);
typedef void                (*f_dtor)(void *qstr);
typedef void               *(*f_ldat)(const void *container, int index);
typedef void                (*f_listfn)(void *ret16);         /* static    */

static f_geti     p_ww, p_wh, p_modality, p_ldSize;
static f_metafn   p_metaObject;
static f_clsname  p_className;
static f_getb     p_winVisible, p_isTopLevel;
static f_ptr1     p_transientParent;
static f_listfn   p_topWindows, p_allWindows;
static f_listfn   p_topWidgets;
static f_ptr1     p_parentWidget, p_geometry, p_windowHandle;
static f_getb     p_wVisible, p_isWindow;
static f_ptr1     p_contentItem;
static f_list1    p_childItems;
static f_getb     p_iVisible;
static f_getd     p_iw, p_ih, p_ix, p_iy, p_opac;
static f_ptr0     p_appInstance;
static f_inherits p_inherits;
static f_ptr1     p_childrenRef;
static f_qstr     p_objname;
static f_u16      p_unicode;
static f_dtor     p_qdtor;
static f_ptr1     p_parent;
static f_ldat     p_ldAt;
static f_ptr1     p_itemWindow;   /* QQuickItem::window()                 */

/* --------------------------------------------------------- safe calls -- */
static void *s_ptr1(f_ptr1 fn, const void *self)
{
    if (!fn || !self) return NULL;
    __try { return fn(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *s_ptr0(f_ptr0 fn)
{
    if (!fn) return NULL;
    __try { return fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static double s_getd(f_getd fn, const void *self)
{
    if (!fn || !self) return -1.0;
    __try { return fn(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1.0; }
}
static int s_geti(f_geti fn, const void *self)
{
    if (!fn || !self) return -1;
    __try { return fn(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int s_getb(f_getb fn, const void *self)
{
    if (!fn || !self) return 0;
    __try { return fn(self) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int s_inherits(const void *self, const char *cls)
{
    if (!p_inherits || !self) return 0;
    __try { return p_inherits(self, cls) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void s_list(f_listfn fn, unsigned __int64 *box)
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!fn) return;
    __try { fn(box); }
    __except (EXCEPTION_EXECUTE_HANDLER) { box[0] = box[1] = 0; }
}
static void s_list1(f_list1 fn, unsigned __int64 *box, const void *self)
{
    box[0] = box[1] = box[2] = box[3] = 0;
    if (!fn || !self) return;
    __try { fn(box, self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { box[0] = box[1] = 0; }
}
static int s_ldSize(const void *box)
{
    if (!p_ldSize || !box) return 0;
    __try { return p_ldSize(box); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void *s_ldAt(const void *box, int idx)
{
    void **pp, *el = NULL;
    if (!p_ldAt || !box || idx < 0) return NULL;
    __try { pp = (void **)p_ldAt(box, idx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    if (!pp) return NULL;
    __try { el = *pp; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return el;
}

/* ------------------------------------------------------------ utils --- */
/* metaObject()->className() first (QML types carry useful names there),
 * MSVC RTTI as fallback. Both read-only and SEH-guarded. */
static int rtti_name(const void *obj, char *out, int cap);

static int class_name(const void *obj, char *out, int cap)
{
    out[0] = 0;
    if (obj && p_metaObject && p_className) {
        __try {
            const void *mo = p_metaObject(obj);
            const char *cn = mo ? p_className(mo) : NULL;
            int i = 0;
            if (cn) {
                for (; i < cap - 1 && cn[i]; i++) {
                    int c = (unsigned char)cn[i];
                    out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
                }
                out[i] = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
        if (out[0])
            return 1;
    }
    return rtti_name(obj, out, cap);
}

static int elem_ok(void *p)
{
    if (!p) return 0;
    __try {
        void *vtab = *(void **)p;
        return vtab != NULL && *(void **)vtab != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int rtti_name(const void *obj, char *out, int cap)
{
    out[0] = 0;
    if (!obj) return 0;
    __try {
        void **vtab = *(void ***)obj;
        unsigned *col;
        HMODULE base = NULL;
        char *td, *nm;
        int i = 0;

        col = (unsigned *)vtab[-1];
        if (col[0] != 1) return 0;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)vtab, &base) || !base)
            return 0;
        td = (char *)base + col[3];
        nm = td + 16;
        if (nm[0] != '.' || nm[1] != '?') return 0;
        nm += 4;                                   /* ".?AV" / ".?AU"       */
        while (i < cap - 1 && nm[i] && nm[i] != '@' && nm[i] != '$' &&
               nm[i] != '.') {
            int c = (unsigned char)nm[i];
            out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            i++;
        }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return 0; }
    return out[0] != 0;
}

static void qname(const void *obj, char *out, int cap)
{
    out[0] = 0;
    if (!obj || !p_objname || !p_unicode) return;
    __try {
        unsigned __int64 qs = p_objname(obj);
        const unsigned short *u = p_unicode(&qs);
        int i = 0;
        if (u)
            for (; i < cap - 1 && u[i]; i++) {
                int c = u[i];
                out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
            }
        out[i] = 0;
        if (p_qdtor) p_qdtor(&qs);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

/* QList<T> via QListData::size()/at() - legacy blocks decoded by Qt itself */
static int read_list(const void *box, void **out, int cap)
{
    int n, i;

    if (!box || !*(const unsigned __int64 *)box) return 0;
    n = s_ldSize(box);
    if (n <= 0) return 0;
    if (n > cap) n = cap;
    for (i = 0; i < n; i++) {
        out[i] = s_ldAt(box, i);
        if (!elem_ok(out[i])) return i;
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
        int k;

        nm[0] = 0;
        qname(cur, nm, sizeof nm);
        if (!class_name(cur, cl, sizeof cl)) strcpy_s(cl, sizeof cl, "?");
        k = _snprintf_s(out + used, (size_t)(cap - used), _TRUNCATE,
                        used ? ">%s" : "%s", nm[0] ? nm : cl);
        if (k < 0 || used + k >= cap - 2) break;
        used += k;
        cur = s_ptr1(p_parent, cur);
    }
}

/* ------------------------------------------------------------- state -- */
#define MAX_COVERS 64
typedef struct {
    void  *item;
    char   kind;                    /* 'i' item, 'w' widget               */
    char   cls[72];
    char   path[112];
    double op, x, y, w, h;
} Cover;

static Cover  g_covers[MAX_COVERS];
static int    g_ncover;
static int    g_nodes_left;
static int    g_dump_items;
static LONG64 g_prev_fp = -1;
static int    g_prev_count = -1;
static double g_min_pct = 80.0;

/* ------------------------------------------------------- walk: items -- */
static int g_child_dbg = 6;    /* number of "childItems empty" notes     */

static void walk_item(const void *item, double px, double py, int depth,
                      const char *path, double rw, double rh, int overview)
{
    void *kids[96];
    void *u[96];
    const void *listp;
    unsigned __int64 box[4];
    double x, y, w, h, op;
    int n = 0, nu = 0, i, j, dup;

    if (!item || g_nodes_left <= 0 || depth > 10) return;
    g_nodes_left--;
    if (!s_getb(p_iVisible, item)) return;

    x  = s_getd(p_ix, item);
    y  = s_getd(p_iy, item);
    w  = s_getd(p_iw, item);
    h  = s_getd(p_ih, item);
    op = s_getd(p_opac, item);
    if (w < 0 || h < 0) return;
    x += px;
    y += py;

    if (overview && g_dump_items > 0 && depth <= 8) {
        char nm[64], cls[72];
        qname(item, nm, sizeof nm);
        if (!class_name(item, cls, sizeof cls)) strcpy_s(cls, sizeof cls, "?");
        g_dump_items--;
        if (depth <= 2)
            plog("ITEM d%d %-28s '%s' %.0fx%.0f@%.0f,%.0f op=%.2f win=%p",
                 depth, cls, nm, w, h, x, y, op,
                 p_itemWindow ? s_ptr1(p_itemWindow, item) : NULL);
        else
            plog("ITEM d%d %-28s '%s' %.0fx%.0f@%.0f,%.0f op=%.2f",
                 depth, cls, nm, w, h, x, y, op);
    }

    if (rw > 0 && rh > 0 &&
        w >= rw * g_min_pct / 100.0 && h >= rh * g_min_pct / 100.0 &&
        x <= 4.0 && y <= 4.0 && op > 0.002 && g_ncover < MAX_COVERS) {
        Cover *c = &g_covers[g_ncover++];
        c->item = (void *)item;
        c->kind = 'i';
        if (!class_name(item, c->cls, sizeof c->cls))
            strcpy_s(c->cls, sizeof c->cls, "?");
        c->op = op; c->x = x; c->y = y; c->w = w; c->h = h;
        strncpy(c->path, path, sizeof c->path - 1);
        c->path[sizeof c->path - 1] = 0;
    }

    /* union of childItems() and QObject::children() (QQuickItem only):
     * childItems() is a sorted proxy view; children() is the raw object
     * tree. If one route comes back empty the other usually still works. */
    s_list1(p_childItems, box, item);
    n = read_list(box, kids, 48);
    if (n == 0 && depth <= 2 && g_child_dbg > 0) {
        g_child_dbg--;
        plog("DBG childItems empty d%d item=%p box.e=%p size=%d",
             depth, item, (void *)box[0], s_ldSize(box));
    }
    for (i = 0; i < n && nu < 96; i++)
        u[nu++] = kids[i];

    listp = s_ptr1(p_childrenRef, item);
    if (listp) {
        n = read_list(listp, kids, 48);
        for (i = 0; i < n; i++) {
            if (!s_inherits(kids[i], "QQuickItem"))
                continue;
            dup = 0;
            for (j = 0; j < nu; j++)
                if (u[j] == kids[i]) { dup = 1; break; }
            if (!dup && nu < 96)
                u[nu++] = kids[i];
        }
    }

    for (i = 0; i < nu; i++) {
        char cp[112];
        _snprintf_s(cp, sizeof cp, _TRUNCATE, "%s%d.", path, i);
        walk_item(u[i], x, y, depth + 1, cp, rw, rh, overview);
    }
}

/* ----------------------------------------------------- walk: widgets -- */
static void walk_widget(const void *w, int depth, const char *path,
                        int ww, int wh, int overview)
{
    void *kids[64];
    const void *listp;
    unsigned __int64 box[4];
    int n = 0, i;

    if (!w || g_nodes_left <= 0 || depth > 10) return;
    g_nodes_left--;

    if (s_inherits(w, "QWidget")) {
        const int *r  = (const int *)s_ptr1(p_geometry, w);
        int vis = s_getb(p_wVisible, w);

        if (r && overview && g_dump_items > 0) {
            char nm[64], cls[72];
            qname(w, nm, sizeof nm);
            if (!class_name(w, cls, sizeof cls)) strcpy_s(cls, sizeof cls, "?");
            g_dump_items--;
            plog("WID d%d %-28s '%s' geo=%d,%d %dx%d vis=%d",
                 depth, cls, nm, r[0], r[1], r[2], r[3], vis);
        }
        if (r && !overview && vis && ww > 0 && wh > 0 &&
            r[2] >= ww * g_min_pct / 100.0 && r[3] >= wh * g_min_pct / 100.0 &&
            g_ncover < MAX_COVERS) {
            Cover *c = &g_covers[g_ncover++];
            c->item = (void *)w;
            c->kind = 'w';
            if (!class_name(w, c->cls, sizeof c->cls))
                strcpy_s(c->cls, sizeof c->cls, "?");
            c->op = 1.0; c->x = r[0]; c->y = r[1]; c->w = r[2]; c->h = r[3];
            strncpy(c->path, path, sizeof c->path - 1);
            c->path[sizeof c->path - 1] = 0;
        }
        /* a widget hosting its own QQuickWindow (QWindowContainer) */
        if (vis && p_windowHandle && depth >= 1 && depth <= 6) {
            void *sub = s_ptr1(p_windowHandle, w);
            if (sub && sub != w && s_inherits(sub, "QQuickWindow")) {
                void *root = s_ptr1(p_contentItem, sub);
                double rw = s_getd(p_iw, root), rh = s_getd(p_ih, root);
                if (rw > 100 && rh > 100)
                    walk_item(root, 0, 0, depth + 1, path, rw, rh, overview);
            }
        }
    }

    listp = s_ptr1(p_childrenRef, w);
    if (listp)
        n = read_list(listp, kids, 64);
    for (i = 0; i < n; i++) {
        char cp[112];
        _snprintf_s(cp, sizeof cp, _TRUNCATE, "%s%d.", path, i);
        walk_widget(kids[i], depth + 1, cp, ww, wh, overview);
    }
}

/* ---------------------------------------------------------- reporting -- */
static void note_window(void *win, const char *src)
{
    static void *seen[48];
    static int seen_n;
    char cls[72] = "?";
    int i;

    for (i = 0; i < seen_n; i++)
        if (seen[i] == win) return;
    if (seen_n < (int)(sizeof seen / sizeof seen[0])) seen[seen_n++] = win;
    if (!class_name(win, cls, sizeof cls)) strcpy_s(cls, sizeof cls, "?");
    plog("QWIN<%s> %p %-22s %dx%d vis=%d top=%d modal=%d trans=%p content=%p",
         src, win, cls, s_geti(p_ww, win), s_geti(p_wh, win),
         s_getb(p_winVisible, win), s_getb(p_isTopLevel, win),
         s_geti(p_modality, win), s_ptr1(p_transientParent, win),
         s_inherits(win, "QQuickWindow") ? s_ptr1(p_contentItem, win) : NULL);
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

    plog("SNAP covers %d -> %d", g_prev_count < 0 ? -1 : g_prev_count, g_ncover);
    for (k = 0; k < g_ncover; k++) {
        char nm[96], par[288];
        qname(g_covers[k].item, nm, sizeof nm);
        build_parents(g_covers[k].item, par, sizeof par);
        plog("COVER[%c] %p %-26s name='%s' op=%.2f pos=%.0f,%.0f %.0fx%.0f "
             "path='%s' par='%s'",
             g_covers[k].kind, g_covers[k].item, g_covers[k].cls, nm,
             g_covers[k].op, g_covers[k].x, g_covers[k].y,
             g_covers[k].w, g_covers[k].h, g_covers[k].path, par);
    }
    g_prev_count = g_ncover;
    g_prev_fp = fp;
}

/* ------------------------------------------------------------- sweep -- */
static int g_dump_now;

static void sweep(int overview)
{
    unsigned __int64 box[4];
    void *objs[40];
    int n, i;

    g_ncover = 0;
    g_nodes_left = 6000;
    if (overview) g_dump_items = 420;

    if (p_allWindows) {
        s_list(p_allWindows, box);
        n = read_list(box, objs, 40);
        for (i = 0; i < n; i++)
            note_window(objs[i], "all");
        if (overview) {
            /* walk every QWindow's scene, incl. the hidden modal dialog
             * host views; QObject::inherits() misfires for objects whose
             * meta-object lives in another Qt copy, so call contentItem()
             * directly (SEH-guarded) and sanity-check the result */
            for (i = 0; i < n; i++) {
                void *root;
                double rw, rh;

                root = s_ptr1(p_contentItem, objs[i]);
                if (!root || !elem_ok(root))
                    continue;
                rw = s_getd(p_iw, root);
                rh = s_getd(p_ih, root);
                if (rw >= 60 && rh >= 60) {
                    plog("---- view tree %p (%.0fx%.0f) ----", objs[i], rw, rh);
                    walk_item(root, 0, 0, 1, "", rw, rh, 1);
                } else {
                    plog("---- view %p skipped: content %.0fx%.0f ----",
                         objs[i], rw, rh);
                }
            }
        }
    }

    s_list(p_topWindows, box);
    n = read_list(box, objs, 40);
    for (i = 0; i < n; i++) {
        void *root;
        double rw, rh;

        note_window(objs[i], "top");
        if (!s_inherits(objs[i], "QQuickWindow")) continue;
        root = s_ptr1(p_contentItem, objs[i]);
        rw = s_getd(p_iw, root);
        rh = s_getd(p_ih, root);
        if (rw > 100 && rh > 100) {
            if (overview && rw >= 900)
                plog("---- QML TREE of %p root=%.0fx%.0f ----", objs[i], rw, rh);
            walk_item(root, 0, 0, 1, "", rw, rh, overview && rw >= 900);
        }
    }

    if (p_topWidgets) {
        s_list(p_topWidgets, box);
        n = read_list(box, objs, 40);
        if (n == 0 && p_appInstance) {
            const void *app   = s_ptr0(p_appInstance);
            const void *listp = app ? s_ptr1(p_childrenRef, app) : NULL;
            if (listp) {
                n = read_list(listp, objs, 40);
                for (i = 0; i < n;) {
                    if (!s_inherits(objs[i], "QWidget") ||
                        !s_getb(p_isWindow, objs[i])) {
                        objs[i] = objs[--n];
                        continue;
                    }
                    i++;
                }
                plog("topWidgets empty; app-children fallback: %d window(s)", n);
            }
        }
        for (i = 0; i < n; i++) {
            const int *r;
            int ww, wh;

            if (!s_inherits(objs[i], "QWidget")) continue;
            r  = (const int *)s_ptr1(p_geometry, objs[i]);
            ww = r ? r[2] : 0;
            wh = r ? r[3] : 0;
            walk_widget(objs[i], 0, "", ww, wh, overview);
        }
    }

    emit_snapshot();
}

/* ----------------------------------------------------- patch engine --- */
static int hexdigit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int expand(const char *s, unsigned char *out, int cap)
{
    int i = 0, o = 0;
    while (s[i] && o < cap) {
        if (s[i] == '\\' && hexdigit(s[i + 1]) >= 0 && hexdigit(s[i + 2]) >= 0) {
            out[o++] = (unsigned char)(hexdigit(s[i + 1]) * 16 + hexdigit(s[i + 2]));
            i += 3;
        } else {
            out[o++] = (unsigned char)s[i++];
        }
    }
    return o;
}
static void lower_copy(const char *s, char *out, int cap)
{
    int i;
    for (i = 0; s[i] && i < cap - 1; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

/* ---- image patch core: safe to call under the loader lock ---- */
static int patch_bytes(BYTE *base, SIZE_T sz, const unsigned char *pat, int plen,
                       const unsigned char *rep, int *applied_out)
{
    size_t off;
    int hits = 0, applied = 0;

    for (off = 0; off + (size_t)plen <= sz;) {
        int ok = 0;
        __try { ok = memcmp(base + off, pat, (size_t)plen) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = -1; }
        if (ok < 0) { off += 0x1000; continue; }
        if (!ok)    { off++; continue; }
        hits++;
        {
            DWORD oldp;
            if (VirtualProtect(base + off, (SIZE_T)plen, PAGE_READWRITE, &oldp)) {
                memcpy(base + off, rep, (size_t)plen);
                VirtualProtect(base + off, (SIZE_T)plen, oldp, &oldp);
                __try {
                    if (memcmp(base + off, rep, (size_t)plen) == 0)
                        applied++;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        off += (size_t)plen;
    }
    if (applied_out) *applied_out = applied;
    return hits;
}

/* ---- DLL load notifications: patch every mapped image at map time,
 *    i.e. BEFORE its DllMain (and long before QML compiles anything) ---- */
/* UNICODE_STRING is not visible from <windows.h> here - mirror it */
typedef struct _MY_USTR {
    unsigned short Length;
    unsigned short MaximumLength;
    wchar_t       *Buffer;
} MY_USTR;
typedef struct _LDR_DLL_NOTIF_DATA {
    ULONG    Flags;
    MY_USTR *FullDllName;
    MY_USTR *BaseDllName;
    PVOID    DllBase;
    ULONG    SizeOfImage;
} LDR_DLL_NOTIF_DATA;
typedef void (__stdcall *PDRDN)(ULONG, const LDR_DLL_NOTIF_DATA *, PVOID);

typedef struct {
    char            modsub[64];
    char            pat_s[192], rep_s[192];
    unsigned char   pat[96], rep[96];
    int             len;              /* 0 = slot empty                  */
    volatile LONG   done;             /* already patched somewhere       */
} PatchLine;
static PatchLine   g_pl[16];
static void       *g_notif_cookie;

static void reg_patch_line(const char *modsub, const char *pat_s, const char *rep_s)
{
    int i, len;
    char low[64];

    lower_copy(modsub, low, sizeof low);
    for (i = 0; i < (int)(sizeof g_pl / sizeof g_pl[0]); i++)
        if (g_pl[i].len && strcmp(g_pl[i].modsub, low) == 0 &&
            strcmp(g_pl[i].pat_s, pat_s) == 0)
            return;                             /* already registered */

    for (i = 0; i < (int)(sizeof g_pl / sizeof g_pl[0]); i++)
        if (!g_pl[i].len) break;
    if (i == (int)(sizeof g_pl / sizeof g_pl[0])) return;
    len = expand(pat_s, g_pl[i].pat, sizeof g_pl[i].pat);
    {
        int rlen = expand(rep_s, g_pl[i].rep, sizeof g_pl[i].rep);
        if (len <= 0 || rlen != len) return;
    }
    strncpy(g_pl[i].modsub, low, sizeof g_pl[i].modsub - 1);
    g_pl[i].modsub[sizeof g_pl[i].modsub - 1] = 0;
    strncpy(g_pl[i].pat_s, pat_s, sizeof g_pl[i].pat_s - 1);
    g_pl[i].pat_s[sizeof g_pl[i].pat_s - 1] = 0;
    strncpy(g_pl[i].rep_s, rep_s, sizeof g_pl[i].rep_s - 1);
    g_pl[i].rep_s[sizeof g_pl[i].rep_s - 1] = 0;
    g_pl[i].len = len;
    g_pl[i].done = 0;
}

static int g_pl_done_for(const char *modsub, const char *pat_s)
{
    int i;
    char low[64];
    lower_copy(modsub, low, sizeof low);
    for (i = 0; i < (int)(sizeof g_pl / sizeof g_pl[0]); i++)
        if (g_pl[i].len && strcmp(g_pl[i].modsub, low) == 0 &&
            strcmp(g_pl[i].pat_s, pat_s) == 0)
            return (int)g_pl[i].done;
    return 0;
}

static void g_pl_set_done(const char *modsub, const char *pat_s)
{
    int i;
    char low[64];
    lower_copy(modsub, low, sizeof low);
    for (i = 0; i < (int)(sizeof g_pl / sizeof g_pl[0]); i++)
        if (g_pl[i].len && strcmp(g_pl[i].modsub, low) == 0 &&
            strcmp(g_pl[i].pat_s, pat_s) == 0)
            InterlockedExchange(&g_pl[i].done, 1);
}

static void __stdcall dll_notify(ULONG reason, const LDR_DLL_NOTIF_DATA *d, PVOID ctx)
{
    WCHAR *wn;
    int j, i;
    char low[300];

    (void)ctx;
    if (reason != 1 || !d || !d->DllBase || !d->BaseDllName) return;

    wn  = d->BaseDllName->Buffer;
    for (j = 0; wn && (j * 2) < (int)d->BaseDllName->Length && j < 299; j++)
        low[j] = (char)tolower((unsigned char)wn[j]);
    low[j] = 0;

    for (i = 0; i < (int)(sizeof g_pl / sizeof g_pl[0]); i++) {
        int hits, applied = 0;
        if (!g_pl[i].len || g_pl[i].done) continue;
        if (!strstr(low, g_pl[i].modsub)) continue;
        hits = patch_bytes((BYTE *)d->DllBase, (SIZE_T)d->SizeOfImage,
                           g_pl[i].pat, g_pl[i].len, g_pl[i].rep, &applied);
        if (hits > 0) {
            InterlockedExchange(&g_pl[i].done, 1);
            plog("NOTIFY patch at map of %s: '%s' -> '%s' %d hit(s) %d applied",
                 low, g_pl[i].pat_s, g_pl[i].rep_s, hits, applied);
        }
    }
}

static void arm_dll_notify(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NTSTATUS (__stdcall *reg)(ULONG, void *, void *, void **);

    if (!ntdll) return;
    reg = (NTSTATUS (__stdcall *)(ULONG, void *, void *, void **))
              (void *)GetProcAddress(ntdll, "LdrRegisterDllNotification");
    if (!reg) { plog("LdrRegisterDllNotification not found"); return; }
    if (reg(0, (void *)dll_notify, NULL, &g_notif_cookie) == 0)
        plog("dll load notification armed");
}

static int cmd_patch(const char *modsub, const char *pat_s, const char *rep_s)
{
    unsigned char pat[96], rep[96];
    int plen, rlen, total = 0, applied = 0;
    char low[128];
    HANDLE snap;
    MODULEENTRY32W me;

    plen = expand(pat_s, pat, sizeof pat);
    rlen = expand(rep_s, rep, sizeof rep);
    if (plen <= 0 || rlen != plen) {
        plog("CFG patch: bad length pat=%d rep=%d", plen, rlen);
        return -1;
    }
    lower_copy(modsub, low, sizeof low);

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return -1;
    me.dwSize = sizeof me;
    if (Module32FirstW(snap, &me)) {
        do {
            char nlow[300];
            BYTE *base;
            SIZE_T sz;
            size_t j;
            int hits, applied1 = 0;

            for (j = 0; me.szModule[j] && j < 260; j++)
                nlow[j] = (char)tolower((unsigned char)me.szModule[j]);
            nlow[j] = 0;
            if (!strstr(nlow, low)) continue;

            base = me.modBaseAddr;
            sz   = me.modBaseSize;
            if (!base || sz == 0 || sz > 0x40000000) continue;

            hits = patch_bytes(base, sz, pat, plen, rep, &applied1);
            if (hits) {
                plog("CFG patch '%s' -> '%s' in %s: %d hit(s), %d applied",
                     pat_s, rep_s, me.szModule, hits, applied1);
                total   += hits;
                applied += applied1;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return total;
}

static unsigned g_seen[32];
static int      g_seen_n;
static unsigned g_try_h[32];
static int      g_try_n[32];
static int      g_try_cnt;

static unsigned line_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (*s) { h = (h ^ (unsigned char)*s++) * 1099511628211ULL; }
    return (unsigned)(h & 0x7fffffffu);
}

static int try_count(unsigned h, int bump)
{
    int i;
    for (i = 0; i < g_try_cnt; i++)
        if (g_try_h[i] == h) {
            if (bump) g_try_n[i]++;
            return g_try_n[i];
        }
    if (bump && g_try_cnt < (int)(sizeof g_try_h / sizeof g_try_h[0])) {
        g_try_h[g_try_cnt] = h;
        g_try_n[g_try_cnt] = 1;
        g_try_cnt++;
    }
    return 0;
}

/* Patch lines stay live until they hit something (or 90 tries elapse):
 * right after a hub restart the target DLL may not be mapped yet while
 * QML is still loading, and the QML source is read from the mapped image
 * on first use - so retrying every couple of seconds still lands before
 * the dialog engine compiles the file in practice. */
static void apply_cfg(void)
{
    char cfgp[MAX_PATH];
    FILE *f;
    char line[640];

    tmpPath("probe_ext.cfg", cfgp, sizeof cfgp);
    f = fopen(cfgp, "rt");

    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char cmd[64] = "", a[192] = "", b[192] = "", c[192] = "";
        unsigned h = line_hash(line);
        int i, dup = 0, got, done = 1;

        if (line[0] == ';' || line[0] == '#') continue;
        for (i = 0; i < g_seen_n; i++)
            if (g_seen[i] == h) { dup = 1; break; }
        if (dup) continue;

        got = sscanf_s(line, "%63s %191s %191s %191s",
                       cmd, (unsigned)_countof(cmd),
                       a, (unsigned)_countof(a),
                       b, (unsigned)_countof(b),
                       c, (unsigned)_countof(c));
        if (got < 1) continue;

        if (strcmp(cmd, "patch") == 0 && a[0] && b[0] && c[0]) {
            int hits, tries;
            reg_patch_line(a, b, c);             /* feed the notify engine */
            hits = g_pl_done_for(a, b) ? 1 : cmd_patch(a, b, c);
            tries = try_count(h, 1);
            if (hits > 0) {
                g_pl_set_done(a, b);
                plog("CFG patch ok: %s '%s' -> '%s' (%d hit(s))", a, b, c, hits);
            } else if (tries < 2400) {
                done = 0;                          /* retry next sweep     */
            } else {
                plog("CFG patch giving up (0 hits): %s '%s'", a, b);
            }
        } else if (strcmp(cmd, "dump_now") == 0) {
            InterlockedExchange((LONG *)&g_dump_now, 1);
        } else if (strcmp(cmd, "stop") == 0) {
            plog("CFG stop");
            InterlockedExchange(&g_stop, 1);
        }

        if (done && g_seen_n < (int)(sizeof g_seen / sizeof g_seen[0]))
            g_seen[g_seen_n++] = h;
    }
    fclose(f);
}

/* ----------------------------------------------------------- thread --- */
static HMODULE wait_module(const wchar_t *name, int tries)
{
    HMODULE h;
    int i;
    for (i = 0; i < tries; i++) {
        h = GetModuleHandleW(name);
        if (h) return h;
        Sleep(300);
    }
    return NULL;
}

static unsigned WINAPI probe_main(LPVOID arg)
{
    HMODULE core  = wait_module(L"Qt5Core.dll", 400);
    HMODULE gui   = wait_module(L"Qt5Gui.dll", 40);
    HMODULE quick  = wait_module(L"Qt5Quick.dll", 40);
    HMODULE wdgts  = GetModuleHandleW(L"Qt5Widgets.dll");
    int sweep_no = 0, overview;

    (void)arg;
    arm_dll_notify();     /* before anything: catch images as they map */
    plog("==== attach pid=%lu v%s core=%p gui=%p quick=%p widgets=%p ====",
         GetCurrentProcessId(), PROBE_VERSION, (void *)core, (void *)gui,
         (void *)quick, (void *)wdgts);
    if (!core || !gui) { plog("no Qt, detaching"); goto out; }

    p_ww              = (f_geti)   GetProcAddress(gui, "?width@QWindow@@QEBAHXZ");
    p_wh              = (f_geti)   GetProcAddress(gui, "?height@QWindow@@QEBAHXZ");
    p_winVisible      = (f_getb)   GetProcAddress(gui, "?isVisible@QWindow@@QEBA_NXZ");
    p_isTopLevel      = (f_getb)   GetProcAddress(gui, "?isTopLevel@QWindow@@QEBA_NXZ");
    p_modality        = (f_geti)   GetProcAddress(gui, "?modality@QWindow@@QEBA?AW4WindowModality@Qt@@XZ");
    p_transientParent = (f_ptr1)   GetProcAddress(gui, "?transientParent@QWindow@@QEBAPEAV1@XZ");
    p_topWindows      = (f_listfn) GetProcAddress(gui, "?topLevelWindows@QGuiApplication@@SA?AV?$QList@PEAVQWindow@@@@XZ");
    p_allWindows      = (f_listfn) GetProcAddress(gui, "?allWindows@QGuiApplication@@SA?AV?$QList@PEAVQWindow@@@@XZ");
    if (wdgts) {
        p_topWidgets   = (f_listfn) GetProcAddress(wdgts, "?topLevelWidgets@QApplication@@SA?AV?$QList@PEAVQWidget@@@@XZ");
        p_parentWidget = (f_ptr1)   GetProcAddress(wdgts, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
        p_wVisible     = (f_getb)   GetProcAddress(wdgts, "?isVisible@QWidget@@QEBA_NXZ");
        p_isWindow     = (f_getb)   GetProcAddress(wdgts, "?isWindow@QWidget@@QEBA_NXZ");
        p_geometry     = (f_ptr1)   GetProcAddress(wdgts, "?geometry@QWidget@@QEBAAEBVQRect@@XZ");
        p_windowHandle = (f_ptr1)   GetProcAddress(wdgts, "?windowHandle@QWidget@@QEBAPEAVQWindow@@XZ");
    }
    if (quick) {
        p_contentItem = (f_ptr1)  GetProcAddress(quick, "?contentItem@QQuickWindow@@QEBAPEAVQQuickItem@@XZ");
        p_childItems  = (f_list1) GetProcAddress(quick, "?childItems@QQuickItem@@QEBA?AV?$QList@PEAVQQuickItem@@@@XZ");
        p_iVisible    = (f_getb)  GetProcAddress(quick, "?isVisible@QQuickItem@@QEBA_NXZ");
        p_iw          = (f_getd)  GetProcAddress(quick, "?width@QQuickItem@@QEBANXZ");
        p_ih          = (f_getd)  GetProcAddress(quick, "?height@QQuickItem@@QEBANXZ");
        p_ix          = (f_getd)  GetProcAddress(quick, "?x@QQuickItem@@QEBANXZ");
        p_iy          = (f_getd)  GetProcAddress(quick, "?y@QQuickItem@@QEBANXZ");
        p_opac        = (f_getd)  GetProcAddress(quick, "?opacity@QQuickItem@@QEBANXZ");
    }
    p_appInstance = (f_ptr0)  GetProcAddress(core, "?instance@QCoreApplication@@SAPEAV1@XZ");
    p_inherits    = (f_inherits) GetProcAddress(core, "?inherits@QObject@@QEBA_NPEBD@Z");
    p_childrenRef = (f_ptr1)  GetProcAddress(core, "?children@QObject@@QEBAAEBV?$QList@PEAVQObject@@@@XZ");
    p_objname     = (f_qstr)  GetProcAddress(core, "?objectName@QObject@@QEBA?AVQString@@XZ");
    p_unicode     = (f_u16)   GetProcAddress(core, "?unicode@QString@@QEBAPEBVQChar@@XZ");
    p_qdtor       = (f_dtor)  GetProcAddress(core, "??1QString@@QEAA@XZ");
    p_parent      = (f_ptr1)  GetProcAddress(core, "?parent@QObject@@QEBAPEAV1@XZ");
    p_ldSize      = (f_geti)  GetProcAddress(core, "?size@QListData@@QEBAHXZ");
    p_ldAt        = (f_ldat)  GetProcAddress(core, "?at@QListData@@QEBAPEAPEAXH@Z");
    p_metaObject  = (f_metafn) GetProcAddress(core, "?metaObject@QObject@@UEBAPEBUQMetaObject@@XZ");
    p_className   = (f_clsname) GetProcAddress(core, "?className@QMetaObject@@QEBAPEBDXZ");
    if (quick)
        p_itemWindow = (f_ptr1) GetProcAddress(quick, "?window@QQuickItem@@QEBAPEAVQQuickWindow@@XZ");
    if (!p_ldSize || !p_ldAt) { plog("QListData accessors missing, detaching"); goto out; }
    if (!p_topWindows)        { plog("topLevelWindows missing, detaching");     goto out; }

    plog("armed: quick=%p widgets=%p allWin=%p items=%p",
         (void *)quick, (void *)wdgts, (void *)p_allWindows, (void *)p_childItems);

    apply_cfg();                        /* patches land before QML settles */

    while (!g_stop && sweep_no < 900) {
        Sleep(350);
        sweep_no++;
        if (sweep_no % 2 == 1) apply_cfg();
        overview = (sweep_no <= 3) || InterlockedExchange((LONG *)&g_dump_now, 0);
        __try {
            sweep(overview);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            plog("sweep #%d faulted (hub unaffected)", sweep_no);
        }
    }
out:
    plog("==== detach after %d sweeps ====", sweep_no);
    EnterCriticalSection(&g_loglock);
    if (g_log) { fclose(g_log); g_log = NULL; }
    LeaveCriticalSection(&g_loglock);
    DeleteCriticalSection(&g_loglock);
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    HANDLE th;
    DWORD tid;

    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char logp[MAX_PATH];
        DisableThreadLibraryCalls(h);
        g_self = h;
        InitializeCriticalSection(&g_loglock);
        tmpPath("probe_ext.log", logp, sizeof logp);
        g_log = fopen(logp, "at");
        if (g_log) setvbuf(g_log, NULL, _IOFBF, 4096);
        th = CreateThread(NULL, 0, probe_main, NULL, 0, &tid);
        if (th) CloseHandle(th);
    }
    return TRUE;
}









