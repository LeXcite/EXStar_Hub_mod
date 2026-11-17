/*
 * template.c - minimal starter module for the EXStar Hub mod framework.
 *
 * Copy this folder to `modules\<your_module>\`, keep just the .c file,
 * add the id to build.ps1 (one line), rebuild, drop the produced DLL into
 * the application's `modules\` directory. Toggle it with a section in
 * `modules.ini`:
 *
 *   [<your_module>]
 *   enabled = 1
 *
 * Your module config lives in `modules\<your_module>.ini` and is read
 * through host->cfg_int()/cfg_string() (see include/exstar_sdk.h).
 * Reusable pieces: include/exstar_qrc.h (rewrite compressed QML
 * resources in a mapped DLL) and include/exstar_export.h (detour an
 * export by unique name) - both header-only, see docs/architecture.md.
 */

#include <windows.h>

#include "exstar_sdk.h"

static const ExModuleInfo g_info = {
    EX_ABI_VERSION,
    "template",                 /* must match the DLL file name and
                                   the modules.ini section name */
    "Template Module",
    "0.1.0",
    "skeleton module - logs its lifecycle"
};

static void WINAPI onFrame(void)
{
    /* Put your detour callbacks here. They run on arbitrary application
     * threads: keep them allocation-free, fast and SEH-guarded. */
}

EX_EXPORT const ExModuleInfo *ex_get_info(void)
{
    return &g_info;
}

EX_EXPORT int ex_init(const ExHost *host)
{
    host->log("template: initializing");

    /* Example of using host services:
     *
     *   int    n   = host->cfg_int(host, "answer", 42);
     *   void  *fn  = host->gl_thunk("glDrawArrays");   // waits for GL
     *   if (fn) host->hook(fn, my_detour, (void **)&real_fn);
     */
    (void) onFrame;

    /* Return non-zero to decline starting; the host logs it and the
     * application continues completely unaffected. */
    return 0;
}

EX_EXPORT void ex_shutdown(void)
{
    /* Nothing to tear down safely: the host is only unloaded when the
     * process is terminating. Keep it trivial. */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h;
    (void)reason;
    (void)r;
    return TRUE;                         /* never fail */
}
