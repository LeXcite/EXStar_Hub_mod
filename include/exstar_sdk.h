/*
 * exstar_sdk.h - module ABI for the EXStar Hub mod framework.
 *
 * A module is a plain C DLL placed into the `modules\` folder next to the
 * host (ExStarRec.dll). The host loads every module DLL found there,
 * validates ExModuleInfo::abi, consults `modules.ini` for the per-module
 * `enabled` flag and, when enabled, calls ex_init() on a worker thread
 * that owns a private OpenGL context.
 *
 * Rules for module authors:
 *   - DllMain of a module must do nothing (all work in ex_init);
 *   - do not link opengl32.lib: obtain function addresses via
 *     host->gl_thunk() (works for both 1.x exports and modern thunks);
 *   - do not call MH_* directly: install detours via host->hook();
 *   - ex_init runs on the host worker thread, detours run on arbitrary
 *     application threads: module state must be thread-safe or
 *     GL-thread-only (documented per case);
 *   - never crash the host: guard risky paths with SEH;
 *   - return non-zero from ex_init to disable the module gracefully.
 *
 * ABI versioning: the host refuses modules whose ExModuleInfo::abi
 * differs from EX_ABI_VERSION. Additive changes to ExHost bump the
 * version; `ExHost::size` additionally allows forward-compatible growth.
 */

#ifndef EXSTAR_SDK_H
#define EXSTAR_SDK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EX_ABI_VERSION 1u

#define EX_EXPORT __declspec(dllexport)

typedef struct ExModuleInfo {
    uint32_t    abi;         /* must be EX_ABI_VERSION at compile time */
    const char *id;          /* stable id, matches modules.ini section */
    const char *name;        /* human readable name                    */
    const char *version;     /* module's own version                   */
    const char *description; /* one-line purpose, shown in logs/UIs    */
} ExModuleInfo;

typedef struct ExHost ExHost;

/*
 * Services provided by the host. All pointers remain valid for the
 * lifetime of the process. Function pointers must not be called from
 * module DllMain.
 */
struct ExHost {
    uint32_t  abi;        /* EX_ABI_VERSION the host was built with   */
    uint32_t  size;       /* sizeof(ExHost) on the host side          */
    void     *internal;   /* host-private, do not touch               */

    /* Diagnostics. No-op when host logging is disabled. Thread-safe. */
    void (*log)(const char *fmt, ...);

    /* Configuration of THIS module, read from `modules\<id>.ini`.
     * Lines have the form `key = value`; unknown keys are ignored.
     * Thread-safe (file is re-read on demand; call during ex_init). */
    int (*cfg_int)(const ExHost *self, const char *key, int dflt);
    int (*cfg_string)(const ExHost *self, const char *key,
                      char *buf, unsigned cap);   /* 1 = found        */

    /* Resolve an OpenGL entry point. Prefers wglGetProcAddress (modern
     * driver thunks, shared process-wide), falls back to opengl32.dll
     * exports (GL 1.x). Waits up to ~5 minutes for the application to
     * initialize OpenGL. Returns NULL on timeout/error. Blocking.      */
    void *(*gl_thunk)(const char *name);

    /* Install an inline detour (MinHook wrapper): target -> detour,
     * original callable via *tramp. Returns 1 on success.              */
    int (*hook)(void *target, void *detour, void **tramp);
};

/* ---- exports every module must provide ---------------------------- */

/* Module metadata; pointer must stay valid (static storage).          */
EX_EXPORT const ExModuleInfo *ex_get_info(void);

/* Called once by the host. 0 = OK; non-zero = disable this module.    */
EX_EXPORT int ex_init(const ExHost *host);

/* Optional teardown at host unload; may be NULL. Process is usually   */
/* just terminating at this point - keep it trivial.                   */
EX_EXPORT void ex_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* EXSTAR_SDK_H */
