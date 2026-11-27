/*
 * marker_quality.c - owns everything the tracking markers look like: their
 * color says how well the scanner has observed them (red / yellow / green),
 * their outline says which marker is in the scanner's view right now
 * (blue), EXStar Hub 2.x (file stamp 3.1.49).
 *
 * Architecture (two sides inside ONE module, mirroring the proven
 * production pipeline; the module loads into every app process, each side
 * activates only where its pieces exist):
 *
 *   scan side (scanservice.exe - the only process where vendor
 *   HandlePrjFile::putFrameDataIntoFile receives frames):
 *     detour the export (by NAME, via host->hook), parse the frame's
 *     QVariantMap (layout proven offline against Qt5Core 5.15.10 with
 *     tools/qmap_lab.c), accumulate per-marker coverage (Welford RMS +
 *     >=20deg direction slots, ported from the reference model), and publish a small
 *     snapshot file into C:\Users\Public\Shining3d\EXStar Hub\ - the same
 *     cross-process bridge the vendor app tolerates (no DACL, no
 *     SeCreateGlobalPrivilege issues).
 *
 *   render side (hub - the only process with librenderkit + osg):
 *     signature-scan the renderer for the vendor color-update stub
 *     ("add rcx,8; jmp <colorUpdate>", exactly one occurrence in this
 *     build) and swap the pointer that references it (renderer+0x28CAA8
 *     statically verified).  Every frame the vendor now calls our hook
 *     with owner = { ?, osg::Group*, visibility-array* }: we forward to
 *     the original stub and then override two per-child osg uniforms -
 *     "frontInnerColor" (the disc) with the quality color for that marker
 *     id, and "frontExternalColor" (the outline) with the in-view color.
 *     "In the current frame" is read straight from what the vendor just
 *     wrote: red into the disc means the node is in CURRENT_MARKER
 *     (docs/research/marker-pipeline.md), and the vendor never touches
 *     the outline at runtime, so the outline is a channel of its own.
 *     Marker id per child index comes from bindings captured on
 *     setMarkerPoints (render chain node+0x150 -> mix, +0x138 -> geom,
 *     geom+0x220 -> key vector, geom+0x248 -> state -> +8 visibility).
 *     The outline needs no id, no binding and no bridge file: it works
 *     from the first frame of a project, also before any scanning, and
 *     also if the scan side never got a sibling slot.
 *
 * Rules honored: injection only, zero RVAs (names + signatures only),
 * DllMain untouched, SEH around every foreign pointer, no file patching,
 * session-only (bridge file deleted at start; no persistence anywhere).
 *
 * Quality stages (user-approved spec): NEW entity per marker id, exactly
 * three stages, green latches and is only reset by a new project:
 *   red    - seen at all
 *   yellow - frames >= yellow_frames && supported >= yellow_supported
 *   green  - frames >= green_frames && RMS spread <= green_spread_um &&
 *            supported >= green_supported   (latched)
 * Thresholds live in modules\marker_quality.ini; defaults follow the
 * reference model and are calibrated against live probe data.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>

#include "exstar_sdk.h"

#define MOD_ID      "marker_quality"
#define MOD_VER     "1.1.8"

/* vendor exports, resolved by name only -------------------------------- */
#define EXP_PUT_FRAME \
    "?putFrameDataIntoFile@HandlePrjFile@@QEAAXAEBV?$QMap@VQString@@VQVariant@@@@@Z"
#define EXP_SET_MARKERS \
    "?setMarkerPoints@ModelNode@ExtDigit@RenderKit@Sn3D@@QEAAIUSn3DMarkerParam@234@@Z"

/* frame ABI (verified against the same program): records stride 0xB0,
 * double3 camera-local position at +0, int32 id at +0x50; world = R*local+t
 * with rt = 12 doubles (row-major 3x3 + translation). */
#define REC_SIZE    0xB0
#define REC_ID_OFF  0x50

#define MAX_IDS             4096
#define MAX_FRAME_ENTRIES   256

static const ExHost *g_host;

/* ------------------------------------------------------------------ */
/* Quality model (ported from the reference model, thresholds ini-driven) */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t frames;
    double   center[3];
    double   sq_err;
} Quality;

typedef struct {
    Quality  pos;
    double   dir[8][3];
    uint32_t support[8];
    unsigned directions;
    unsigned octants;
    double   max_angle;
    unsigned complete;          /* green latched */
    double   dev_sum, dev_max;  /* calibration reporting */
    uint64_t dev_n;
} Coverage;

static int quality_add(Quality *q, const double p[3])
{
    int a;
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
        return 0;
    q->frames++;
    for (a = 0; a < 3; a++) {
        double delta = p[a] - q->center[a];
        double next  = q->center[a] + delta / (double) q->frames;
        q->sq_err += delta * (p[a] - next);
        q->center[a] = next;
    }
    return isfinite(q->sq_err);
}

static double quality_spread(const Quality *q)
{
    if (!q->frames)
        return 0.0;
    return sqrt(q->sq_err > 0 ? q->sq_err / (double) q->frames : 0.0);
}

static unsigned coverage_supported(const Coverage *c)
{
    unsigned i, n = 0;
    for (i = 0; i < c->directions; i++)
        n += c->support[i] >= 3;
    return n;
}

/* config (modules\marker_quality.ini) */
static int g_green_frames   = 20;
static int g_green_spread_um = 100;   /* RMS spread in micrometres        */
static int g_green_supported = 4;
static int g_yellow_frames  = 8;
static int g_yellow_supported = 2;
static int g_report_ms      = 5000;
static int g_publish_ms     = 300;
static int g_verbose        = 1;    /* periodic calibration/binding logs   */
static int g_scan_side      = 1;
static int g_paint_side     = 1;
static uint32_t g_argb[3];            /* red, yellow, green (0xffrrggbb)  */

static void coverage_add(Coverage *c, const double p[3], const double cam[3])
{
    double ray[3], sq = 0, residual = 0, len, largest = -2, dev;
    int a;
    unsigned i, nearest = 0, octant = 0;

    if (c->complete)
        return;
    if (c->pos.frames) {                     /* deviation BEFORE the update */
        for (a = 0; a < 3; a++) {
            dev = p[a] - c->pos.center[a];
            sq += dev * dev;
        }
        dev = sqrt(sq);
        c->dev_sum += dev;
        c->dev_n++;
        if (dev > c->dev_max)
            c->dev_max = dev;
    }
    if (!quality_add(&c->pos, p))
        return;
    for (a = 0; a < 3; a++) {
        double e = p[a] - c->pos.center[a];
        ray[a] = cam[a] - p[a];
        residual += e * e;
    }
    sq = 0;
    for (a = 0; a < 3; a++)
        sq += ray[a] * ray[a];
    if (!isfinite(sq) || sq < 1e-12 || residual > 0.15 * 0.15)
        return;                              /* bad pose: no coverage credit */
    len = sqrt(sq);
    for (a = 0; a < 3; a++) {
        ray[a] /= len;
        if (ray[a] >= 0)
            octant |= 1u << a;
    }
    c->octants |= 1u << octant;
    for (i = 0; i < c->directions; i++) {
        double dot = 0, angle;
        for (a = 0; a < 3; a++)
            dot += ray[a] * c->dir[i][a];
        if (dot > largest) { largest = dot; nearest = i; }
        angle = acos(dot < -1 ? -1 : dot > 1 ? 1 : dot) * 57.29577951308232;
        if (angle > c->max_angle)
            c->max_angle = angle;
    }
    if (largest <= 0.9396926207859084 && c->directions < 8) {   /* >= 20 deg */
        i = c->directions++;
        for (a = 0; a < 3; a++)
            c->dir[i][a] = ray[a];
        c->support[i] = 1;
    } else if (c->directions && largest > 0.9396926207859084) {
        if (c->support[nearest] != 0xFFFFFFFFu)
            c->support[nearest]++;
    }
    if (c->pos.frames >= (uint64_t) g_green_frames &&
        quality_spread(&c->pos) <= (double) g_green_spread_um / 1000.0 &&
        coverage_supported(c) >= (unsigned) g_green_supported)
        c->complete = 1;   /* positions are in mm: um -> mm is /1000 */
}

static unsigned coverage_tier(const Coverage *c)   /* 0 red 1 yellow 2 green */
{
    if (!c->pos.frames)
        return 0xFFFFFFFFu;                  /* unseen */
    if (c->complete)
        return 2;
    if (c->pos.frames >= (uint64_t) g_yellow_frames &&
        coverage_supported(c) >= (unsigned) g_yellow_supported)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Snapshot bridge (scanservice -> hub)                                */
/* ------------------------------------------------------------------ */
#define SNAP_MAGIC  0x3142514Fu              /* "MQB1" */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    int32_t  frame;                          /* <0: new project / cleared  */
    uint32_t count;
    uint32_t argb[MAX_IDS];
} Snapshot;

static const wchar_t *SNAP_DIR  = L"C:\\Users\\Public\\Shining3d\\EXStar Hub\\";
static const wchar_t *SNAP_NAME = L"exstar_marker_quality.dat";
static const wchar_t *SNAP_TMP  = L"exstar_marker_quality.tmp";

static void snap_path(wchar_t *out, size_t cap, const wchar_t *name)
{
    wcsncpy(out, SNAP_DIR, cap - 1);
    out[cap - 1] = 0;
    wcsncat(out, name, cap - wcslen(out) - 1);
}

static void snap_delete(void)
{
    wchar_t p[MAX_PATH];
    snap_path(p, MAX_PATH, SNAP_NAME);
    DeleteFileW(p);                          /* session-only: start clean  */
}

/* The shared data folder may not exist on a fresh machine (the vendor app
 * is often assumed to have created it); create it recursively, idempotently.  */
static void ensure_snap_dir(void)
{
    static const wchar_t *dirs[] = {
        L"C:\\Users\\Public\\Shining3d",
        L"C:\\Users\\Public\\Shining3d\\EXStar Hub"
    };
    size_t i;
    for (i = 0; i < 2; i++)
        CreateDirectoryW(dirs[i], NULL);
}

