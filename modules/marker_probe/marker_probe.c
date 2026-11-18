/*
 * marker_probe.c - DIAGNOSTIC probe for the future marker_quality module.
 * Development tool: not embedded into the manager, not released.
 * NOTE: it detours the same anchors as marker_quality (color slot,
 * setMarkerPoints) - never leave it in the modules dir next to an enabled
 * marker_quality: whichever loads first wins and the other one degrades.
 *
 * Goals (live answers for EXStar Hub 2.x (file stamp 3.1.49), no RVAs anywhere):
 *   1. Frame pipeline: does HandlePrjFile::putFrameDataIntoFile receive
 *      frameData.curMarker / rt / frameCount; are marker records 0xB0 with
 *      int32 id at +0x50 and float3 local position at +0; does
 *      world = R*local + t produce consistent finite positions.
 *   2. Qt QVariantMap: key names + meta-type ids (incl. the vendor
 *      shared-pointer wrapper type), discovered live.
 *   3. Render side: setMarkerPoints chain node+0x150 -> mix, mix+0x138 ->
 *      geometry, geometry+0x220 -> key vector, geometry+0x248 -> state ->
 *      +8 flags; keys must look like marker ids.
 *   4. Calibration data: per-id quality statistics (frames, RMS spread of
 *      the world position, per-observation deviation) dumped periodically
 *      so red/yellow/green thresholds can be chosen from real scans.
 *   5. Topology: pid/exe where each hook fires.
 *
 * Qt access is fully name-based: QMap<QString,QVariant> is a red-black tree
 * with the layout that is stable across Qt5; the walk compares keys through
 * the exported QString::size()/utf16() and reads types through
 * QVariant::userType()/constData()/toInt(). No vendor function is called
 * except the two detoured exports via their trampolines. All foreign reads
 * are SEH-guarded.
 *
 * The quality math is ported from a companion reference MarkerQuality.h and
 * MarkerCoverage.h with the thresholds kept as in the original.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "exstar_sdk.h"

#define MOD_ID      "marker_probe"
#define MOD_VER     "0.3"

/* vendor exports, resolved by name only -------------------------------- */
#define EXP_PUT_FRAME \
    "?putFrameDataIntoFile@HandlePrjFile@@QEAAXAEBV?$QMap@VQString@@VQVariant@@@@@Z"
#define EXP_SET_MARKERS \
    "?setMarkerPoints@ModelNode@ExtDigit@RenderKit@Sn3D@@QEAAIUSn3DMarkerParam@234@@Z"

/* frame ABI candidate (verified layout for 2.0.0-45) -------------------- */
#define REC_SIZE    0xB0
#define REC_ID_OFF  0x50

#define MAX_IDS             4096
#define MAX_FRAME_ENTRIES   256

static const ExHost *g_host;

/* ------------------------------------------------------------------ */
/* Quality model (ported from the companion reference)                  */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t frames;
    double   center[3];
    double   sq_err;
} Quality;

