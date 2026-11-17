/*
 * marker_quality.c - paint tracking markers red / yellow / green according
 * to how well the scanner has observed them, EXStar Hub 3.1.49.
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
 *     the original stub and then override the per-child osg uniform
 *     "frontInnerColor" with the quality color for that marker id.
 *     Marker id per child index comes from bindings captured on
 *     setMarkerPoints (render chain node+0x150 -> mix, +0x138 -> geom,
 *     geom+0x220 -> key vector, geom+0x248 -> state -> +8 visibility).
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
#define MOD_VER     "1.0.2"

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
static int    (*p_osg_uniform_get)(const void *, float *); /* get(Vec4f&)  */
static volatile LONG g_color_cb_calls;
static volatile LONG g_color_writes;
static volatile LONG g_color_missing_uniform;
static volatile LONG g_color_missing_binding;
static volatile LONG g_palette_markers;
static ULONGLONG      g_hb_tick;               /* render-side heartbeat     */

/* ---- in-frame sharing with marker_inframe ---------------------------- *
 * Both modules drive the same osg uniform `frontInnerColor`.  To keep
 * marker_inframe working (it enlarges in-frame markers), we do NOT hide
 * the vendor red from it: a marker the vendor just painted red (= in the
 * current scanner frame) gets its quality color with the green byte +1
 * (invisible, 1/255).  marker_inframe detects the in-frame marker by an
 * exact float match against the tagged colors exported below.
 * Contract shared with modules\marker_inframe\marker_inframe.c:
 *   - vendor red test:      R >= 0.55 && G <= 0.38 && B <= 0.38
 *   - ex_mod_inframe_snapshot() -> distinct float4 colors in use now.
 * Same GL thread writes (this callback) and reads (uniform upload), and
 * the callback runs before the frame's draws - no locking needed.      */
static int           g_share_inframe = 1;
static volatile LONG g_color_tagged;
static uint32_t      g_tag_argb[3];            /* collected during a pass */
static float         g_tag_vals[3 * 4];        /* published tuples        */
static volatile LONG g_tag_count;

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