static int snap_exists(void)
{
    wchar_t p[MAX_PATH];
    snap_path(p, MAX_PATH, SNAP_NAME);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static SRWLOCK       g_lock = SRWLOCK_INIT;  /* guards g_cov + publish     */
static Coverage      g_cov[MAX_IDS];
enum { ST_INVALID, ST_NO_FRAME, ST_NO_MARKER, ST_NO_POSE,
       ST_BAD_SPAN, ST_NO_IDS, ST_ACCEPTED, ST_COUNT };
static volatile LONG g_stats[ST_COUNT];
static volatile LONG g_frames_total;
static void *volatile g_project;             /* owner => new project reset */
static volatile LONG g_sample_logged;
static ULONGLONG     g_last_report;
static ULONGLONG     g_last_publish;
static volatile LONG g_publish_busy;
static Snapshot      g_published;            /* last written payload       */
static int           g_published_valid;
static uint32_t      g_seq;

static void (*tramp_put_frame)(void *, const void *);
static unsigned (*tramp_set_markers)(void *, void *);

/* resolved Qt helpers (Qt5Core exports only) --------------------------- */
typedef int                     (*f_variant_type)(const void *);
typedef const void             *(*f_variant_data)(const void *);
typedef int                     (*f_variant_int)(const void *, unsigned char *);
typedef int                     (*f_str_size)(const void *);
typedef const unsigned short   *(*f_str_utf16)(const void *);

static f_variant_type  p_userType;
static f_variant_data  p_constData;
static f_variant_int   p_toInt;
static f_str_size      p_strSize;
static f_str_utf16     p_strUtf16;
static const void     *g_map_null;           /* &QMapDataBase::shared_null */

/* ------------------------------------------------------------------ */
/* Safe primitives                                                     */
/* ------------------------------------------------------------------ */
static int safe_read(void *dst, const void *src, SIZE_T size)
{
    __try {
        memcpy(dst, src, size);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Qt QMap<QString,QVariant> access                                    */
/*                                                                     */
/* Layout proven OFFLINE against this exact Qt5Core.dll 5.15.10 by     */
/* building a real map through exported functions (tools/qmap_lab.c):  */
/*   *map = d; d+0x00 ref, d+0x04 alloc, d+0x08 end sentinel           */
/*   (root node's parent points back there), d+0x10 = root,            */
/*   d+0x20 = leftmost. Node: parent@0 (tagged, &~3), left@8, right@16,*/
/*   QString key@24, QVariant value@32 { union@0, type@+8, shared@+12 }*/
/* ------------------------------------------------------------------ */
#define D_SENTINEL 8
#define D_ROOT     0x10
#define N_LEFT     8
#define N_RIGHT    16
#define N_KEY      24
#define N_VAL      32

static int variant_type(const void *v)
{
    int t = -1;
    if (!v)
        return -1;
    __try {
        t = p_userType(v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return t;
}

static const void *variant_data(const void *v)
{
    const void *d = NULL;
    if (!v)
        return NULL;
    __try {
        d = p_constData(v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return d;
}

/* vendor shared-pointer wrapper: constData -> { object, alias } */
static const void *variant_shared_object(const void *v)
{
    const void *wrapper = variant_data(v);
    const void *object = NULL;
    if (!wrapper)
        return NULL;
    __try {
        object = *(const void * volatile *) wrapper;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    return object;
}

/* read node key into ASCII buffer; returns length or -1 */
static int node_key(const void *node, char *out, int cap)
{
    const void *key = (const char *) node + N_KEY;
    const unsigned short *u;
    int n, i;
    __try {
        n = p_strSize(key);
        if (n <= 0 || n >= cap)
            return -1;
        u = (const unsigned short *) p_strUtf16(key);
        if (!u)
            return -1;
        for (i = 0; i < n; i++)
            out[i] = (char) u[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    out[n] = '\0';
    for (i = 0; i < n; i++)
        if ((unsigned char) out[i] < 0x20 || (unsigned char) out[i] > 0x7E)
            return -1;
    return n;
}

typedef struct {
    const char *want;
    const void *found;
    int         active;
} MapCtx;

static void map_walk(const void *node, const void *header, int depth,
                     MapCtx *ctx)
{
    const void *l = NULL, *r = NULL;

    if (!node || node == header || depth > 48 || !ctx->active)
        return;
    if (ctx->found)
        return;
    __try {
        l = *(const void * volatile *) ((const char *) node + N_LEFT);
        r = *(const void * volatile *) ((const char *) node + N_RIGHT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->active = 0;
        return;
    }
    map_walk(l, header, depth + 1, ctx);
    if (ctx->active && !ctx->found) {
        char name[64];
        if (node_key(node, name, (int) sizeof name) >= 0 &&
            strcmp(name, ctx->want) == 0)
            ctx->found = (const char *) node + N_VAL;
    }
    map_walk(r, header, depth + 1, ctx);
}

static int map_root(const void *map, const void **root, const void **header)
{
    const void *d, *rt;
    if (!map)
        return 0;
    d = *(const void * volatile *) map;
    if (!d || d == g_map_null)
        return 0;
    rt = NULL;
    __try {
        rt = *(const void * volatile *) ((const char *) d + D_ROOT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    rt = (const void *) ((uintptr_t) rt & ~(uintptr_t) 3);
    if (!rt)
        return 0;
    *root = rt;
    *header = (const char *) d + D_SENTINEL;
    return 1;
}

static const void *map_find(const void *map, const char *key)
{
    const void *root, *header;
    MapCtx ctx;

    if (!map_root(map, &root, &header))
        return NULL;
    memset(&ctx, 0, sizeof ctx);
    ctx.want = key;
    ctx.active = 1;
    map_walk(root, header, 0, &ctx);
    return ctx.found;
}

static int map_helpers_ready(void)
{
    return p_userType && p_constData && p_strSize && p_strUtf16;
}

/* ------------------------------------------------------------------ */
/* Snapshot publishing (scan side)                                     */
/* ------------------------------------------------------------------ */
static void publish_locked(int cleared)    /* g_lock must be held          */
{
    Snapshot snap;
    int id;
    int ok = 0;
    wchar_t tmp[MAX_PATH], dst[MAX_PATH];

    memset(&snap, 0, sizeof snap);
    snap.magic = SNAP_MAGIC;
    snap.count = MAX_IDS;
    snap.seq   = ++g_seq;
    snap.frame = cleared ? -1 : 0;
    if (!cleared) {
        for (id = 0; id < MAX_IDS; id++) {
            unsigned tier = coverage_tier(&g_cov[id]);
            snap.argb[id] = tier == 0xFFFFFFFFu ? 0u : g_argb[tier];
        }
    }
    if (g_published_valid && snap.frame == g_published.frame &&
        memcmp(g_published.argb, snap.argb, sizeof snap.argb) == 0 &&
        snap_exists())
        return;                            /* nothing changed: stay off fs */
    memcpy(&g_published, &snap, sizeof snap);
    g_published_valid = 1;
    g_last_publish = GetTickCount64();

    ensure_snap_dir();                       /* folder may vanish too      */
    snap_path(tmp, MAX_PATH, SNAP_TMP);
    snap_path(dst, MAX_PATH, SNAP_NAME);
    {
        HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        if (file == INVALID_HANDLE_VALUE)
            return;
        ok = WriteFile(file, &snap, sizeof snap, &written, NULL) &&
             written == sizeof snap;
        CloseHandle(file);
    }
    if (ok)
        MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
    else
        DeleteFileW(tmp);
}

static void publish_maybe(int force)
{
    ULONGLONG now = GetTickCount64();

    if (!g_scan_side)
        return;
    if (InterlockedCompareExchange(&g_publish_busy, 1, 0))
        return;                            /* one writer is enough         */
    if (!force && g_last_publish &&
        now - g_last_publish < (ULONGLONG) g_publish_ms) {
        InterlockedExchange(&g_publish_busy, 0);
        return;                            /* throttled; next frame retry  */
    }
    AcquireSRWLockExclusive(&g_lock);
    publish_locked(0);
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_publish_busy, 0);
}

/* ------------------------------------------------------------------ */
/* Frame observation (scan side)                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t id;
    size_t  n;
    double  mean[3];
} FrameEntry;

/* ------------------------------------------------------------------ */
/* Pose (rt) acquisition                                               */
/*                                                                     */
/* an earlier reference read rt as 12 doubles behind the shared-pointer */
/* an earlier build; on this exact build the wrapper deref produced    */
/* garbage (calibration log showed 1e38 centers), so we probe the     */
/* candidate layouts once, keep the one whose rotation part is         */
/* orthonormal, and dump raw bytes if nothing validates.               */
/* ------------------------------------------------------------------ */
static volatile LONG g_pose_mode;   /* 0 unknown 1 *cdata 2 cdata 3 *cdata+8 */
static volatile LONG g_pose_bad;    /* consecutive failed frames        */
static volatile LONG g_pose_dumps;  /* diagnostics budget               */

static int pose_valid(const double *p)   /* p = 12 doubles, row-major R|t */
{
    int c;
    for (c = 0; c < 3; c++) {
        double n = p[c] * p[c] + p[3 + c] * p[3 + c] + p[6 + c] * p[6 + c];
        if (!isfinite(n) || n < 0.98 || n > 1.02)
            return 0;                    /* column must be a unit vector */
    }
    if (!isfinite(p[9]) || !isfinite(p[10]) || !isfinite(p[11]) ||
        fabs(p[9]) > 1e7 || fabs(p[10]) > 1e7 || fabs(p[11]) > 1e7)
        return 0;
    return 1;
}

static int pose_load(int mode, const void *cdata, double *pose)
{
    const void *src;
    if (mode == 1) {
        if (!safe_read(&src, cdata, 8) || !src)
            return 0;
        src = (const char *) src;
    } else if (mode == 2) {
        src = cdata;
    } else {
        if (!safe_read(&src, cdata, 8) || !src)
            return 0;
        src = (const char *) src + 8;
    }
    return safe_read(pose, src, 12 * sizeof(double)) && pose_valid(pose);
}

static void pose_diag_dump(const char *why, const void *markerVar,
                           const void *poseVar, const void *cdata)
{
    double raw[12];
    const void *obj = NULL;

    if (InterlockedIncrement(&g_pose_dumps) > 3)
        return;
    if (cdata)
        safe_read(&obj, cdata, 8);
    g_host->log("mq: pose DIAG(%s) cmType=%d rtType=%d cdata=%p obj=%p",
                why, variant_type(markerVar), variant_type(poseVar),
                cdata, obj);
    if (obj && safe_read(raw, obj, sizeof raw)) {
        g_host->log("mq: pose DIAG   *cdata : %g %g %g | %g %g %g | %g %g %g | %g %g %g",
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                    raw[7], raw[8], raw[9], raw[10], raw[11]);
    }
    if (cdata && safe_read(raw, cdata, sizeof raw)) {
        g_host->log("mq: pose DIAG    cdata  : %g %g %g | %g %g %g | %g %g %g | %g %g %g",
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6],
                    raw[7], raw[8], raw[9], raw[10], raw[11]);
    }
}

static int observe_pose(const void *markerVar, const void *poseVar,
                        double *pose)
{
    const void *cdata = variant_data(poseVar);
    int mode;

    if (!cdata)
        return 0;
    mode = (int) g_pose_mode;
    if (mode && pose_load(mode, cdata, pose)) {
        InterlockedExchange(&g_pose_bad, 0);
        return 1;
    }
    if (mode) {
        if (InterlockedIncrement(&g_pose_bad) < 50) {
            InterlockedIncrement(&g_stats[ST_NO_POSE]);
            return 0;
        }
        g_host->log("mq: pose mode=%d failed 50x, re-probing", mode);
        InterlockedExchange(&g_pose_mode, 0);
    }
    for (mode = 1; mode <= 3; mode++) {
        if (pose_load(mode, cdata, pose)) {
            if (mode != (int) g_pose_mode)
                g_host->log("mq: pose source locked to mode=%d", mode);
            InterlockedExchange(&g_pose_mode, mode);
            InterlockedExchange(&g_pose_bad, 0);
            return 1;
        }
    }
    pose_diag_dump("no-valid-pose", markerVar, poseVar, cdata);
    InterlockedIncrement(&g_stats[ST_NO_POSE]);
    return 0;
}

static void report(void);

static void observe(void *owner, const void *params)
{
    const void *frameVar, *frame, *markerVar, *poseVar, *countVar;
    const void *markerObj;
    uintptr_t begin = 0, end = 0;
    size_t count, i, used = 0;
    double pose[12];
    unsigned char intOk = 0;
    int frameNo = -1, good, stageChanged = 0;
    FrameEntry entries[MAX_FRAME_ENTRIES];

    if (owner != g_project) {
        g_project = owner;                  /* new project: full reset     */
        AcquireSRWLockExclusive(&g_lock);
        memset(g_cov, 0, sizeof g_cov);
        publish_locked(1);                  /* frame=-1 clears the hub     */
        ReleaseSRWLockExclusive(&g_lock);
        g_host->log("mq: new project owner=%p, quality reset", owner);
    }
    InterlockedIncrement(&g_frames_total);

    if (!map_helpers_ready()) {
        InterlockedIncrement(&g_stats[ST_INVALID]);
        return;
    }
    frameVar = map_find(params, "frameData");
    if (!frameVar || variant_type(frameVar) != 8) {   /* 8 = QVariantMap */
        InterlockedIncrement(&g_stats[ST_NO_FRAME]);
        return;
    }
    frame = variant_data(frameVar);
    if (!frame) {
        InterlockedIncrement(&g_stats[ST_NO_FRAME]);
        return;
    }

    markerVar = map_find(frame, "curMarker");
    poseVar   = map_find(frame, "rt");
    countVar  = map_find(frame, "frameCount");

    if (countVar) {
        __try {
            frameNo = p_toInt(countVar, &intOk);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            frameNo = -1;
        }
    }

    if (!markerVar) { InterlockedIncrement(&g_stats[ST_NO_MARKER]); return; }
    if (!poseVar)   { InterlockedIncrement(&g_stats[ST_NO_POSE]);   return; }

    markerObj = variant_shared_object(markerVar);
    if (!markerObj) {
        InterlockedIncrement(&g_stats[ST_NO_MARKER]);
        return;
    }
    if (!observe_pose(markerVar, poseVar, pose))
        return;                            /* stats counted inside         */

    good = 1;
    __try {
        begin = *(const uintptr_t volatile *) ((const char *) markerObj + 0);
        end   = *(const uintptr_t volatile *) ((const char *) markerObj + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        good = 0;
    }
    if (!good) { InterlockedIncrement(&g_stats[ST_INVALID]); return; }
    if (!begin || end < begin || (end - begin) % REC_SIZE) {
        InterlockedIncrement(&g_stats[ST_BAD_SPAN]);
        return;
    }
    count = (end - begin) / REC_SIZE;
    if (count == 0 || count > MAX_FRAME_ENTRIES) {
        InterlockedIncrement(&g_stats[ST_BAD_SPAN]);
        return;
    }

    for (i = 0; i < count; i++) {
        const unsigned char *rec = (const unsigned char *) begin + i * REC_SIZE;
        double local[3] = { 0, 0, 0 };      /* double[3], NOT float3        */
        int32_t id = -1;                    /* (packed laser ABI variant)   */
        double world[3];
        size_t s;

        if (!safe_read(local, rec, sizeof local) ||
            !safe_read(&id, rec + REC_ID_OFF, sizeof id))
            break;
        if (id < 0 || id >= MAX_IDS)
            continue;
        world[0] = (double) local[0] * pose[0] + (double) local[1] * pose[3] +
                   (double) local[2] * pose[6]  + pose[9];
        world[1] = (double) local[0] * pose[1] + (double) local[1] * pose[4] +
                   (double) local[2] * pose[7]  + pose[10];
        world[2] = (double) local[0] * pose[2] + (double) local[1] * pose[5] +
                   (double) local[2] * pose[8]  + pose[11];
        if (!isfinite(world[0]) || !isfinite(world[1]) || !isfinite(world[2]))
            continue;
        for (s = 0; s < used; s++)
            if (entries[s].id == id)
                break;
        if (s == used) {
            entries[s].id = id;
            entries[s].n = 1;
            entries[s].mean[0] = world[0];
            entries[s].mean[1] = world[1];
            entries[s].mean[2] = world[2];
            used++;
        } else {
            entries[s].mean[0] += world[0];
            entries[s].mean[1] += world[1];
            entries[s].mean[2] += world[2];
            entries[s].n++;
        }
    }
    if (!used) { InterlockedIncrement(&g_stats[ST_NO_IDS]); return; }
    InterlockedIncrement(&g_stats[ST_ACCEPTED]);

    if (!InterlockedCompareExchange(&g_sample_logged, 1, 0)) {
        double n = (double) entries[0].n;
        g_host->log("mq: first sample id=%d world=(%.3f,%.3f,%.3f) "
                    "poseMode=%d pose=(%.3f,%.3f,%.3f)",
                    entries[0].id, entries[0].mean[0] / n,
                    entries[0].mean[1] / n, entries[0].mean[2] / n,
                    (int) g_pose_mode, pose[9], pose[10], pose[11]);
    }

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < used; i++) {
        Coverage *c = &g_cov[entries[i].id];
        unsigned was = coverage_tier(c);
        double m[3];
        m[0] = entries[i].mean[0] / (double) entries[i].n;
        m[1] = entries[i].mean[1] / (double) entries[i].n;
        m[2] = entries[i].mean[2] / (double) entries[i].n;
        coverage_add(c, m, pose + 9);
        if (coverage_tier(c) != was)
            stageChanged = 1;              /* force a publish now          */
    }
    ReleaseSRWLockExclusive(&g_lock);

    publish_maybe(stageChanged);
    report();
    (void) frameNo;
}

static void report(void)
{
    ULONGLONG now = GetTickCount64();
    int id, active = 0;

    if (!g_verbose)
        return;
    if (g_last_report && now - g_last_report < (ULONGLONG) g_report_ms)
        return;
    g_last_report = now;
    g_host->log("mq: === pid=%lu frames=%ld stats=[invalid=%ld noFrame=%ld "
                "noMarker=%ld noPose=%ld badSpan=%ld noIds=%ld ok=%ld] ===",
                (unsigned long) GetCurrentProcessId(),
                (long) g_frames_total,
                (long) g_stats[ST_INVALID], (long) g_stats[ST_NO_FRAME],
                (long) g_stats[ST_NO_MARKER], (long) g_stats[ST_NO_POSE],
                (long) g_stats[ST_BAD_SPAN], (long) g_stats[ST_NO_IDS],
                (long) g_stats[ST_ACCEPTED]);
    AcquireSRWLockShared(&g_lock);
    for (id = 0; id < MAX_IDS; id++) {
        const Coverage *c = &g_cov[id];
        if (!c->pos.frames)
            continue;
        active++;
        g_host->log("mq: id=%d frames=%llu spread=%.4f devAvg=%.4f "
                    "devMax=%.4f dirs=%u supported=%u maxAng=%.1f oct=%X "
                    "tier=%u center=(%.3f,%.3f,%.3f)",
                    id,
                    (unsigned long long) c->pos.frames,
                    quality_spread(&c->pos),
                    c->dev_n ? c->dev_sum / (double) c->dev_n : 0.0,
                    c->dev_max,
                    c->directions, coverage_supported(c),
                    c->max_angle, c->octants, coverage_tier(c),
                    c->pos.center[0], c->pos.center[1], c->pos.center[2]);
    }
    ReleaseSRWLockShared(&g_lock);
    g_host->log("mq: === %d marker(s) tracked ===", active);
}

static void hook_put_frame(void *owner, const void *params)
{
    if (g_scan_side) {
        __try {
            observe(owner, params);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_stats[ST_INVALID]);
        }
    }
    if (tramp_put_frame)
        tramp_put_frame(owner, params);
}

/* ------------------------------------------------------------------ */
/* Render side (hub)                                                   */
/* ------------------------------------------------------------------ */
/*
 * Bindings: setMarkerPoints gives us the ModelNode; the vendor render
 * chain (validated live) is  node+0x150 -> mix, mix+0x138 -> geom,
 * geom+0x220 -> key vector (u64 marker ids, begin/end at +0x88/+0x98),
 * geom+0x248 -> state, state+8 -> visibility object (i8 per marker at
 * +0x88/+0x98: <0 hidden).  The color-update owner is
 * { ?, osg::Group*, same visibility object }: matching on the visibility
 * pointer + count identifies which ids the current child array holds.
 */
#define N_BINDINGS 8
typedef struct {
    uintptr_t flags;                        /* owner+16 identity           */
    uint32_t  count;
    uint64_t  ids[MAX_IDS];
    ULONGLONG last;
} Binding;
static Binding   g_bindings[N_BINDINGS];
static SRWLOCK   g_bind_lock = SRWLOCK_INIT;
static volatile LONG g_chain_fail_logged;
static ULONGLONG     g_bind_log_tick;

static int chain_get(void *node, uintptr_t *keysObj, uintptr_t *flagsObj)
{
    uintptr_t mix = 0, geom = 0, state = 0, keys = 0, flags = 0;

    if (!node)
        return 0;
    __try {
        mix   = *(volatile uintptr_t *) ((char *) node + 0x150);
        if (!mix) return 0;
        geom  = *(volatile uintptr_t *) (mix + 0x138);
        if (!geom) return 0;
        keys  = *(volatile uintptr_t *) (geom + 0x220);
        state = *(volatile uintptr_t *) (geom + 0x248);
        if (!keys || !state) return 0;
        flags = *(volatile uintptr_t *) (state + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!flags)
        return 0;
    *keysObj = keys;
    *flagsObj = flags;
    return 1;
}

static void bind_marker_render(void *node)
{
    uintptr_t keysObj = 0, flagsObj = 0, kr[2] = { 0, 0 };
    uint64_t ids[MAX_IDS];
    size_t count, slot, oldest = 0;
    ULONGLONG now = GetTickCount64(), oldestTick = ULLONG_MAX;
    int i;

    if (!chain_get(node, &keysObj, &flagsObj)) {
        LONG n = InterlockedIncrement(&g_chain_fail_logged);
        if (n <= 3)
            g_host->log("mq: render chain unresolved (fail #%ld)", (long) n);
        return;
    }
    if (!safe_read(kr, (const void *) (keysObj + 0x88), 16) ||
        kr[1] <= kr[0] || (kr[1] - kr[0]) % 8)
        return;
    count = (kr[1] - kr[0]) / 8;
    if (count > MAX_IDS)
        count = MAX_IDS;
    if (!safe_read(ids, (const void *) kr[0], count * 8))
        return;

    AcquireSRWLockExclusive(&g_bind_lock);
    for (i = 0; i < N_BINDINGS; i++)
        if (g_bindings[i].flags == flagsObj && g_bindings[i].count == count) {
            g_bindings[i].last = now;       /* refresh ids (ids can move)  */
            memcpy(g_bindings[i].ids, ids, count * 8);
            goto done;
        }
    slot = N_BINDINGS;
    for (i = 0; i < N_BINDINGS; i++)
        if (!g_bindings[i].flags) { slot = (size_t) i; break; }
    if (slot == N_BINDINGS) {
        for (i = 0; i < N_BINDINGS; i++)
            if (g_bindings[i].last < oldestTick) {
                oldestTick = g_bindings[i].last;
                oldest = (size_t) i;
            }
        slot = oldest;
    }
    g_bindings[slot].flags = flagsObj;
    g_bindings[slot].count = (uint32_t) count;
    memcpy(g_bindings[slot].ids, ids, count * 8);
    g_bindings[slot].last = now;
    if (g_verbose && now - g_bind_log_tick >= 10000) {
        g_bind_log_tick = now;
        g_host->log("mq: binding %d: flags=%p count=%zu (ids %llu..%llu)",
                    (int) slot, (void *) flagsObj, count,
                    ids[0], ids[count - 1]);
    }
done:
    ReleaseSRWLockExclusive(&g_bind_lock);
}

static unsigned hook_set_marker_points(void *node, void *params)
{
    unsigned r = 0;

    if (tramp_set_markers)
        r = tramp_set_markers(node, params);
    if (g_paint_side) {
        __try {
            bind_marker_render(node);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return r;
}

/* ---- color slot (signature, no RVA) --------------------------------- */
static void  (*g_orig_color_update)(void *);
static void  *(*p_osg_get_child)(void *, unsigned);        /* Node*        */
static unsigned (*p_osg_num_children)(void *);
static void  *(*p_osg_get_state_set)(void *);
static void  *(*p_osg_get_uniform)(void *, const void *);  /* std::string& */
static int    (*p_osg_set_uniform)(void *, const void *);  /* Vec4f&       */
static int    (*p_osg_uniform_get)(const void *, float *); /* get(Vec4f&) =
                                              * the in-view detector        */
static int    (*p_osg_uniform_type)(const void *);  /* Uniform::getType()    */
static void  *(*p_osg_get_or_create)(void *, const void *, int, unsigned);
                                             /* StateSet::getOrCreateUniform(
                                              * const std::string&, Type, n)  */
static void     (*p_osg_add_uniform)(void *, void *, unsigned);
                                             /* StateSet::addUniform(Uniform*,
                                              * overrideMask) - the only way to
                                              * say "nothing below me wins"    */
static void     (*p_osg_remove_uniform)(void *, const void *);
                                             /* StateSet::removeUniform(const
                                              * std::string&) - taking the entry
                                              * the module made away is what
                                              * hands the ring back to the
                                              * vendor's channel below         */
static volatile LONG g_color_cb_calls;
static volatile LONG g_color_missing_uniform;
static volatile LONG g_color_missing_binding;
static volatile LONG g_palette_markers;
static ULONGLONG      g_hb_tick;               /* render-side heartbeat     */

/* ---- the in-view outline -----------------------------------------------
 * The vendor signals "this marker is in the scanner's current frame" by
 * writing red into the per-child uniform `frontInnerColor`.  We read that
 * write back and spend it on a channel the quality palette never uses:
 * the marker's outline, `frontExternalColor`, which the vendor does not
 * change at runtime (live uniform dump: only u1 ever gets a red upload,
 * the outline stays black - docs/research/marker-pipeline.md).  The disc
 * keeps carrying the quality color, so both signals are readable at the
 * same time: steer by the outline, read quality off the disc.
 *
 * The vendor only ever paints the disc, and the ring channel it leaves behind
 * sits below the markers and is shared by them, so the module gives each marker
 * a channel of its own (the vendor's own getOrCreateUniform, armed with
 * OVERRIDE_VALUES), puts the highlight in it while the marker is in the
 * scanner's view and takes that entry away again when the marker leaves: the
 * vendor's own channel below paints the ring once more, with whatever value it
 * holds then (the channel code tells how it was measured).  Nothing is
 * invented and nothing can stay lit.
 *
 * The in-frame test below is the shared vendor contract, unchanged:
 *   R >= 0.55 && G <= 0.38 && B <= 0.38                                    */
static int           g_inframe       = 1;   /* 0 = stock look entirely    */
static int           g_inframe_create = 1;  /* make a channel for a marker
                                             * that has none (inframe_create)*/
static int           g_inframe_disc;        /* 1 = highlight the disc     */
static int           g_hide_red       = 1;  /* 1 = no vendor red flash    */
static uint32_t      g_hl_rgb   = 0x0078FFu;/* 0,120,255                  */
static float         g_hl_alpha = 1.0f;
static volatile LONG g_hl_outline;          /* outline written: in frame  */
static volatile LONG g_hl_restored;         /* outline written: its own   */
static volatile LONG g_hl_no_outline;       /* in frame, no channel to use */
static volatile LONG g_hl_channel_made;     /* channels the module created */
static volatile LONG g_disc_quality;        /* disc = quality color       */
static volatile LONG g_disc_blank;          /* disc = white, no data yet  */
static volatile LONG g_disc_highlight;      /* disc = highlight color     */

/* palette mirrored from the bridge file (hub side) */
static SRWLOCK   g_pal_lock = SRWLOCK_INIT;
static Snapshot  g_palette;
static int       g_palette_valid;
static ULONGLONG g_palette_read_attempt;
static ULONGLONG g_palette_log_tick;         /* throttle palette logging   */

static void palette_refresh(void)
{
    ULONGLONG now = GetTickCount64();
    Snapshot snap;
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read = 0;
    int i, painted = 0, logIt = 0;
    uint32_t seq = 0;

    if (now - g_palette_read_attempt < 500)
        return;
    g_palette_read_attempt = now;

    {
        wchar_t path[MAX_PATH];
        snap_path(path, MAX_PATH, SNAP_NAME);
        file = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            AcquireSRWLockExclusive(&g_pal_lock);
            g_palette_valid = 0;
            ReleaseSRWLockExclusive(&g_pal_lock);
            return;
        }
        if (!GetFileSizeEx(file, &size) ||
            size.QuadPart != (LONGLONG) sizeof snap ||
            !ReadFile(file, &snap, sizeof snap, &read, NULL) ||
            read != sizeof snap) {
            CloseHandle(file);
            return;
        }
        CloseHandle(file);
    }
    if (snap.magic != SNAP_MAGIC || snap.count != MAX_IDS)
        return;
    if (snap.frame < 0) {
        AcquireSRWLockExclusive(&g_pal_lock);
        g_palette_valid = 0;
        ReleaseSRWLockExclusive(&g_pal_lock);
        g_host->log("mq: palette cleared (new project)");
        return;
    }
    AcquireSRWLockExclusive(&g_pal_lock);
    if (!g_palette_valid || g_palette.seq != snap.seq) {
        memcpy(&g_palette, &snap, sizeof snap);
        g_palette_valid = 1;
        for (i = 0; i < MAX_IDS; i++)
            painted += (g_palette.argb[i] >> 24) == 255;
        InterlockedExchange(&g_palette_markers, painted);
        logIt = (now - g_palette_log_tick >= 5000);
        if (logIt)
            g_palette_log_tick = now;
        seq = g_palette.seq;
    }
    ReleaseSRWLockExclusive(&g_pal_lock);
    if (logIt && g_verbose)
        g_host->log("mq: palette seq=%u painted=%d", seq, painted);
}

/* the two uniform names of a marker child, in the layout MSVC uses for
 * std::string (measured and proven by tools\str_lab.c):
 *
 *     @0   union { char buffer[16]; char *pointer; }   16 bytes
 *     @16  size_t size
 *     @24  size_t capacity
 *
 * and the STL reads buffer when capacity <= 15, pointer otherwise.  So
 * "frontInnerColor", 15 characters, is described inline (capacity 15),
 * while "frontExternalColor", 18 characters, needs the pointer plus EIGHT
 * BYTES OF PADDING: without them `size` lands inside the union and the
 * capacity reads as zero, and every lookup of a name longer than 15
 * characters misses quietly (measured live 2026-11-27: the outline channel
 * looked absent for exactly this reason).  The capacity itself only has to
 * be above 15.
 * The string is never destroyed (this is C, and the vendor map lookup only
 * reads), so a pointer at static text is safe. */
struct sso_str { char text[16]; size_t size, capacity; };
struct ref_str { const void *ptr; size_t pad; size_t size, capacity; };

/* the shared vendor contract: red disc == node is in CURRENT_MARKER */
static int isVendorRed(const float *v)
{
    return v[0] >= 0.55f && v[1] <= 0.38f && v[2] <= 0.38f;
}

static void setVec4(void *uniform, uint32_t rgb, float alpha,
                    volatile LONG *counter)
{
    float v[4];

    v[0] = (float) (((rgb >> 16) & 255u) / 255.0);
    v[1] = (float) (((rgb >> 8)  & 255u) / 255.0);
    v[2] = (float) (( rgb        & 255u) / 255.0);
    v[3] = alpha;
    if (p_osg_set_uniform(uniform, v) && counter)
        InterlockedIncrement(counter);
}

/* the two uniform names of a marker, in the layout MSVC uses for std::string
 * (measured and proven by tools\str_lab.c): a 16-byte union
 * { buffer | pointer }, then size @16 and capacity @24, and the STL reads the
 * inline buffer when capacity <= 15.  "frontInnerColor" is 15 characters and
 * fits inline; "frontExternalColor" is 18 and has to be described as a heap
 * string, which needs eight bytes of padding between the pointer and the
 * size.  Both point at static text: the vendor lookup only reads. */
static const char RING_TEXT[] = "frontExternalColor";
static const struct sso_str NAME_DISC = { "frontInnerColor", 15, 15 };
static const struct ref_str NAME_RING = { RING_TEXT, 0, 18, 18 };

/* ------------------------------------------------------ the outline channel
 * Where the ring color comes from, read out of the vendor's own code: the
 * function that builds one marker, `Sn3D::RenderKit::MarkerPoint::createMarkerPoint`
 * (librenderkit.dll), does
 *
 *      StateSet *ss = geometry->getOrCreateStateSet();
 *      ss->addUniform(new osg::Uniform("frontExternalColor", param + 0x0c));
 *      ss->addUniform(new osg::Uniform("frontInnerColor",   param + 0x2c));
 *
 * so the ring color is not a setting of the whole program, and the vendor never
 * writes it again at runtime - its in-view flash only ever touches the disc.
 *
 * Where it sits in a running scene the live runs settled (2026-11-28): the map
 * of a marker's own StateSet holds ONLY frontInnerColor, and the ring's
 * frontExternalColor sits one StateSet BELOW it, with two different markers
 * walking down to the same uniform object.  That channel cannot carry a
 * per-marker signal - one write would light the whole field - so the marker is
 * given a channel of its own, which OSG lets a StateSet use against one below
 * it only with a bit: State::pushUniformList (include/osg/State, same code in
 * osg158-osg.dll at RVA 0x18003b770)
 *
 *      else if ((us.uniformVec.back().second & StateAttribute::OVERRIDE) &&
 *               !(aitr->second.second & StateAttribute::PROTECTED))
 *          // push existing back since override keeps the previous value.
 *
 * OVERRIDE_VALUES (2) on the entry, written by addUniform(Uniform*, mask),
 * which keys by uniform->getName() and stores `mask & 6`.  Worth noting what
 * that means for a channel this module makes: getOrCreateUniform (RVA 0x14dd40)
 * ends in addUniform(this, uniform, 1), and 1 & 6 is 0 - a fresh channel has no
 * override at all, which is why an earlier build of this module painted into it
 * and showed nothing.
 *
  * HOW THE CUE IS PAINTED - and why nothing is remembered between frames.  Two
  * live runs of 2026-11-28 measured the two ways of getting this wrong:
  *
  *   - a resolved uniform pointer kept across frames is a pointer to something
  *     the scene may no longer draw: 163 channels resolved for 49 markers, with
  *     207 highlight writes going nowhere, the outline visible for one moment;
  *   - a list of "the channels I made" is remembered state too, and it only
  *     shrinks if a marker is ever seen leaving the view.  Under a scanner a
  *     marker is not taken out of view, it is replaced by a new node with a new
  *     StateSet, so the list never shrank: 1025 entries for 49 markers, and the
  *     cue switched itself off at its own limit while the counters still looked
  *     healthy.
  *
  * So every frame the marker's own map is read again and acted on as it is now:
  *
  *   in view    the marker's own entry is what is written; when it is missing it
  *              is made (getOrCreateUniform, value type copied from the disc
  *              uniform) and armed with OVERRIDE_VALUES;
  *   out of view the entry is REMOVED (StateSet::removeUniform), which hands the
  *              ring back to the vendor's own channel below - with whatever
  *              value that channel holds at that moment, so nothing is frozen
  *              and no "not in view" color has to be invented;
  *   an entry that is not one this module painted is left alone entirely.  Which
  *              one that is, is read from the object rather than remembered: an
  *              entry holding the highlight color can only have been painted by
  *              this module, and one holding anything else is the vendor's, and
  *              this module has no honest color to put back into it.
  *
  * Guards: everything read out of the scene is checked (SEH around the calls,
  * colors required to be 0..1 before a read result is trusted), removal happens
  * only on a transition so the marker's map is not churned every frame, and the
  * cue is one `inframe = 0` away from a stock look. */
static SRWLOCK  g_chan_lock = SRWLOCK_INIT;
static int      g_chanReported;
static int      g_chanOffReported;
static int      g_chanDeepReported;
static int      g_chanRefused;                 /* a foreign object misbehaved:
                                                * stop, colors are unaffected */

/* --------------------------------------------------- the StateSet uniform map */
#define SS_UNIFORM_MAP  0xb0
#define NODE_LEFT       0x00
#define NODE_ROOT       0x08       /* the header's _Parent: where the tree is */
#define NODE_RIGHT      0x10
#define NODE_ISNIL      0x19
#define NODE_KEY        0x20
#define NODE_KEY_SIZE   0x30
#define NODE_KEY_CAP    0x38
#define NODE_VALUE      0x40       /* the value: ref_ptr<Uniform>            */
#define NODE_MASK       0x48       /* the entry's override bits, `mask & 6` in
                                    * addUniform: 2 = OVERRIDE_VALUES,
                                    * 4 = PROTECTED_VALUES                   */
#define NAMES_CAP       192
#define OVR_VALUES      2u         /* StateAttribute::OVERRIDE, the bit OSG
                                    * checks against a deeper StateSet        */

/* the key of one node as C text; 0 when it cannot be read */
static int node_key_name(const void *node, char *out, size_t cap)
{
    const char *data = NULL;
    char text[40];
    size_t size = 0, capacity = 0, n;

    if (out && cap)
        out[0] = '\0';
    if (!safe_read(&data, (const char *) node + NODE_KEY, 8) ||
        !safe_read(&size, (const char *) node + NODE_KEY_SIZE, 8) ||
        !safe_read(&capacity, (const char *) node + NODE_KEY_CAP, 8) ||
        size > sizeof text - 1)
        return 0;
    if (capacity > 15) {                      /* heap: the union is a pointer */
        if (!safe_read(text, data, size))
            return 0;
    } else {                                  /* the 16-byte inline buffer    */
        if (!safe_read(text, (const char *) node + NODE_KEY, size))
            return 0;
    }
    text[size] = '\0';
    if (out && cap) {
        n = size < cap - 1 ? size : cap - 1;
        memcpy(out, text, n);
        out[n] = '\0';
    }
    return 1;
}

/* in-order walk, bounded.  With `want` it stops at the first node of that name
 * and reports the uniform and the entry's override bits; with want == NULL it
 * only collects the names for the log. */
static void umap_walk(const void *node, int depth, const char *want,
                      void **found, unsigned *mask, char *names, int *nNames)
{
    char name[40] = "";
    const void *child;
    char isNil = 1;

    if (!node || depth <= 0 || *found)
        return;                    /* a name appears once in a map           */
    if (!safe_read(&isNil, (const char *) node + NODE_ISNIL, 1) || isNil)
        return;                    /* the header is also the sentinel        */
    if (safe_read(&child, (const char *) node + NODE_LEFT, 8))
        umap_walk(child, depth - 1, want, found, mask, names, nNames);
    if (*found)
        return;
    if (node_key_name(node, name, sizeof name)) {
        if (names && nNames && *nNames < 10) {
            size_t len = strlen(names);
            size_t add = strlen(name);
            if (len + add + 3 < NAMES_CAP) {
                if (len)
                    names[len++] = ' ';
                memcpy(names + len, name, add);
                names[len + add] = '\0';
            }
            (*nNames)++;
        }
        if (want && strcmp(name, want) == 0) {
            safe_read(found, (const char *) node + NODE_VALUE, 8);
            if (*found && mask)
                safe_read(mask, (const char *) node + NODE_MASK, 4);
        }
    }
    if (safe_read(&child, (const char *) node + NODE_RIGHT, 8))
        umap_walk(child, depth - 1, want, found, mask, names, nNames);
}

/* the entry named `want` in THIS StateSet's map: its uniform, its override
 * bits (mask, optional) and, in names, everything the map holds.
 *
 * The map is read by hand because StateSet::getUniform, the vendor's own
 * lookup, answers NULL for this name on every marker live while the ring still
 * paints - and because getUniform cannot report the override bits, which here
 * are the whole question.  The layout is not guessed: it is how
 * getUniform@StateSet@osg (RVA 0x14e310) walks the map and how
 * addUniform@StateSet@osg (RVA 0x14c2f0) fills it.
 *
 * The trap this nearly cost a third live round: the module also walks Qt's
 * QMap on the scan side, whose node offsets are 8/16/24/32.  Those offsets on
 * a std::map find nothing - and once produced a confident wrong conclusion. */
static void *map_lookup(void *states, const char *want, unsigned *mask,
                        char *names)
{
    const void *header = NULL, *root = NULL;
    void *found = NULL;
    int n = 0;

    if (names)
        names[0] = '\0';
    if (mask)
        *mask = 0;
    if (!states ||
        !safe_read(&header, (const char *) states + SS_UNIFORM_MAP, 8) ||
        !safe_read(&root, (const char *) header + NODE_ROOT, 8))
        return NULL;
    umap_walk(root, 24, want, &found, mask, names, &n);
    return found;
}

/* Creates the channel where a marker's own belongs: on the marker's own
 * StateSet, with the value TYPE copied from the disc uniform rather than
 * guessed.  getOrCreateUniform inserts it with no override bits, so the
 * arming is done here - without it the channel loses to the vendor's deeper
 * one, which is exactly what an earlier build of this module did. */
static void *ring_create(void *states, void *disc)
{
    void *u = NULL;
    int type;

    if (!p_osg_get_or_create || !p_osg_uniform_type || !states || !disc)
        return NULL;
    __try {
        type = p_osg_uniform_type(disc);       /* FLOAT_VEC4 in practice      */
        if (type < 0)
            return NULL;
        u = p_osg_get_or_create(states, &NAME_RING, type, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (!u)
        return NULL;
    if (!p_osg_add_uniform) {
        g_host->log("mq: outline: StateSet::addUniform is not resolved, so a "
                    "channel of the marker's own cannot be armed against the "
                    "vendor's deeper one; the outline stays off");
        return NULL;
    }
    __try {
        p_osg_add_uniform(states, u, OVR_VALUES);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_chanRefused = 1;
        return NULL;
    }
    return u;
}

/* Takes the channel away again.  removeUniform is the vendor's own call, and it
 * is what makes "out of view" the stock ring: with the marker's own entry gone,
 * the channel below the marker paints again, with the value the vendor gave it
 * at that moment. */
static void ring_remove(void *states, void *u)
{
    if (!states || !p_osg_remove_uniform)
        return;
    __try {
        p_osg_remove_uniform(states, &NAME_RING);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_chanRefused = 1;
        return;
    }
}

static void chan_report(void *states, void *u)
{
    char names[NAMES_CAP] = "";

    if (g_chanReported)
        return;
    g_chanReported = 1;
    map_lookup(states, NULL, NULL, names);
    g_host->log("mq: outline: made on the marker's own StateSet - channel %p, "
                "armed with OVERRIDE_VALUES (the marker's map now holds [%s])",
                u, names[0] ? names : "(empty)");
}

static void chan_report_off(const char *how)
{
    if (g_chanOffReported)
        return;
    g_chanOffReported = 1;
    g_host->log("mq: outline: %s; the in-view cue stays off, colors "
                "unaffected", how);
}

/* The reads and the writes go through SEH, and a value read is only trusted
 * when it is a color: the objects come out of the scene, not out of a call
 * this module made. */
static int ring_read(void *u, float *v)
{
    int i, ok = 0;

    if (!p_osg_uniform_get)
        return 0;
    __try {
        ok = p_osg_uniform_get(u, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_chanRefused = 1;
        return 0;
    }
    if (!ok)
        return 0;
    for (i = 0; i < 4; i++)
        if (!(v[i] >= -0.001f && v[i] <= 1.001f))
            return 0;
    return 1;
}

static int ring_write(void *u, const float *v)
{
    int ok = 0;

    __try {
        ok = p_osg_set_uniform(u, v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_chanRefused = 1;
        return 0;
    }
    return ok;
}

/* One look down the marker's tree, for the log only: where the vendor keeps the
 * ring this module is overriding, and with which bits.  Bounded, SEH, and it
 * says PROTECTED if that is what the vendor marked - the one thing an override
 * above it cannot beat, in which case the log states that the outline cannot be
 * made per marker here instead of leaving it a silent nothing. */
static void ring_report_deeper(void *node, int depth)
{
    void *states, *u;
    unsigned n, i, mask = 0;

    if (g_chanDeepReported || !node || depth <= 0)
        return;
    if (!p_osg_num_children || !p_osg_get_child)
        return;
    __try {
        n = p_osg_num_children(node);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++) {
        void *part = NULL;

        __try {
            part = p_osg_get_child(node, i);
            states = part ? p_osg_get_state_set(part) : NULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (!states)
            continue;
        u = map_lookup(states, RING_TEXT, &mask, NULL);
        if (u) {
            float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            g_chanDeepReported = 1;
            if (ring_read(u, v))
                g_host->log("mq: outline: the vendor's ring color comes from "
                            "%p, below the marker: %.2f %.2f %.2f alpha %.2f, "
                            "entry override bits %u%s",
                            u, v[0], v[1], v[2], v[3], mask,
                            (mask & 4u)
                                ? " - PROTECTED, so nothing above it can win "
                                  "and the outline stays off"
                                : (mask & 2u)
                                    ? " - the vendor's own OVERRIDE"
                                    : "");
            else
                g_host->log("mq: outline: the vendor's ring color comes from "
                            "%p, below the marker, but its value could not be "
                            "read; entry override bits %u", u, mask);
            return;
        }
        ring_report_deeper(part, depth - 1);
        if (g_chanDeepReported)
            return;
    }
}

/* Is this the channel this module paints, or the vendor's own?  The answer is
 * read from the object and not remembered: an entry that holds the highlight
 * color can only have been painted by this module, and one that holds anything
 * else belongs to the vendor and is left alone.
 *
 * A remembered list was the first attempt and it cannot work here.  A marker
 * under the scanner is not taken out of view, it is replaced by a new node with
 * a new StateSet, so an entry that was added to a list is never seen leaving and
 * the list fills up: the live run of 2026-11-28 measured 1025 entries for 49
 * markers and then switched the cue off by its own guard.  Nothing is kept
 * across frames now. */
static void hl_color(float *v)
{
    v[0] = (float) ((g_hl_rgb >> 16) & 255u) / 255.0f;
    v[1] = (float) ((g_hl_rgb >> 8)  & 255u) / 255.0f;
    v[2] = (float) ( g_hl_rgb        & 255u) / 255.0f;
    v[3] = g_hl_alpha;
}

static int ring_is_ours(void *u, const float *hl)
{
    float v[4];
    int i;

    if (!ring_read(u, v))
        return 0;
    for (i = 0; i < 4; i++)
        if (v[i] - hl[i] > 0.001f || hl[i] - v[i] > 0.001f)
            return 0;
    return 1;
}

/* One marker, one frame, and nothing carried into it from the last frame: the
 * marker's own map is read again and acted on as it is now, because the vendor
 * replaces markers while it scans and a pointer kept from before that points at
 * something the scene no longer draws.
 *
 *   in view    the marker's own entry is painted, made and armed with
 *              OVERRIDE_VALUES when it is missing;
 *   out of view an entry this module painted is REMOVED, which hands the ring
 *              back to the vendor's channel below with whatever value that
 *              channel holds then; an entry that is not ours is left alone;
 *   nothing    nothing to do, nothing to undo. */
static void outline_one(void *marker, void *states, void *disc, int inFrame)
{
    float hl[4], v[4];
    unsigned mask = 0;
    void *u;

    hl_color(hl);
    AcquireSRWLockExclusive(&g_chan_lock);
    if (!g_chanRefused) {
        u = map_lookup(states, RING_TEXT, &mask, NULL);

        if (u && !ring_is_ours(u, hl)) {
            /* not ours: the vendor owns this marker's ring, and there is no
             * color of ours that would be honest to put back into it */
            chan_report_off("a marker carries its own \"frontExternalColor\", "
                            "which this module does not own and therefore "
                            "leaves to the vendor");
            goto done;
        }

        if (inFrame) {
            if (!u) {
                if (!g_inframe_create) {
                    InterlockedIncrement(&g_hl_no_outline);
                    goto done;
                }
                ring_report_deeper(marker, 3);
                u = ring_create(states, disc);
                if (!u) {
                    if (!g_chanRefused)
                        chan_report_off("a channel could not be made on the "
                                        "marker's own StateSet");
                    goto done;
                }
                InterlockedIncrement(&g_hl_channel_made);
                mask = OVR_VALUES;
            }
            v[0] = hl[0]; v[1] = hl[1]; v[2] = hl[2]; v[3] = hl[3];
            if (ring_write(u, v)) {
                /* a StateSet the scene rebuilt would have lost the bit */
                if ((mask & 6u) != OVR_VALUES && p_osg_add_uniform) {
                    __try {
                        p_osg_add_uniform(states, u, OVR_VALUES);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        g_chanRefused = 1;
                        goto done;
                    }
                }
                chan_report(states, u);
                InterlockedIncrement(&g_hl_outline);
            }
        } else if (u) {
            ring_remove(states, u);            /* the vendor's ring paints     */
            InterlockedIncrement(&g_hl_restored);
        }
    }
done:
    ReleaseSRWLockExclusive(&g_chan_lock);
}

static void hook_color_update(void *owner)
{
    LONG n;
    uintptr_t group = 0, flags = 0, vr[2] = { 0, 0 };
    unsigned char vis[MAX_IDS];
    size_t count, i;
    const Binding *bind = NULL;
    int palValid;

    if (g_orig_color_update)
        g_orig_color_update(owner);          /* vendor paints first        */
    if (!g_paint_side || !owner)
        return;

    n = InterlockedIncrement(&g_color_cb_calls);
    if (n == 1)
        g_host->log("mq: colorCb first call owner=%p", owner);
    {
        ULONGLONG now = GetTickCount64();
        if (g_verbose && now - g_hb_tick >= 30000) {
            g_hb_tick = now;                 /* else: one line per frame   */
            g_host->log("mq: render hb pid=%lu cb=%ld qdisc=%ld blank=%ld "
                        "discHl=%ld outline=%ld restored=%ld noChannel=%ld "
                        "made=%ld noDisc=%ld noBinding=%ld palette=%d/%d",
                        (unsigned long) GetCurrentProcessId(), (long) n,
                        (long) g_disc_quality, (long) g_disc_blank,
                        (long) g_disc_highlight, (long) g_hl_outline,
                        (long) g_hl_restored, (long) g_hl_no_outline,
                        (long) g_hl_channel_made,
                        (long) g_color_missing_uniform,
                        (long) g_color_missing_binding,
                        (int) g_palette_valid, (int) g_palette_markers);
        }
    }

    palette_refresh();
    if (!safe_read(&group, (const char *) owner + 8, 8) || !group ||
        !safe_read(&flags, (const char *) owner + 16, 8) || !flags)
        return;
    if (!safe_read(vr, (const void *) (flags + 0x88), 16) ||
        vr[1] <= vr[0] || (vr[1] - vr[0]) > MAX_IDS)
        return;
    count = vr[1] - vr[0];                   /* i8 visibility per marker   */
    if (p_osg_num_children &&
        p_osg_num_children((void *) group) != (unsigned) count)
        return;
    if (!safe_read(vis, (const void *) vr[0], count))
        return;

    AcquireSRWLockShared(&g_bind_lock);
    for (i = 0; i < N_BINDINGS; i++)
        if (g_bindings[i].flags == flags && g_bindings[i].count == count) {
            bind = &g_bindings[i];
            break;
        }
    if (!bind) {
        InterlockedIncrement(&g_color_missing_binding);
        if (g_verbose && n <= 8)
            g_host->log("mq: colorCb no binding for flags=%p count=%zu "
                        "(no quality colors for this group, the outline "
                        "is independent of it)",
                        (void *) flags, count);
    }

    AcquireSRWLockShared(&g_pal_lock);
    palValid = g_palette_valid;

    for (i = 0; i < count; i++) {
        void *child, *states, *disc;
        int inFrame = 0;
        uint32_t argb = 0;

        if ((int8_t) vis[i] < 0)
            continue;                        /* hidden marker              */
        __try {
            child   = p_osg_get_child((void *) group, (unsigned) i);
            states  = child ? p_osg_get_state_set(child) : NULL;
            disc    = states ? p_osg_get_uniform(states, &NAME_DISC) : NULL;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;                           /* the chain broke: stop here */
        }
        if (!disc) {
            InterlockedIncrement(&g_color_missing_uniform);
            continue;
        }

        /* what did the vendor write into the disc THIS frame? */
        if (g_inframe && p_osg_uniform_get) {
            float cur[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            if (p_osg_uniform_get(disc, cur) && isVendorRed(cur))
                inFrame = 1;
        }

        /* ---- the disc belongs to the quality stages ---- */
        if (bind && palValid) {
            uint64_t id = bind->ids[i];

            if (id < MAX_IDS)
                argb = g_palette.argb[id];
        }
        if ((argb >> 24) == 255) {
            setVec4(disc, argb, 1.0f, &g_disc_quality);
        } else if (inFrame) {
            /* no quality data for this marker: the vendor red would flash
             * here, which is exactly the signal the outline now carries */
            if (g_inframe && g_inframe_disc)
                setVec4(disc, g_hl_rgb, g_hl_alpha, &g_disc_highlight);
            else if (g_hide_red)
                setVec4(disc, 0x00FFFFFFu, 1.0f, &g_disc_blank);
        }

        /* ---- the outline carries "in the scanner's view" ---- */
        if (g_inframe && !g_inframe_disc)
            outline_one(child, states, disc, inFrame);
    }
    ReleaseSRWLockShared(&g_pal_lock);
    ReleaseSRWLockShared(&g_bind_lock);
}

static int scan_color_slot(HMODULE mod)
{
    static const unsigned char sig[] = {
        0x48, 0x83, 0xC1, 0x08, 0xE9, 0x77, 0xED, 0xFF, 0xFF
    };
    unsigned char *base = (unsigned char *) mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *) mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    uintptr_t targets[8];
    int nTargets = 0, nSlots = 0, i, nsec;
    void *volatile *found = NULL;

    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS *) (base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    sec = IMAGE_FIRST_SECTION(nt);
    nsec = nt->FileHeader.NumberOfSections;

    /* 1. find the stub in executable sections */
    for (i = 0; i < nsec; i++) {
        unsigned char *p, *e;
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        p = base + sec[i].VirtualAddress;
        e = p + sec[i].Misc.VirtualSize;
        if (e < p + sizeof sig)
            continue;
        for (; p + sizeof sig <= e; p++) {
            if (memcmp(p, sig, sizeof sig) == 0) {
                if (nTargets < 8)
                    targets[nTargets] = (uintptr_t) p;
                nTargets++;
            }
        }
    }
    if (!nTargets) {
        g_host->log("mq: color stub signature not found in renderer");
        return 0;
    }
    if (nTargets != 1) {
        g_host->log("mq: %d stub candidates (need exactly 1), not hooking",
                    nTargets);
        return 0;
    }
    g_host->log("mq: color stub at renderer+0x%llX",
                (unsigned long long) (targets[0] - (uintptr_t) base));

    /* 2. find the single qword (any non-code section; vendor slot lives
        * in .rdata) pointing at the stub */
    for (i = 0; i < nsec; i++) {
        unsigned char *p, *e;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;
        p = base + sec[i].VirtualAddress;
        e = p + sec[i].Misc.VirtualSize;
        for (; p + 8 <= e; p += 8) {
            uintptr_t v = 0;
            if (!safe_read(&v, p, 8))
                continue;
            if (v == targets[0]) {
                g_host->log("mq: color slot at renderer+0x%llX -> stub",
                            (unsigned long long) ((uintptr_t) p -
                                                  (uintptr_t) base));
                found = (void *volatile *) p;
                nSlots++;
            }
        }
    }
    if (nSlots != 1) {
        g_host->log("mq: color slot candidates=%d (need exactly 1), not hooking",
                    nSlots);
        return 0;
    }

    /* 3. swap it (in-memory only; original kept for the pass-through call) */
    {
        DWORD protect, unused;
        if (!VirtualProtect((void *) found, 8, PAGE_READWRITE, &protect))
            return 0;
        g_orig_color_update = (void (*)(void *)) *found;
        InterlockedExchangePointer((PVOID volatile *) found,
                                   (PVOID) hook_color_update);
        VirtualProtect((void *) found, 8, protect, &unused);
        g_host->log("mq: color slot hooked");
        /* The process that paints owns the session clean-up: drop a stale
         * bridge file here (not at module start) so sibling helpers that
         * spawn mid-session cannot wipe a live palette. */
        snap_delete();
        return 1;
    }
}

static int osg_exports_ready(HMODULE osg)
{
    p_osg_get_child = (void *(*)(void *, unsigned))
        GetProcAddress(osg, "?getChild@Group@osg@@QEAAPEAVNode@2@I@Z");
    p_osg_num_children = (unsigned (*)(void *)) (void *)
        GetProcAddress(osg, "?getNumChildren@Group@osg@@UEBAIXZ");
    p_osg_get_state_set = (void *(*)(void *))
        GetProcAddress(osg, "?getStateSet@Node@osg@@QEAAPEAVStateSet@2@XZ");
    p_osg_get_uniform = (void *(*)(void *, const void *))
        GetProcAddress(osg, "?getUniform@StateSet@osg@@QEAAPEAVUniform@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    p_osg_set_uniform = (int (*)(void *, const void *)) (void *)
        GetProcAddress(osg, "?set@Uniform@osg@@QEAA_NAEBVVec4f@2@@Z");
    /* optional: reading the vendor value is what detects "marker in
     * view"; without it the module still paints the quality colors, it
     * just leaves the vendor's in-view flash alone */
    p_osg_uniform_get = (int (*)(const void *, float *)) (void *)
        GetProcAddress(osg, "?get@Uniform@osg@@QEBA_NAEAVVec4f@2@@Z");
    /* Optional.  The outline cue needs a channel, and the live runs of
     * 2026-11-27/28 showed what it takes: the name lookup is not enough to
     * find one, the channel that does exist sits below the marker and is
     * shared, so the module reads the StateSet's uniform map itself (see the
     * channel code) and paints a channel of the marker's OWN, made the way the
     * vendor makes them - getOrCreateUniform with the `Type` copied from the
     * disc uniform rather than guessed - and given OVERRIDE_VALUES by
     * addUniform, which is the only thing in OSG that lets it win against the
     * shared one below.  What was in it is remembered, so "out of view" is that
     * value and not a color this module invented. */
    p_osg_uniform_type = (int (*)(const void *)) (void *)
        GetProcAddress(osg, "?getType@Uniform@osg@@QEBA?AW4Type@12@XZ");
    p_osg_get_or_create =
        (void *(*)(void *, const void *, int, unsigned)) (void *)
        GetProcAddress(osg, "?getOrCreateUniform@StateSet@osg@@QEAAPEAVUnif"
        "orm@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@"
        "std@@W4Type@32@I@Z");
    p_osg_add_uniform = (void (*)(void *, void *, unsigned)) (void *)
        GetProcAddress(osg, "?addUniform@StateSet@osg@@QEAAXPEAVUniform@2@I@Z");
    p_osg_remove_uniform = (void (*)(void *, const void *)) (void *)
        GetProcAddress(osg, "?removeUniform@StateSet@osg@@QEAAXAEBV?$basic_"
        "string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    return p_osg_get_child && p_osg_num_children && p_osg_get_state_set &&
           p_osg_get_uniform && p_osg_set_uniform;
}

/* ------------------------------------------------------------------ */
/* Installation                                                        */
/* ------------------------------------------------------------------ */
static void install_thread(void *unused)
{
    HMODULE qt = NULL, file = NULL, render = NULL, osg = NULL;
    int i, qt_done = 0, put_done = 0, smp_done = 0, slot_done = 0;

    (void) unused;
    /* loop until everything this process can offer is wired up; modules
     * load lazily, so poll for up to ~5 minutes, then exit quietly (a
     * process without the target DLLs simply never hosts this module). */
    for (i = 0; i < 600 && !(qt_done && put_done && smp_done && slot_done);
         i++) {
        static int putTry, smpTry, qtTries;
        if (!qt)
            qt = GetModuleHandleW(L"Qt5Core.dll");
        if (qt && !qt_done) {
            p_userType = (f_variant_type) GetProcAddress(qt, "?userType@QVariant@@QEBAHXZ");
            p_constData = (f_variant_data) GetProcAddress(qt, "?constData@QVariant@@QEBAPEBXXZ");
            p_toInt = (f_variant_int) GetProcAddress(qt, "?toInt@QVariant@@QEBAHPEA_N@Z");
            p_strSize = (f_str_size) GetProcAddress(qt, "?size@QString@@QEBAHXZ");
            p_strUtf16 = (f_str_utf16) GetProcAddress(qt, "?utf16@QString@@QEBAPEBGXZ");
            g_map_null = GetProcAddress(qt, "?shared_null@QMapDataBase@@2U1@B");
            qt_done = p_userType && p_constData && p_toInt && p_strSize &&
                      p_strUtf16 && g_map_null;
            if (!qt_done && ++qtTries > 10) {
                qt_done = 1;                 /* deterministic: stop trying */
                g_host->log("mq: Qt helpers incomplete on this Qt build, "
                            "scan side disabled");
            }
        }
        if (!file)
            file = GetModuleHandleW(L"Sn3DFile.dll");
        if (!g_scan_side) {
            put_done = 1;
        } else if (file && !put_done) {
            void *target = GetProcAddress(file, EXP_PUT_FRAME);
            if (!target) {
                put_done = 1;                /* renamed export: no retry   */
                g_host->log("mq: putFrameDataIntoFile export not found, "
                            "scan side off");
            } else if (g_host->hook(target, hook_put_frame,
                                     (void **) &tramp_put_frame)) {
                g_host->log("mq: putFrameDataIntoFile detoured");
                put_done = 1;
            } else if (++putTry > 10) {
                put_done = 1;
                g_host->log("mq: putFrame hook failed 10x, giving up");
            }
        }
        if (!render)
            render = GetModuleHandleW(L"librenderkit.extensions.digit.dll");
        if (!render && !g_paint_side) {
            smp_done = slot_done = 1;
        } else if (render && !smp_done && g_paint_side) {
            void *target = GetProcAddress(render, EXP_SET_MARKERS);
            if (!target) {
                smp_done = 1;                /* renamed export: no retry   */
                g_host->log("mq: setMarkerPoints export not found, "
                            "bindings off");
            } else if (g_host->hook(target, hook_set_marker_points,
                                     (void **) &tramp_set_markers)) {
                smp_done = 1;
                g_host->log("mq: setMarkerPoints detoured");
            } else if (++smpTry > 10) {
                smp_done = 1;
                g_host->log("mq: setMarkerPoints hook failed 10x, giving up");
            }
        }
        if (!osg && render)
            osg = GetModuleHandleW(L"osg158-osg.dll");
        if (render && osg && !slot_done && g_paint_side) {
            if (osg_exports_ready(osg)) {
                if (scan_color_slot(render))
                    slot_done = 1;
                else
                    slot_done = 1;           /* definitive no: stop retry  */
            }
        } else if (!g_paint_side) {
            slot_done = 1;
        }
        if (!(qt_done && put_done && smp_done && slot_done))
            Sleep(500);
    }
    g_host->log("mq: install done pid=%lu qt=%d put=%d smp=%d slot=%d "
                "render=%p",
                (unsigned long) GetCurrentProcessId(), qt_done, put_done,
                smp_done, slot_done, render);
    if (g_paint_side) {
        if (g_inframe && !p_osg_uniform_get) {
            g_host->log("mq: in-view cue disabled - osg Uniform::get not "
                        "resolved (the red test needs it); colors unaffected");
            g_inframe = 0;
        }
        g_host->log("mq: in-view cue armed where=%s rgb=%u,%u,%u alpha=%.2f "
                    "hideRed=%d create=%d%s",
                    g_inframe ? (g_inframe_disc ? "disc" : "outline") : "off",
                    (unsigned) ((g_hl_rgb >> 16) & 255u),
                    (unsigned) ((g_hl_rgb >> 8) & 255u),
                    (unsigned) (g_hl_rgb & 255u),
                    g_hl_alpha, g_hide_red, g_inframe_create,
                     (!g_inframe || g_inframe_disc) ? ""
                     : (p_osg_get_or_create && p_osg_uniform_type &&
                        p_osg_add_uniform && p_osg_remove_uniform)
                         ? ""
                         : " (StateSet::getOrCreateUniform, Uniform::getType, "
                           "StateSet::addUniform or StateSet::removeUniform not "
                           "resolved: the outline is a channel of the marker's "
                           "own that overrides the shared one and is taken away "
                           "again, so the cue stays off)");
    }
}

/* ------------------------------------------------------------------ */
/* Module ABI                                                          */
/* ------------------------------------------------------------------ */
static uint32_t pack_argb(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xff000000u | ((uint32_t) r << 16) | ((uint32_t) g << 8) |
           (uint32_t) b;
}

EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    static const ExModuleInfo info = {
        EX_ABI_VERSION, MOD_ID, "Marker Quality", MOD_VER,
        "marker color = tracking quality, colored outline = marker in view "
        "(scan->hub bridge)"
    };
    return &info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    HANDLE th;

    g_host = host;
    g_host->log("mq: module start, exe=marker_quality pid=%lu",
                (unsigned long) GetCurrentProcessId());

    g_green_frames     = host->cfg_int(host, "green_frames", 20);
    g_green_spread_um  = host->cfg_int(host, "green_spread_um", 100);
    g_green_supported  = host->cfg_int(host, "green_supported", 4);
    g_yellow_frames    = host->cfg_int(host, "yellow_frames", 8);
    g_yellow_supported = host->cfg_int(host, "yellow_supported", 2);
    g_report_ms        = host->cfg_int(host, "report_ms", 5000);
    g_publish_ms       = host->cfg_int(host, "publish_ms", 300);
    g_verbose          = host->cfg_int(host, "verbose", 1);
    g_scan_side        = host->cfg_int(host, "scan_side", 1);
    g_paint_side       = host->cfg_int(host, "paint_side", 1);
    {   /* the in-view cue: outline (default) or disc, see the
         * "in-view outline" block above */
        char buf[64];

        g_inframe  = host->cfg_int(host, "inframe", 1);
        g_hide_red = host->cfg_int(host, "hide_red", 1);
        g_inframe_create = host->cfg_int(host, "inframe_create", 1);
        g_inframe_disc =
            host->cfg_string(host, "inframe_where", buf, sizeof buf) &&
            _stricmp(buf, "disc") == 0;
        g_hl_rgb   = pack_argb(host->cfg_int(host, "inframe_r", 0),
                               host->cfg_int(host, "inframe_g", 120),
                               host->cfg_int(host, "inframe_b", 255));
        g_hl_alpha = (float) host->cfg_int(host, "inframe_a", 255) / 255.0f;
        if (g_hl_alpha < 0.0f) g_hl_alpha = 0.0f;
        if (g_hl_alpha > 1.0f) g_hl_alpha = 1.0f;
    }
    if (g_green_frames < 1)   g_green_frames = 1;
    if (g_green_spread_um < 1) g_green_spread_um = 1;
    if (g_green_supported < 1) g_green_supported = 1;
    if (g_yellow_frames < 1)  g_yellow_frames = 1;
    if (g_publish_ms < 100)   g_publish_ms = 100;

    /* dark red on purpose: R=128/255=0.50 sits below the 0.55 threshold of
     * the vendor-red detector above, so a marker that is merely painted
     * quality-red never reads back as "in the scanner's view" */
    g_argb[0] = pack_argb(host->cfg_int(host, "red_r",   128),
                          host->cfg_int(host, "red_g",   24),
                          host->cfg_int(host, "red_b",   24));
    g_argb[1] = pack_argb(host->cfg_int(host, "yellow_r", 255),
                          host->cfg_int(host, "yellow_g", 200),
                          host->cfg_int(host, "yellow_b", 25));
    g_argb[2] = pack_argb(host->cfg_int(host, "green_r",  40),
                          host->cfg_int(host, "green_g", 215),
                          host->cfg_int(host, "green_b",  60));

    ensure_snap_dir();                       /* bridge folder, idempotent  */

    th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) install_thread,
                      NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        g_host->log("mq: install thread creation failed");
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* Intentionally empty.  The app is going down: unhooting the vendor
     * slot or the MinHook detours here would race with threads that are
     * still rendering, which is strictly worse than leaving them in place. */
}
