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

#ifdef __cplusplus
}
#endif

#endif /* TAZ_WORK_H */