static void hook_color_update(void *owner)
{
    LONG n;
    uintptr_t group = 0, flags = 0, vr[2] = { 0, 0 };
    unsigned char vis[MAX_IDS];
    size_t count, i;
    int nTagged = 0;
    const Binding *bind = NULL;

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
            g_hb_tick = now;
            g_host->log("mq: render hb pid=%lu cb=%ld writes=%ld noUniform=%ld "
                        "noBinding=%ld palette=%d/%d tagged=%ld",
                        (unsigned long) GetCurrentProcessId(), (long) n,
                        (long) g_color_writes, (long) g_color_missing_uniform,
                        (long) g_color_missing_binding,
                        (int) g_palette_valid, (int) g_palette_markers,
                        (long) g_color_tagged);
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
        ReleaseSRWLockShared(&g_bind_lock);
        InterlockedIncrement(&g_color_missing_binding);
        if (g_verbose && n <= 8)
            g_host->log("mq: colorCb no binding for flags=%p count=%zu",
                        (void *) flags, count);
        return;
    }

    AcquireSRWLockShared(&g_pal_lock);
    if (!g_palette_valid) {
        ReleaseSRWLockShared(&g_pal_lock);
        ReleaseSRWLockShared(&g_bind_lock);
        return;
    }
    nTagged = 0;
    for (i = 0; i < count; i++) {
        uint64_t id = bind->ids[i];
        uint32_t argb, use;
        float rgba[4];
        void *child, *states, *uniform;
        /* MSVC x64 std::string; "frontInnerColor" (15 chars) fits the SSO
         * buffer: { buf[16], size, capacity }                                */
        struct { char text[16]; size_t size, capacity; } name =
            { "frontInnerColor", 15, 15 };

        if ((int8_t) vis[i] < 0)
            continue;                        /* hidden marker              */
        if (id >= MAX_IDS)
            continue;
        argb = g_palette.argb[id];
        if ((argb >> 24) != 255)
            continue;                        /* unseen -> vendor color     */
        use = argb;
        __try {
            child   = p_osg_get_child((void *) group, (unsigned) i);
            states  = child ? p_osg_get_state_set(child) : NULL;
            uniform = states ? p_osg_get_uniform(states, &name) : NULL;
            if (!uniform) {
                InterlockedIncrement(&g_color_missing_uniform);
            } else {
                if (g_share_inframe && p_osg_uniform_get) {
                    /* what did the vendor just write here? red = in frame */
                    float cur[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    if (p_osg_uniform_get(uniform, cur) &&
                        cur[0] >= 0.55f && cur[1] <= 0.38f &&
                        cur[2] <= 0.38f) {
                        uint32_t t = argb, k;
                        if (((argb >> 8) & 255u) < 255u)
                            t += 1u << 8;    /* invisible in-frame tag     */
                        for (k = 0; k < (uint32_t) nTagged; k++)
                            if (g_tag_argb[k] == t)
                                break;
                        if (k == (uint32_t) nTagged && nTagged < 3)
                            g_tag_argb[nTagged++] = t;
                        use = t;
                    }
                }
                rgba[0] = (float) (((use >> 16) & 255u) / 255.0);
                rgba[1] = (float) (((use >> 8)  & 255u) / 255.0);
                rgba[2] = (float) (( use        & 255u) / 255.0);
                rgba[3] = 1.0f;
                if (p_osg_set_uniform(uniform, rgba)) {
                    InterlockedIncrement(&g_color_writes);
                    if (use != argb)
                        InterlockedIncrement(&g_color_tagged);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    ReleaseSRWLockShared(&g_pal_lock);
    ReleaseSRWLockShared(&g_bind_lock);
    {   /* publish the tagged colors for marker_inframe; the float math
         * below is byte-identical to the rgba computation above, so the
         * upload-time comparison is an exact match                       */
        int k;
        for (k = 0; k < nTagged; k++) {
            g_tag_vals[k * 4 + 0] = (float) (((g_tag_argb[k] >> 16) & 255u) / 255.0);
            g_tag_vals[k * 4 + 1] = (float) (((g_tag_argb[k] >> 8)  & 255u) / 255.0);
            g_tag_vals[k * 4 + 2] = (float) (( g_tag_argb[k]        & 255u) / 255.0);
            g_tag_vals[k * 4 + 3] = 1.0f;
        }
        MemoryBarrier();
        InterlockedExchange(&g_tag_count, nTagged);
    }
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
    /* optional: reading the vendor value enables the in-frame sharing
     * with marker_inframe; its absence only degrades that cooperation */
    p_osg_uniform_get = (int (*)(const void *, float *)) (void *)
        GetProcAddress(osg, "?get@Uniform@osg@@QEBA_NAEAVVec4f@2@@Z");
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
        "paint markers red/yellow/green by tracking quality (scan->hub bridge)"
    };
    return &info;
}

/* Cross-module contract (see the in-frame sharing block above and
 * modules\marker_inframe\marker_inframe.c): hands marker_inframe the
 * exact float4 colors this module used THIS frame for in-frame markers
 * (quality color with green byte +1).  Called on the GL thread only. */
EX_EXPORT int ex_mod_inframe_snapshot(const float **vals)
{
    *vals = g_tag_vals;
    return (int) InterlockedCompareExchange(&g_tag_count, 0, 0);
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
    g_share_inframe    = host->cfg_int(host, "share_inframe", 1);
    if (g_green_frames < 1)   g_green_frames = 1;
    if (g_green_spread_um < 1) g_green_spread_um = 1;
    if (g_green_supported < 1) g_green_supported = 1;
    if (g_yellow_frames < 1)  g_yellow_frames = 1;
    if (g_publish_ms < 100)   g_publish_ms = 100;

    /* dark red on purpose: R=128/255=0.50 sits below the 0.55 threshold
     * of the shared vendor-red detector, so a quality-red marker never
     * looks "in frame" to marker_inframe (see contract above)         */
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
