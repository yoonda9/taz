#ifndef TAZ_SHUTDOWN_H
#define TAZ_SHUTDOWN_H

#include "taz/server.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* How the daemon is leaving the loop. TAZ_SHUTDOWN_NONE until the first
     * taz_shutdown_request call. */
    typedef enum
    {
        TAZ_SHUTDOWN_NONE = 0,
        TAZ_SHUTDOWN_STOP = 1,
        TAZ_SHUTDOWN_RESTART = 2
    } taz_shutdown_mode_t;

    /* Force-close any connection whose graceful uv_shutdown has not
     * completed by this many milliseconds after the walk starts (a peer
     * that never drains its read side). */
#define TAZ_SHUTDOWN_DRAIN_MS 1000U

    /* If the loop is still alive this long after the walk starts, log every
     * open handle's type once at WARN - a diagnosable stall, never a
     * reintroduced uv_stop. */
#define TAZ_SHUTDOWN_STALL_LOG_MS 5000U

    /* Bind the server taz_shutdown_request's walk runs against. main calls
     * this once, before uv_run. Tests that call taz_server_shutdown
     * directly on their own server never need this - it only matters to
     * taz_shutdown_request. */
    void taz_shutdown_bind(taz_server_t *server);

    /* Request shutdown in the given mode (TAZ_SHUTDOWN_STOP or
     * TAZ_SHUTDOWN_RESTART, never TAZ_SHUTDOWN_NONE). The first call runs
     * taz_server_shutdown on the bound server (if any), exactly once. A
     * later call never starts another walk; it only changes the mode when
     * it is a STOP arriving during a RESTART (a stop signal while a
     * restart drains: the daemon then exits instead of re-executing).
     * Safe to call from a signal/console-ctrl handler running on the loop
     * thread. */
    void taz_shutdown_request(taz_shutdown_mode_t mode);

    /* The mode shutdown was requested in (STOP once any STOP arrived,
     * otherwise RESTART), or TAZ_SHUTDOWN_NONE before any request. */
    taz_shutdown_mode_t taz_shutdown_mode(void);

    /* 1 once taz_shutdown_request has been called at least once; 0 before
     * that. Equivalent to taz_shutdown_mode() != TAZ_SHUTDOWN_NONE. */
    int taz_shutdown_in_progress(void);

    /* The shutdown walk: taz_work_begin_shutdown (cancels every outstanding
     * pool-work request daemon-wide), close the listener, gracefully
     * close every connection on server->loop (taz_conn_shutdown_all),
     * uv_unref the platform signal/console-ctrl handles (they stay
     * installed, so a repeat signal during the drain is still a no-op
     * rather than the OS default action), then arm a uv_unref'd deadline
     * timer that force-closes any connection still mid-close at
     * TAZ_SHUTDOWN_DRAIN_MS and logs every open handle type once at
     * TAZ_SHUTDOWN_STALL_LOG_MS. Callable directly by tests against their
     * own server, with or without taz_shutdown_bind/taz_shutdown_request.
     * Idempotent: a second call (direct, or via a second
     * taz_shutdown_request) is a no-op. */
    void taz_server_shutdown(taz_server_t *server);

    /* Call once uv_run(&server->loop, ...) has returned. Closes the
     * deadline timer (if still open) and the platform signal/console-ctrl
     * handles, drains the loop once more, then uv_loop_close. Returns 0 on a
     * clean close. If uv_loop_close reports a stray handle/request (UV_EBUSY),
     * logs each open handle's type at ERROR and returns that nonzero code
     * instead - a visible failure, the caller's cue to exit non-zero rather
     * than hide a leak. */
    int taz_shutdown_finish(taz_server_t *server);

    /* Unit tests only: forget every static the module above keeps
     * (mode/in_progress, the bound server, the walk's own idempotence
     * guard) so the next test starts from TAZ_SHUTDOWN_NONE. Callers that
     * exercised taz_server_shutdown must already have closed its deadline
     * timer via taz_shutdown_finish; this never touches a handle or loop
     * itself. Does not reset work.c's own sticky flag - pair with
     * taz_work_reset_for_tests(). */
    void taz_shutdown_reset_for_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_SHUTDOWN_H */
