#include "restart.h"

#include <stdbool.h>
#include <stdlib.h>

#include <pb_decode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/log.h"
#include "taz/response.h"
#include "taz/shutdown.h"
#include "taz/v1/daemon_control.pb.h"
#include "taz/work.h"

/* One acknowledged RESTART whose response has not left its connection's
 * write queue yet: the stream stays open and the connection referenced
 * until then. Heap-allocated, ~100 B; freed by detach. */
typedef struct restart_req_s
{
    taz_dispatch_t *d;
    uint32_t stream_id;
    uint64_t effective_delay_ms;

    struct restart_req_s *prev;
    struct restart_req_s *next;

    taz_stream_ops_t ops;
} restart_req_t;

/* Every attached request, so taz_restart_close_pending can let go of them
 * all. Loop thread only. */
static restart_req_t *g_attached = NULL;

/* The daemon's one restart timer. Armed by the first request whose
 * response has been written and moved earlier by any later request with a
 * shorter delay, so however many RESTARTs arrive, one timer (the earliest
 * deadline) decides when the daemon restarts. It belongs to the daemon,
 * not to any connection: the requester disconnecting never cancels it.
 * Closed by its own firing or by taz_restart_close_pending. NULL when no
 * request has needed it yet. Loop thread only. */
static uv_timer_t *g_timer = NULL;
static int g_armed = 0;

static void attached_link(restart_req_t *s)
{
    s->prev = NULL;
    s->next = g_attached;
    if (g_attached != NULL)
    {
        g_attached->prev = s;
    }
    g_attached = s;
}

static void attached_unlink(restart_req_t *s)
{
    if (s->prev != NULL)
    {
        s->prev->next = s->next;
    }
    else
    {
        g_attached = s->next;
    }
    if (s->next != NULL)
    {
        s->next->prev = s->prev;
    }
}

/* Ends the RESTART stream, releases the connection reference and frees
 * s. The timer (if armed) keeps running without it. */
static void detach(restart_req_t *s)
{
    taz_dispatch_t *d = s->d;
    const uint32_t stream_id = s->stream_id;

    attached_unlink(s);
    free(s);
    taz_dispatch_stream_done(d, stream_id);
    taz_dispatch_conn_unref(d);
}

static void on_timer_closed(uv_handle_t *handle)
{
    free(handle);
}

static void close_timer(void)
{
    if (g_timer == NULL)
    {
        return;
    }
    (void)uv_timer_stop(g_timer);
    uv_close((uv_handle_t *)g_timer, on_timer_closed);
    g_timer = NULL;
    g_armed = 0;
}

/* Closed first, so the walk taz_shutdown_request starts (and its
 * taz_restart_close_pending) finds nothing left to close. */
static void on_restart_timer(uv_timer_t *handle)
{
    (void)handle;
    close_timer();
    taz_shutdown_request(TAZ_SHUTDOWN_RESTART);
}

static int ensure_timer(uv_loop_t *loop)
{
    uv_timer_t *timer;
    int rc;

    if (g_timer != NULL)
    {
        return 0;
    }
    timer = (uv_timer_t *)malloc(sizeof(*timer));
    if (timer == NULL)
    {
        return UV_ENOMEM;
    }
    rc = uv_timer_init(loop, timer);
    if (rc != 0)
    {
        free(timer);
        return rc;
    }
    g_timer = timer;
    g_armed = 0;
    return 0;
}

/* Restart delay_ms from now, unless the daemon is already due to restart
 * sooner. */
static void arm(uv_loop_t *loop, uint64_t delay_ms)
{
    if (ensure_timer(loop) != 0)
    {
        taz_log(TAZ_LOG_ERROR, "restart: cannot arm the restart timer");
        return;
    }
    if (g_armed && uv_timer_get_due_in(g_timer) <= delay_ms)
    {
        return;
    }
    g_armed = 1;
    (void)uv_timer_start(g_timer, on_restart_timer, delay_ms, 0U);
}

/* Arms the timer the first time the connection's write queue is empty -
 * the response (queued before it) has then been written - and hands the
 * request over to the daemon. */
static void try_arm(restart_req_t *s)
{
    if (taz_dispatch_conn_write_queue_size(s->d) != 0U)
    {
        return;
    }
    arm(s->d->loop, s->effective_delay_ms);
    detach(s);
}

static void restart_on_writable(void *user)
{
    try_arm((restart_req_t *)user);
}

/* A restart cannot be cancelled once acknowledged: always refuses. */
static int restart_cancel(void *user, taz_stream_done_fn_t done, void *done_arg)
{
    (void)user;
    (void)done;
    (void)done_arg;
    return 0;
}

/* The requesting connection is closing before its write queue drained.
 * The restart was already acknowledged, so it still happens: arm the
 * timer now (the response will never finish writing) and let go of the
 * connection. Once shutdown has begun the restart can no longer happen
 * (a STOP won, or a restart is already under way): just let go. */
static void restart_abort(void *user)
{
    restart_req_t *s = (restart_req_t *)user;

    if (!taz_work_shutdown_requested())
    {
        arm(s->d->loop, s->effective_delay_ms);
    }
    detach(s);
}

/* RESTART never reads a FILE_CHUNK; required by taz_stream_ops_t regardless,
 * since taz_dispatch_frame calls on_chunk unconditionally on any stream with
 * ops registered. */
static void restart_on_chunk(void *user, const taz_frame_header_t *header,
                             const uint8_t *payload)
{
    (void)user;
    (void)header;
    (void)payload;
}

void taz_restart_close_pending(void)
{
    while (g_attached != NULL)
    {
        detach(g_attached);
    }
    close_timer();
}

void handle_restart(taz_dispatch_t *d, const taz_frame_header_t *header,
                    const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                    void *ctx)
{
    taz_v1_RestartRequest req = taz_v1_RestartRequest_init_zero;
    taz_v1_RestartResponse resp = taz_v1_RestartResponse_init_zero;
    restart_req_t *s;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_RestartRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode RestartRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    /* Once shutdown has begun (this daemon is already stopping or
     * restarting), every other request is refused the same way DETACH is -
     * a second restart cannot be queued behind the first. */
    if (taz_work_shutdown_requested())
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_BUSY,
                       "daemon is shutting down", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    /* The timer exists before the response says acknowledged=true, so
     * arming it later cannot fail for want of memory. */
    s = (restart_req_t *)calloc(1U, sizeof(*s));
    if (s == NULL || ensure_timer(d->loop) != 0)
    {
        free(s);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "restart failed",
                       "out of memory");
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    s->d = d;
    s->stream_id = header->stream_id;
    s->effective_delay_ms = (req.delay_ms < TAZ_RESTART_MIN_DELAY_MS)
                                ? (uint64_t)TAZ_RESTART_MIN_DELAY_MS
                                : (uint64_t)req.delay_ms;
    attached_link(s);

    s->ops.on_chunk = restart_on_chunk;
    s->ops.cancel = restart_cancel;
    s->ops.abort = restart_abort;
    s->ops.on_writable = restart_on_writable;
    taz_dispatch_set_stream_ops(d, header->stream_id, &s->ops, s);
    taz_dispatch_conn_ref(d);

    resp.acknowledged = true;
    resp.effective_delay_ms = (uint32_t)s->effective_delay_ms;
    taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                      taz_v1_RestartResponse_fields, &resp);

    try_arm(s);
}
