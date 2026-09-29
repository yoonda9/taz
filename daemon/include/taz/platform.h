#ifndef TAZ_PLATFORM_H
#define TAZ_PLATFORM_H

#include <uv.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Install OS-level signal handlers on the given loop so that Ctrl+C /
     * SIGTERM causes uv_run to return.
     * POSIX: registers uv_signal_t watchers for SIGINT and SIGTERM.
     * Win32:  registers a SetConsoleCtrlHandler that wakes the loop. */
    void taz_platform_init_signals(uv_loop_t *loop);

    /* Flush all standard I/O streams before exit.
     * POSIX: fflush(stdout) + fflush(stderr).
     * Win32:  _flushall() covers every open C-runtime stream. */
    void taz_platform_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_PLATFORM_H */