typedef struct {
    Quality  pos;
    uint64_t dir_frames;
    double   dir[8][3];
    uint32_t support[8];
    unsigned directions;
    unsigned octants;
    double   max_angle;
    unsigned complete;
    /* probe extras for threshold calibration */
    double   dev_sum;      /* sum of |obs - previous mean| (mm)           */
    uint64_t dev_n;
    double   dev_max;
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
    c->dir_frames++;
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
    if (c->pos.frames >= 20 && quality_spread(&c->pos) <= 0.10 &&
        coverage_supported(c) >= 4)
        c->complete = 1;
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static SRWLOCK       g_lock = SRWLOCK_INIT;
static Coverage      g_cov[MAX_IDS];
/* parse outcome counters (reference order + extras) */
enum { ST_INVALID, ST_NO_FRAME, ST_NO_MARKER, ST_NO_POSE,
       ST_BAD_SPAN, ST_NO_IDS, ST_ACCEPTED, ST_COUNT };
static volatile LONG g_stats[ST_COUNT];
static volatile LONG g_frames_total;
static void         *g_project;       /* HandlePrjFile owner: reset edge   */
static ULONGLONG     g_last_report;
static int           g_first_frame = 1;
static int           g_frame_dumped;
static int           g_report_ms = 5000;
static int           g_probe_on = 1;
static int           g_ptr_type = -1;   /* live-discovered wrapper type    */

/* render side */
static volatile LONG g_smp_calls;      /* setMarkerPoints invocations      */
static volatile LONG g_smp_parsed;     /* params layout validated          */
static volatile LONG g_chain_null;     /* calls with node+0x150 == 0       */
static volatile LONG g_smp_last_count; /* key count of the last log line   */
static int           g_chain_ok;       /* node chain resolved at least once*/
static int           g_hook_put;       /* putFrameDataIntoFile detoured    */
static int           g_hook_smp;       /* setMarkerPoints detoured         */

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
static const void     *g_map_null;    /* &QMapDataBase::shared_null        */

static void (*tramp_put_frame)(void *, const void *);
static unsigned (*tramp_set_markers)(void *, void *);

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
/* Qt5 QMap<QString,QVariant> access (layout stable across Qt5)        */
/* ------------------------------------------------------------------ */
/* Layout proven OFFLINE against our Qt5Core.dll 5.15.10 (GCC/MinGW MS-ABI
 * build) by building a real QMap<QString,QVariant> through exported Qt
 * functions and dumping it:
 *   *map = d
 *   d+0x00 u32 ref, d+0x04 u32 alloc
 *   d+0x08 sentinel ("end"); root node's parent points back here
 *   d+0x10 = root pointer            (rebalance@QMapDataBase compares [this+0x10])
 *   d+0x20 = leftmost node ("begin")
 *   node: parent@0 (POINTER-TAGGED, bit0 color -> & ~3), left@8, right@16,
 *         QString key@24 (8B d-ptr),
 *         QVariant value@32 = { union data@0, int type@+8, bool is_shared@+12 }
 *         (QVariant::userType()/constData() take &value, i.e. node+32.) */
#define D_SENTINEL 8
#define D_ROOT     0x10
#define N_PARENT   0
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
    /* reject non-ASCII garbage */
    for (i = 0; i < n; i++)
        if ((unsigned char) out[i] < 0x20 || (unsigned char) out[i] > 0x7E)
            return -1;
    return n;
}

typedef struct {
    const char *want;      /* NULL => dump mode                            */
    const void *found;     /* QVariant* inside the node                    */
    int         active;
    int         seen;      /* nodes visited (dump mode)                    */
} MapCtx;

static void map_node(const void *node, MapCtx *ctx)
{
    char name[64];

    if (node_key(node, name, (int) sizeof name) < 0)
        return;
    if (ctx->want) {
        if (strcmp(name, ctx->want) == 0)
            ctx->found = (const char *) node + N_VAL;
    } else {
        ctx->seen++;
        g_host->log("probe: key \"%s\" type=%d", name,
                    variant_type((const char *) node + N_VAL));
    }
}

