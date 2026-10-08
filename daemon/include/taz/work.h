#ifndef TAZ_WORK_H
#define TAZ_WORK_H

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
     * taz_work_request_shutdown. */
    int taz_work_submit_step(taz_dispatch_t *d, taz_work_fn_t work,
                             taz_work_done_fn_t done, void *user);

    /* 1 once taz_work_request_shutdown has been called (whether or not
     * uv_stop has fired). */
    int taz_work_shutdown_requested(void);

    /* Reset the shutdown_requested flag to 0. Unit tests only: allows tests
     * to run in sequence without the sticky flag from a prior shutdown test
     * breaking subsequent tests. */
    void taz_work_reset_for_tests(void);

    /* Call from a signal/console-ctrl handler running on loop's own thread
     * instead of uv_stop(loop) directly. uv_stop() halts the loop on the
     * next iteration regardless of outstanding uv_queue_work items; since
     * those run on a separate pool thread and post their completion back
     * into *loop once done, stopping (and the caller returning from
     * uv_run, then tearing down loop) while one is still in flight leaves
     * the pool thread to post into freed/invalid memory. This defers the
     * actual uv_stop() until every taz_work_submit call still outstanding
     * has reached its on-loop-thread completion, or issues it immediately
     * if none are outstanding. Idempotent; safe to call more than once. */
    void taz_work_request_shutdown(uv_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* TAZ_WORK_H */
