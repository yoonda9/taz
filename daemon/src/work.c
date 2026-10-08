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
    int release_stream; /* 1 to release stream_id after work, 0 for multi-step
                         */
} work_wrapper_t;

/* Pool thread: never touches d, only the caller's own *user. */
static void on_work(uv_work_t *req)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    w->work(w->user);
}

/* Count of taz_work_submit calls that have not yet reached the end of
 * on_after_work; touched only on the loop thread (taz_work_submit from
 * frame dispatch, on_after_work as a uv_queue_work completion), so needs
 * no lock. g_shutdown_loop is non-NULL once taz_work_request_shutdown has
 * been called and is still waiting for g_outstanding to drain.
 * g_shutdown_requested is set once taz_work_request_shutdown has been called
 * and persists (sticky) until the process exits. */
static size_t g_outstanding = 0U;
static uv_loop_t *g_shutdown_loop = NULL;
static int g_shutdown_requested = 0;

/* Loop thread, any status (including UV_ECANCELED, treated the same as a
 * normal completion): deliver the result, optionally release the stream,
 * unref the connection, then free the wrapper. */
static void on_after_work(uv_work_t *req, int status)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    int closing;
    (void)status;

    closing = taz_dispatch_conn_closing(w->d) || g_shutdown_requested;
    w->done(w->user, closing);
    if (w->release_stream)
    {
        taz_dispatch_stream_done(w->d, w->stream_id);
    }
    taz_dispatch_conn_unref(w->d);
    free(w);

    g_outstanding--;
    if (g_shutdown_loop != NULL && g_outstanding == 0U)
    {
        uv_loop_t *loop = g_shutdown_loop;
        g_shutdown_loop = NULL;
        uv_stop(loop);
    }
}

/* Common implementation for both submit variants. */
static int work_submit_internal(taz_dispatch_t *d, uint32_t stream_id,
                                taz_work_fn_t work, taz_work_done_fn_t done,
                                void *user, int release_stream)
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
    w->release_stream = release_stream;

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
    g_outstanding++;
    return 0;
}

int taz_work_submit(taz_dispatch_t *d, uint32_t stream_id, taz_work_fn_t work,
                    taz_work_done_fn_t done, void *user)
{
    return work_submit_internal(d, stream_id, work, done, user, 1);
}

int taz_work_submit_step(taz_dispatch_t *d, taz_work_fn_t work,
                         taz_work_done_fn_t done, void *user)
{
    /* stream_id is not used since we don't release; use 0 as a placeholder. */
    return work_submit_internal(d, 0, work, done, user, 0);
}

int taz_work_shutdown_requested(void)
{
    return g_shutdown_requested;
}

void taz_work_reset_for_tests(void)
{
    g_shutdown_requested = 0;
}

void taz_work_request_shutdown(uv_loop_t *loop)
{
    g_shutdown_requested = 1;
    if (g_outstanding == 0U)
    {
        uv_stop(loop);
        return;
    }
    g_shutdown_loop = loop;
}