static void map_walk(const void *node, const void *header, int depth,
                     MapCtx *ctx)
{
    const void *l = NULL, *r = NULL;

    if (!node || node == header || depth > 48 || !ctx->active)
        return;
    if (ctx->want && ctx->found)
        return;
    __try {
        l = *(const void * volatile *) ((const char *) node + N_LEFT);
        r = *(const void * volatile *) ((const char *) node + N_RIGHT);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx->active = 0;
        return;
    }
    map_walk(l, header, depth + 1, ctx);
    if (ctx->active && (!ctx->want || !ctx->found)) {
        map_node(node, ctx);
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
    rt = (const void *) ((uintptr_t) rt & ~(uintptr_t) 3);   /* untag just in case */
    if (!rt)
        return 0;
    *root = rt;
    *header = (const char *) d + D_SENTINEL;   /* end sentinel, embedded */
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

static void map_dump(const void *map, const char *tag)
{
    const void *root, *header;
    MapCtx ctx;

    g_host->log("probe: --- %s ---", tag);
    if (!map_root(map, &root, &header)) {
        g_host->log("probe: %s empty", tag);
        return;
    }
    memset(&ctx, 0, sizeof ctx);
    ctx.want = NULL;
    ctx.active = 1;
    map_walk(root, header, 0, &ctx);
    g_host->log("probe: %s nodes=%d", tag, ctx.seen);
}

static int map_helpers_ready(void)
{
    return p_userType && p_constData && p_strSize && p_strUtf16;
}

/* ------------------------------------------------------------------ */
/* putFrameDataIntoFile: observe the frame                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t id;
    size_t  n;
    double  mean[3];
} FrameEntry;

static void report(void);

static void observe(void *owner, const void *params)
{
    const void *frameVar, *frame, *markerVar, *poseVar, *countVar;
    const void *markerObj, *poseObj;
    uintptr_t begin = 0, end = 0;
    size_t count, i, used = 0;
    double pose[12];
    unsigned char intOk = 0;
    int frameNo = -1, good;
    FrameEntry entries[MAX_FRAME_ENTRIES];

    if (owner != g_project) {
        g_project = owner;
        memset(g_cov, 0, sizeof g_cov);
        g_first_frame = 1;
        g_host->log("probe: new project owner=%p, quality reset", owner);
    }
    InterlockedIncrement(&g_frames_total);

    if (!map_helpers_ready()) {
        InterlockedIncrement(&g_stats[ST_INVALID]);
        return;
    }
    if (g_first_frame) {
        g_first_frame = 0;
        g_host->log("probe: first frame here (pid=%lu), dumping top-level map",
                    (unsigned long) GetCurrentProcessId());
        map_dump(params, "top-level map");
    }
    frameVar = map_find(params, "frameData");
    if (!frameVar) {
        InterlockedIncrement(&g_stats[ST_NO_FRAME]);
        return;
    }
    if (variant_type(frameVar) != 8) {      /* 8 = QVariantMap */
        g_host->log("probe: frameData found but type=%d (pid=%lu)",
                    variant_type(frameVar), (unsigned long) GetCurrentProcessId());
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

    if (!g_frame_dumped) {
        g_frame_dumped = 1;
        map_dump(frame, "frameData map");
        g_ptr_type = variant_type(markerVar);
        g_host->log("probe: curMarker type=%d rt type=%d frameCount type=%d",
                    g_ptr_type, variant_type(poseVar), variant_type(countVar));
    }

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
    poseObj   = variant_shared_object(poseVar);
    if (!markerObj || !poseObj) {
        InterlockedIncrement(&g_stats[ST_NO_MARKER]);
        return;
    }

    good = 1;
    __try {
        begin = *(const uintptr_t volatile *) ((const char *) markerObj + 0);
        end   = *(const uintptr_t volatile *) ((const char *) markerObj + 8);
        memcpy(pose, poseObj, sizeof pose);
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
        float local[3] = { 0, 0, 0 };
        int32_t id = -1;
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

    if (g_first_frame == 0 && g_last_report == 0) {
        g_host->log("probe: first frame parsed: records=%zu ids=%zu "
                    "frameCount=%d pose=[%.3f %.3f %.3f %.3f %.3f %.3f "
                    "%.3f %.3f %.3f %.3f %.3f %.3f]",
                    count, used, frameNo,
                    pose[0], pose[1], pose[2], pose[3], pose[4], pose[5],
                    pose[6], pose[7], pose[8], pose[9], pose[10], pose[11]);
    }

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < used; i++) {
        Coverage *c = &g_cov[entries[i].id];
        double m[3];
        m[0] = entries[i].mean[0] / (double) entries[i].n;
        m[1] = entries[i].mean[1] / (double) entries[i].n;
        m[2] = entries[i].mean[2] / (double) entries[i].n;
        coverage_add(c, m, pose + 9);
    }
    ReleaseSRWLockExclusive(&g_lock);

    report();
}

static void report(void)
{
    ULONGLONG now = GetTickCount64();
    int id, active = 0, id_min = MAX_IDS, id_max = -1;

    if (g_last_report && now - g_last_report < (ULONGLONG) g_report_ms)
        return;
    g_last_report = now;
    g_host->log("probe: === pid=%lu frames=%ld stats=[invalid=%ld noFrame=%ld "
                "noMarker=%ld noPose=%ld badSpan=%ld noIds=%ld ok=%ld] "
                "smpCalls=%ld ===",
                (unsigned long) GetCurrentProcessId(),
                (long) g_frames_total,
                (long) g_stats[ST_INVALID], (long) g_stats[ST_NO_FRAME],
                (long) g_stats[ST_NO_MARKER], (long) g_stats[ST_NO_POSE],
                (long) g_stats[ST_BAD_SPAN], (long) g_stats[ST_NO_IDS],
                (long) g_stats[ST_ACCEPTED], (long) g_smp_calls);
    AcquireSRWLockShared(&g_lock);
    for (id = 0; id < MAX_IDS; id++) {
        const Coverage *c = &g_cov[id];
        if (!c->pos.frames)
            continue;
        active++;
        if (id < id_min) id_min = id;
        if (id > id_max) id_max = id;
        g_host->log("probe: id=%d frames=%llu spread=%.4f devAvg=%.4f "
                    "devMax=%.4f dirs=%u supported=%u maxAng=%.1f oct=%X "
                    "complete=%u center=(%.3f,%.3f,%.3f)",
                    id,
                    (unsigned long long) c->pos.frames,
                    quality_spread(&c->pos),
                    c->dev_n ? c->dev_sum / (double) c->dev_n : 0.0,
                    c->dev_max,
                    c->directions, coverage_supported(c),
                    c->max_angle, c->octants, c->complete,
                    c->pos.center[0], c->pos.center[1], c->pos.center[2]);
    }
    ReleaseSRWLockShared(&g_lock);
    g_host->log("probe: === %d marker(s), id range %d..%d ===",
                active, id_min, id_max);
}

static void hook_put_frame(void *owner, const void *params)
{
    static LONG first;
    if (InterlockedExchange(&first, 1) == 0)
        g_host->log("probe: putFrameDataIntoFile first hit, pid=%lu",
                    (unsigned long) GetCurrentProcessId());
    if (g_probe_on) {
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
/* setMarkerPoints detour (params + chain validation only, no writes)  */
/*                                                                     */
/* The export takes Sn3DMarkerParam by value; in the Microsoft x64 ABI */
/* the register slot then carries the address of the byval copy, so    */
/* `params` here is readable exactly like the companion's production hook */
/* on the same export. Layout proven by him on the same program:       */
/*   +0x60 positions obj (vector begin @ +0x88, end @ +0x98, stride12) */
/*   +0x78 keys obj      (vector begin @ +0x88, end @ +0x98, u64 ids)  */
/*   +0x80 flags obj     (vector begin @ +0x88, end @ +0x98, i8 vis:   */
/*                        <0 hidden, 0 normal color, >0 alternate)     */
/* ------------------------------------------------------------------ */
static void inspect_render(void *node, const void *params)
{
    static ULONGLONG logged;
    static const char *last_why = "";
    const char *why = "ok";
    ULONGLONG now = GetTickCount64();
    uintptr_t posObj = 0, keysObj = 0, flagsObj = 0;
    uintptr_t kRange[2] = { 0, 0 }, pRange[2] = { 0, 0 }, fRange[2] = { 0, 0 };
    uint64_t ks[4] = { 0, 0, 0, 0 };
    int vis[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
    size_t count = 0, pcount = 0, sample;
    LONG prev_count;

    if (!params) {
        why = "params null";
    } else if (!safe_read(&posObj,   (const char *) params + 0x60, 8) ||
               !safe_read(&keysObj,  (const char *) params + 0x78, 8) ||
               !safe_read(&flagsObj, (const char *) params + 0x80, 8)) {
        why = "params fault";
    } else if (!keysObj ||
               !safe_read(kRange, (const void *) (keysObj + 0x88), 16) ||
               kRange[1] < kRange[0] || (kRange[1] - kRange[0]) % 8) {
        why = "keys bad";
    } else {
        count = (size_t) ((kRange[1] - kRange[0]) / 8);
        if (!count || count > MAX_IDS) {
            why = "count odd";
        } else {
            sample = count < 4 ? count : 4;
            safe_read(ks, (const void *) kRange[0], sample * 8);
            if (posObj &&
                safe_read(pRange, (const void *) (posObj + 0x88), 16) &&
                pRange[1] >= pRange[0] && (pRange[1] - pRange[0]) % 12 == 0)
                pcount = (size_t) ((pRange[1] - pRange[0]) / 12);
            if (flagsObj &&
                safe_read(fRange, (const void *) (flagsObj + 0x88), 16) &&
                fRange[1] >= fRange[0] &&
                (size_t) (fRange[1] - fRange[0]) == count) {
                unsigned char vb[4] = { 0, 0, 0, 0 };
                safe_read(vb, (const void *) fRange[0], sample);
                vis[0] = (int) (signed char) vb[0];
                vis[1] = (int) (signed char) vb[1];
                vis[2] = (int) (signed char) vb[2];
                vis[3] = (int) (signed char) vb[3];
            }
        }
    }

    if (strcmp(why, "ok") == 0) {
        InterlockedIncrement(&g_smp_parsed);
        prev_count = InterlockedExchange(&g_smp_last_count, (LONG) count);
        (void) prev_count;
        if (now - logged >= 5000) {
            logged = now;
            g_host->log("probe: smp ok: node=%p keys=%p count=%zu pos=%zu "
                        "ids=[%llu,%llu,%llu,%llu] vis=[%d,%d,%d,%d]",
                        node, (void *) kRange[0], count, pcount,
                        (unsigned long long) ks[0],
                        (unsigned long long) ks[1],
                        (unsigned long long) ks[2],
                        (unsigned long long) ks[3],
                        vis[0], vis[1], vis[2], vis[3]);
        }
    } else if (why != last_why && now - logged >= 2000) {
        last_why = why;
        logged = now;
        g_host->log("probe: smp params: %s (node=%p params=%p)",
                    why, node, params);
    }

    /* node render chain: retries every call until the geometry exists
     * (the first call happens before mix/geometry are created) */
    if (!g_chain_ok && node) {
        uintptr_t mix = 0, geom = 0, keys = 0, state = 0, flags = 0;
        int ok = safe_read(&mix, (const char *) node + 0x150, 8);
        if (ok && mix) {
            ok = safe_read(&geom, (const void *) (mix + 0x138), 8) && geom;
            if (ok) {
                safe_read(&keys,  (const void *) (geom + 0x220), 8);
                safe_read(&state, (const void *) (geom + 0x248), 8);
            }
            if (state)
                safe_read(&flags, (const void *) (state + 8), 8);
            if (ok) {
                uintptr_t kR[2] = { 0, 0 }, fR[2] = { 0, 0 };
                g_chain_ok = 1;
                if (keys)
                    safe_read(kR, (const void *) (keys + 0x88), 16);
                if (flags)
                    safe_read(fR, (const void *) (flags + 0x88), 16);
                g_host->log("probe: chain OK node=%p mix=%p geom=%p "
                            "keys=%p state=%p flags=%p keyCount=%zu visBytes=%zu "
                            "(after %ld null calls)",
                            node, (void *) mix, (void *) geom, (void *) keys,
                            (void *) state, (void *) flags,
                            (size_t) (kR[1] > kR[0] ? (kR[1] - kR[0]) / 8 : 0),
                            (size_t) (fR[1] > fR[0] ? fR[1] - fR[0] : 0),
                            (long) g_chain_null);
            }
        } else {
            LONG n = InterlockedIncrement(&g_chain_null);
            if (n == 1 || n == 60)
                g_host->log("probe: chain node+0x150 null (n=%ld, still "
                            "retrying)", (long) n);
        }
    }
}

static void hook_set_markers(void *node, void *params)
{
    static LONG first;
    InterlockedIncrement(&g_smp_calls);
    if (InterlockedExchange(&first, 1) == 0)
        g_host->log("probe: setMarkerPoints first hit, pid=%lu node=%p",
                    (unsigned long) GetCurrentProcessId(), node);
    if (g_probe_on) {
        __try {
            inspect_render(node, params);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* inspection must never break the vendor call */
        }
    }
    if (tramp_set_markers)
        tramp_set_markers(node, params);
}

/* ------------------------------------------------------------------ */
/* Installer thread: wait for modules, resolve by name, hook each      */
/* hook independently (sibling processes may only carry one side)      */
/* ------------------------------------------------------------------ */
static int resolve_qt(void)
{
    HMODULE qt = GetModuleHandleW(L"Qt5Core.dll");
    FARPROC fp;

    if (!qt)
        return 0;
    p_userType  = (f_variant_type) GetProcAddress(qt, "?userType@QVariant@@QEBAHXZ");
    p_constData = (f_variant_data) GetProcAddress(qt, "?constData@QVariant@@QEBAPEBXXZ");
    p_toInt     = (f_variant_int)  GetProcAddress(qt, "?toInt@QVariant@@QEBAHPEA_N@Z");
    p_strSize   = (f_str_size)     GetProcAddress(qt, "?size@QString@@QEBAHXZ");
    p_strUtf16  = (f_str_utf16)    GetProcAddress(qt, "?utf16@QString@@QEBAPEBGXZ");
    fp = GetProcAddress(qt, "?shared_null@QMapDataBase@@2U1@B");
    g_map_null = (const void *) fp;
    if (!p_userType || !p_constData || !p_toInt || !p_strSize || !p_strUtf16)
        return 0;
    g_host->log("probe: Qt5Core helpers resolved (shared_null=%p)",
                g_map_null);
    return 1;
}

static void check_osg_exports(HMODULE osg)
{
    static const char *names[] = {
        "?getChild@Group@osg@@QEAAPEAVNode@2@I@Z",
        "?getNumChildren@Group@osg@@UEBAIXZ",
        "?getStateSet@Node@osg@@QEAAPEAVStateSet@2@XZ",
        "?getUniform@StateSet@osg@@QEAAPEAVUniform@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
        "?set@Uniform@osg@@QEAA_NAEBVVec4f@2@@Z"
    };
    size_t k;

    for (k = 0; k < sizeof names / sizeof names[0]; k++)
        g_host->log("probe: osg export -> %s : %s", names[k],
                    GetProcAddress(osg, names[k]) ? "OK" : "MISSING");
}

/* ------------------------------------------------------------------ */
/* Vendor color-update slot (hub side).                                */
/*                                                                     */
/* the companion study found in the renderer module a read-only slot (in .rdata, proven
 * statically: renderer+0x28CAA8 -> stub at renderer+0x130320, single
 * signature occurrence file-wide) holding one function pointer to the stub
 * "add rcx,8; jmp <colorUpdate>" that the vendor calls every frame with
 * owner = { ?, group, flags } to paint marker discs.  Swapping that slot
 * (VirtualProtect + write) is THE way to recolor markers without touching
 * GL globally.                                                        */
/*                                                                     */
/* We never hardcode an address: scan the renderer's executable        */
/* sections for the stub signature (fixed build of the same program    */
/* version), then scan the writable image sections for the pointer     */
/* referencing it. On an unambiguous hit we install a counting hook    */
/* that only logs owner/group/flags/childCount - painting comes with   */
/* the marker_quality module.                                          */
/* ------------------------------------------------------------------ */
static void (*g_orig_color_update)(void *);
static volatile LONG g_color_cb_calls;
static unsigned (*g_osg_num_children)(void *);

static void hook_color_update(void *owner)
{
    LONG n;

    if (g_orig_color_update)
        g_orig_color_update(owner);
    if (!g_probe_on)
        return;
    n = InterlockedIncrement(&g_color_cb_calls);
    if (n <= 3 && owner) {
        uintptr_t group = 0, flags = 0, fRange[2] = { 0, 0 };
        unsigned kids = 0;
        if (safe_read(&group, (const char *) owner + 8, 8) &&
            safe_read(&flags, (const char *) owner + 16, 8)) {
            if (flags)
                safe_read(fRange, (const void *) (flags + 0x88), 16);
            if (group && g_osg_num_children) {
                __try {
                    kids = g_osg_num_children((void *) group);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    kids = 0xFFFFFFFFu;
                }
            }
            g_host->log("probe: colorCb #%ld owner=%p group=%p flags=%p "
                        "visBytes=%zu children=%u",
                        (long) n, owner, (void *) group, (void *) flags,
                        (size_t) (fRange[1] > fRange[0]
                                  ? fRange[1] - fRange[0] : 0), kids);
        } else {
            g_host->log("probe: colorCb #%ld owner=%p unreadable", (long) n,
                        owner);
        }
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
                if (nTargets < 8) {
                    targets[nTargets] = (uintptr_t) p;
                    g_host->log("probe: color stub candidate at renderer+0x%llX",
                                (unsigned long long) ((uintptr_t) p - (uintptr_t) base));
                }
                nTargets++;
            }
        }
    }
    if (!nTargets) {
        g_host->log("probe: color stub signature not found in renderer");
        return 0;
    }
    if (nTargets > 8) {
        g_host->log("probe: %d stub candidates, too many, skipping", nTargets);
        return 0;
    }

    /* 2. find the single qword (any image section - the vendor slot lives
        * in .rdata) pointing at a candidate */
    for (i = 0; i < nsec; i++) {
        unsigned char *p, *e;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;                       /* stubs live in code */
        p = base + sec[i].VirtualAddress;
        e = p + sec[i].Misc.VirtualSize;
        for (; p + 8 <= e; p += 8) {
            uintptr_t v = 0;
            int t;
            if (!safe_read(&v, p, 8))
                continue;
            for (t = 0; t < nTargets && t < 8; t++) {
                if (v != targets[t])
                    continue;
                g_host->log("probe: color slot candidate renderer+0x%llX "
                            "-> stub renderer+0x%llX",
                            (unsigned long long) ((uintptr_t) p - (uintptr_t) base),
                            (unsigned long long) (v - (uintptr_t) base));
                found = (void *volatile *) p;
                nSlots++;
            }
        }
    }
    if (nSlots != 1) {
        g_host->log("probe: color slot candidates=%d (need exactly 1), not hooked",
                    nSlots);
        return 0;
    }

    /* 3. swap it (in memory only, original kept for the trampoline call) */
    {
        HMODULE osg = GetModuleHandleW(L"osg158-osg.dll");
        DWORD protect, unused;
        if (!osg)
            return 0;
        g_osg_num_children = (unsigned (*)(void *)) (void *)
            GetProcAddress(osg, "?getNumChildren@Group@osg@@UEBAIXZ");
        if (!g_osg_num_children)
            return 0;
        if (!VirtualProtect((void *) found, 8, PAGE_READWRITE, &protect))
            return 0;
        g_orig_color_update = (void (*)(void *)) *found;
        *found = (void *) hook_color_update;
        VirtualProtect((void *) found, 8, protect, &unused);
        g_host->log("probe: color slot hooked");
        return 1;
    }
}

static DWORD WINAPI installer(LPVOID unused)
{
    HMODULE file = NULL, render = NULL, osg = NULL;
    void *fp_put = NULL, *fp_smp = NULL;
    int i, qt_ok = 0, put_done = 0, smp_done = 0, osg_checked = 0, slot_done = 0;

    (void) unused;
    for (i = 0; i < 2400 && !(put_done && smp_done); i++) {
        if (!file)
            file = GetModuleHandleW(L"Sn3DFile.dll");
        if (!render)
            render = GetModuleHandleW(L"librenderkit.extensions.digit.dll");
        if (!osg && (file || render))
            osg = GetModuleHandleW(L"osg158-osg.dll");
        if (osg && !osg_checked) {
            osg_checked = 1;
            check_osg_exports(osg);
        }
        if (osg && render && !slot_done) {
            slot_done = 1;
            scan_color_slot(render);
        }

        if (!put_done && file && (qt_ok || (qt_ok = resolve_qt()))) {
            fp_put = (void *) GetProcAddress(file, EXP_PUT_FRAME);
            g_hook_put = fp_put
                && g_host->hook(fp_put, hook_put_frame,
                                (void **) &tramp_put_frame);
            g_host->log(g_hook_put
                        ? "probe: detoured putFrameDataIntoFile"
                        : "probe: putFrameDataIntoFile NOT hooked");
            put_done = 1;           /* export lookup is deterministic */
        }
        if (!smp_done && render) {
            fp_smp = (void *) GetProcAddress(render, EXP_SET_MARKERS);
            g_hook_smp = fp_smp
                && g_host->hook(fp_smp, hook_set_markers,
                                (void **) &tramp_set_markers);
            g_host->log(g_hook_smp
                        ? "probe: detoured setMarkerPoints"
                        : "probe: setMarkerPoints NOT hooked");
            smp_done = 1;
        }
        if (!(put_done && smp_done))
            Sleep(250);
    }
    g_host->log("probe: install done: Sn3DFile=%p renderkit=%p osg=%p "
                "putFrame=%p hooked=%d setMarkerPoints=%p hooked=%d",
                (void *) file, (void *) render, (void *) osg,
                fp_put, g_hook_put, fp_smp, g_hook_smp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Heartbeat: proves which pipeline halves exist in THIS process even  */
/* when no frames flow (the writer may live in a sibling exe)          */
/* ------------------------------------------------------------------ */
static DWORD WINAPI heartbeat(LPVOID unused)
{
    (void) unused;
    for (;;) {
        Sleep((DWORD) (g_report_ms < 2000 ? 2000 : g_report_ms));
        if (!g_probe_on)
            break;
        g_host->log("probe: hb pid=%lu putFrame=%ld ok=%ld smp=%ld parsed=%ld "
                    "chainNull=%ld hooks=%d%d | Sn3DFile=%p multiPrj=%p "
                    "renderkit=%p osg=%p Qt5Core=%p",
                    (unsigned long) GetCurrentProcessId(),
                    (long) g_frames_total, (long) g_stats[ST_ACCEPTED],
                    (long) g_smp_calls, (long) g_smp_parsed,
                    (long) g_chain_null, g_hook_put, g_hook_smp,
                    (void *) GetModuleHandleW(L"Sn3DFile.dll"),
                    (void *) GetModuleHandleW(L"Sn3DMultiProjects.dll"),
                    (void *) GetModuleHandleW(L"librenderkit.extensions.digit.dll"),
                    (void *) GetModuleHandleW(L"osg158-osg.dll"),
                    (void *) GetModuleHandleW(L"Qt5Core.dll"));
        report();                   /* per-id calibration lines */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module ABI                                                          */
/* ------------------------------------------------------------------ */
static const ExModuleInfo g_info = {
    EX_ABI_VERSION, MOD_ID, "Marker Probe", MOD_VER,
    "diagnostic: marker frame/render pipeline (dev only)"
};

EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    char exe[MAX_PATH] = "?";
    char *base;

    g_host = host;
    g_report_ms = host->cfg_int(host, "report_ms", 5000);
    g_probe_on  = host->cfg_int(host, "enabled_probe", 1);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    base = strrchr(exe, '\\');
    g_host->log("probe: module start, exe=%s pid=%lu",
                base ? base + 1 : exe, (unsigned long) GetCurrentProcessId());
    if (!CreateThread(NULL, 0, installer, NULL, 0, NULL))
        return 1;
    if (!CreateThread(NULL, 0, heartbeat, NULL, 0, NULL))
        g_host->log("probe: heartbeat thread not started");
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    g_probe_on = 0;
}
