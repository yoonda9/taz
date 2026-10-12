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

    /* Part of the shutdown walk: uv_unref every handle
     * taz_platform_init_signals opened, so they no longer keep uv_run
     * alive, while leaving them installed - a repeat signal or control
     * event during the drain still reaches the (by then no-op)
     * taz_shutdown_request instead of the OS default action, which would
     * terminate the process mid-walk. Idempotent; loop thread only. */
    void taz_platform_release_signals(void);

    /* Close every handle taz_platform_init_signals opened. Called by
     * taz_shutdown_finish once uv_run has returned, right before its last
     * drain of the loop.
     * POSIX:  uv_signal_stop then uv_close on both signal handles
     *         (uv_is_closing guarded).
     * Win32:  marks the console-ctrl handler's stopping flag, under the
     *         same lock the handler holds around uv_async_send, before
     *         closing the async handle, so a control event on its own
     *         thread never sends to a closing or closed handle.
     * Idempotent; a no-op if taz_platform_init_signals never ran; loop
     * thread only. */
    void taz_platform_close_signals(void);

    /* Flush all standard I/O streams before exit.
     * POSIX: fflush(stdout) + fflush(stderr).
     * Win32:  _flushall() covers every open C-runtime stream. */
    void taz_platform_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_PLATFORM_H */
