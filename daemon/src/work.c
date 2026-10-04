#include "taz/work.h"

#include <stdlib.h>

#include <uv.h>

/* Heap-allocated per submit; the uv_work_t must be the first member so the
 * libuv callbacks can cast back. Lives from taz_work_submit until
 * on_after_work frees it, exactly once. */
typedef struct
{
    uv_work_t req; /* must be first */
    taz_dispatch_t *d;
    uint32_t stream_id;
    taz_work_fn_t work;
    taz_work_done_fn_t done;
    void *user;
} work_wrapper_t;

/* Pool thread: never touches d, only the caller's own *user. */
static void on_work(uv_work_t *req)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    w->work(w->user);
}

/* Loop thread, any status (including UV_ECANCELED, treated the same as a
 * normal completion): deliver the result, release the stream, unref the
 * connection, then free the wrapper. */
static void on_after_work(uv_work_t *req, int status)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    int closing;
    (void)status;

    closing = taz_dispatch_conn_closing(w->d);
    w->done(w->user, closing);
    taz_dispatch_stream_done(w->d, w->stream_id);
    taz_dispatch_conn_unref(w->d);
    free(w);
}

int taz_work_submit(taz_dispatch_t *d, uint32_t stream_id, taz_work_fn_t work,
                    taz_work_done_fn_t done, void *user)
{
    work_wrapper_t *w;
    int rc;

    w = (work_wrapper_t *)malloc(sizeof(*w));
    if (w == NULL)
    {
        return UV_ENOMEM;
    }
    w->d = d;
    w->stream_id = stream_id;
    w->work = work;
    w->done = done;
    w->user = user;

    taz_dispatch_conn_ref(d);
    /* Cast the wrapper itself (req is its first member) rather than taking
     * &w->req: the latter reads to cppcheck as an address that never
     * escapes this function, since it cannot see that uv_queue_work
     * retains it past the call - a false memleak on the success path. */
    rc = uv_queue_work(d->loop, (uv_work_t *)w, on_work, on_after_work);
    if (rc != 0)
    {
        taz_dispatch_conn_unref(d);
        free(w);
        return rc;
    }
    return 0;
}
