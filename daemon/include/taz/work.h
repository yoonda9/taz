#ifndef TAZ_WORK_H
#define TAZ_WORK_H

#include <stddef.h>
#include <stdint.h>

#include "taz/dispatch.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Runs on a thread-pool thread. Must touch only *user - never a
     * taz_dispatch_t, a connection, a uv_* handle, or stderr/stdout. */
    typedef void (*taz_work_fn_t)(void *user);

    /* Runs on the loop thread once work has returned. closing is 1 when
     * the owning connection began closing while work was in flight, in
     * which case the caller must send nothing; 0 otherwise. Owns freeing
     * *user. */
    typedef void (*taz_work_done_fn_t)(void *user, int closing);

    /* Pin the connection behind d, then run work on d->loop's thread pool
     * and, once it finishes (including a UV_ECANCELED status, treated the
     * same as completion), on the loop thread: call done(user, closing),
     * release stream_id via taz_dispatch_stream_done, and unref the
     * connection - in that order, exactly once.
     *
     * Returns 0 on success. On a UV_E* failure nothing is retained: no ref
     * is held, done is never called, and the caller still owns *user. */
    int taz_work_submit(taz_dispatch_t *d, uint32_t stream_id,
                        taz_work_fn_t work, taz_work_done_fn_t done,
                        void *user);

    /* Like taz_work_submit, but the stream is NOT released afterwards: one
     * step of a multi-step stream whose owner calls taz_dispatch_stream_done
     * itself. Still pins the connection, still counted by
     * taz_work_outstanding and cancellable by taz_work_begin_shutdown. */
    int taz_work_submit_step(taz_dispatch_t *d, taz_work_fn_t work,
                             taz_work_done_fn_t done, void *user);

    /* Like taz_work_submit_step, but taz_work_begin_shutdown never cancels
     * it: for cleanup steps (closing an fd, removing a temp file) whose
     * work must actually run even when the daemon is stopping. */
    int taz_work_submit_cleanup(taz_dispatch_t *d, taz_work_fn_t work,
                                taz_work_done_fn_t done, void *user);

    /* 1 once taz_work_begin_shutdown has been called. Sticky: stays 1 until
     * taz_work_reset_for_tests. */
    int taz_work_shutdown_requested(void);

    /* Number of taz_work_submit/taz_work_submit_step calls that have not
     * yet reached the end of on_after_work. Unit tests only. */
    size_t taz_work_outstanding(void);

    /* Reset the shutdown_requested flag to 0 and forget every outstanding
     * wrapper the module was tracking. Unit tests only: allows tests to
     * run in sequence without the sticky flag (or a stale list entry) from
     * a prior shutdown test breaking subsequent tests. */
    void taz_work_reset_for_tests(void);

    /* Call from a signal/console-ctrl handler, or the shutdown walk,
     * running on loop's own thread. Sets the sticky shutdown_requested
     * flag, then uv_cancels every taz_work_submit/taz_work_submit_step
     * call still outstanding (never a taz_work_submit_cleanup one): a call that
     * has not yet started on a pool thread is cancelled (its done callback runs
     * with closing=1 on the loop thread, as if it had completed); a call
     * already running on a pool thread gets UV_EBUSY from uv_cancel and simply
     * keeps going to completion, done(user, 1) still running once it does.
     * Never calls uv_stop: the loop is left to exit on its own once every
     * handle and request - including any still-running pool work, whose
     * uv_work_t keeps the loop alive - is gone. Idempotent; safe to call more
     * than once. */
    void taz_work_begin_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_WORK_H */
