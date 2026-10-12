#include "taz/work.h"

#include <stdlib.h>

#include <uv.h>

/* Heap-allocated per submit; the uv_work_t must be the first member so the
 * libuv callbacks can cast back. Lives from taz_work_submit until
 * on_after_work frees it, exactly once. prev/next link it into
 * g_outstanding_head - the loop thread's own record of every wrapper that
 * has not yet reached on_after_work, used by taz_work_begin_shutdown to
 * uv_cancel every one of them. Touched only on the loop thread (submit and
 * on_after_work both run there), so it needs no lock. */
typedef struct work_wrapper
{
    uv_work_t req; /* must be first */
    taz_dispatch_t *d;
    uint32_t stream_id;
    taz_work_fn_t work;
    taz_work_done_fn_t done;
    void *user;
    int release_stream; /* 1 to release stream_id after work, 0 for multi-step
                         */
    int cancellable;    /* 0 for cleanup work taz_work_begin_shutdown skips */
    struct work_wrapper *prev;
    struct work_wrapper *next;
} work_wrapper_t;

/* Pool thread: never touches d, only the caller's own *user. */
static void on_work(uv_work_t *req)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    w->work(w->user);
}

/* Count of taz_work_submit calls that have not yet reached the end of
 * on_after_work, and the intrusive list of their wrappers (loop thread
 * only). g_shutdown_requested is set once taz_work_begin_shutdown has been
 * called and persists (sticky) until taz_work_reset_for_tests. */
static size_t g_outstanding = 0U;
static work_wrapper_t *g_outstanding_head = NULL;
static int g_shutdown_requested = 0;

static void list_insert(work_wrapper_t *w)
{
    w->prev = NULL;
    w->next = g_outstanding_head;
    if (g_outstanding_head != NULL)
    {
        g_outstanding_head->prev = w;
    }
    g_outstanding_head = w;
}

static void list_remove(work_wrapper_t *w)
{
    if (w->prev != NULL)
    {
        w->prev->next = w->next;
    }
    else
    {
        g_outstanding_head = w->next;
    }
    if (w->next != NULL)
    {
        w->next->prev = w->prev;
    }
}

/* Loop thread, any status (including UV_ECANCELED, treated the same as a
 * normal completion): deliver the result, optionally release the stream,
 * unref the connection, then free the wrapper. */
static void on_after_work(uv_work_t *req, int status)
{
    work_wrapper_t *w = (work_wrapper_t *)req;
    int closing;
    (void)status;

    list_remove(w);
    g_outstanding--;

    closing = taz_dispatch_conn_closing(w->d) || g_shutdown_requested;
    w->done(w->user, closing);
    if (w->release_stream)
    {
        taz_dispatch_stream_done(w->d, w->stream_id);
    }
    taz_dispatch_conn_unref(w->d);
    free(w);
}

/* Common implementation for both submit variants. */
static int work_submit_internal(taz_dispatch_t *d, uint32_t stream_id,
                                taz_work_fn_t work, taz_work_done_fn_t done,
                                void *user, int release_stream, int cancellable)
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
    w->cancellable = cancellable;

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
    list_insert(w);
    g_outstanding++;
    return 0;
}

int taz_work_submit(taz_dispatch_t *d, uint32_t stream_id, taz_work_fn_t work,
                    taz_work_done_fn_t done, void *user)
{
    return work_submit_internal(d, stream_id, work, done, user, 1, 1);
}

int taz_work_submit_step(taz_dispatch_t *d, taz_work_fn_t work,
                         taz_work_done_fn_t done, void *user)
{
    /* stream_id is not used since we don't release; use 0 as a placeholder. */
    return work_submit_internal(d, 0, work, done, user, 0, 1);
}

int taz_work_submit_cleanup(taz_dispatch_t *d, taz_work_fn_t work,
                            taz_work_done_fn_t done, void *user)
{
    return work_submit_internal(d, 0, work, done, user, 0, 0);
}

int taz_work_shutdown_requested(void)
{
    return g_shutdown_requested;
}

size_t taz_work_outstanding(void)
{
    return g_outstanding;
}

void taz_work_reset_for_tests(void)
{
    g_shutdown_requested = 0;
    g_outstanding_head = NULL;
    g_outstanding = 0U;
}

void taz_work_begin_shutdown(void)
{
    work_wrapper_t *w;

    g_shutdown_requested = 1;
    for (w = g_outstanding_head; w != NULL; w = w->next)
    {
        if (!w->cancellable)
        {
            continue;
        }
        /* Cancels it if still queued (on_after_work then runs with
         * UV_ECANCELED, treated like any other completion); UV_EBUSY if
         * it already started on a pool thread, which just means it keeps
         * running to its own completion - nothing to do either way. */
        (void)uv_cancel((uv_req_t *)&w->req);
    }
}
