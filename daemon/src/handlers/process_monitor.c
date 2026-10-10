#include "process_monitor.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <uv.h>

#include "process.h"
#include "taz/error.h"
#include "taz/v1/common.pb.h"
#include "taz/v1/process.pb.h"
#include "taz/work.h"

/* Any value (including an empty string) disables pidfd for the request,
 * the same way taz_process_monitor_set_pidfd_enabled(0) does for unit
 * tests; the integration tests use it to reach the timer-only path. Read at
 * request time, not cached, so a test can flip it between requests. */
#define TAZ_MONITOR_NO_PIDFD_ENV "TAZ_MONITOR_NO_PIDFD"

static int g_pidfd_enabled = 1;
static taz_process_sample_fn_t g_sample_fn = taz_process_watch_sample;

void taz_process_monitor_set_sample_fn(taz_process_sample_fn_t fn)
{
    g_sample_fn = (fn != NULL) ? fn : taz_process_watch_sample;
}

void taz_process_monitor_set_pidfd_enabled(int enabled)
{
    g_pidfd_enabled = enabled;
}

int taz_process_monitor_pidfd_enabled(void)
{
    return g_pidfd_enabled;
}

uint64_t taz_process_monitor_interval_ms(uint32_t requested)
{
    return (requested < TAZ_PROCESS_MONITOR_MIN_INTERVAL_MS)
               ? (uint64_t)TAZ_PROCESS_MONITOR_MIN_INTERVAL_MS
               : (uint64_t)requested;
}

static int monitor_env_disables_pidfd(void)
{
    char value[2];
    size_t size = sizeof(value);

    return uv_os_getenv(TAZ_MONITOR_NO_PIDFD_ENV, value, &size) != UV_ENOENT;
}

typedef enum
{
    MON_RUNNING = 0,
    MON_ENDING = 1,
    MON_CLOSING = 2
} monitor_state_t;

/* Heap-allocated per MONITOR stream; freed by on_monitor_handle_closed once
 * every owned handle has closed and no sample is in flight. ~1 KB - fine on
 * the heap, never a VLA. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    uint32_t pid;
    uint64_t interval_ms;

    uv_timer_t timer;
    uv_poll_t poll;
    int poll_open; /* poll initialised: teardown must close it */

    taz_process_watch_t watch;

    /* Pool-owned while sample_in_flight; the loop thread only reads these
     * once monitor_sample_done has set sample_in_flight back to 0.
     * sample_fn is set on the loop thread just before each submit. */
    taz_process_sample_fn_t sample_fn;
    taz_process_sample_t sample;
    int sample_ok;
    taz_v1_ErrorCode sample_code;
    const char *sample_detail;
    uint64_t sample_hrtime;
    int sample_in_flight;

    int updates_sent;
    uint64_t prev_cpu_ns;
    uint64_t prev_hrtime;
    taz_process_entry_t last_info; /* .pid set at creation; other fields
                                      filled by the first LIVE update, if
                                      any */

    monitor_state_t state;
    int exit_pending; /* the pidfd fired while a sample was in flight */

    /* Set together by the cancel op while a CANCEL is outstanding; NULL
     * once fired (monitor_fire_cancel_done), same precedent as
     * file_transfer.c's put_cancel/put_fire_cancel_done. */
    taz_stream_done_fn_t cancel_done;
    void *cancel_done_arg;

    int aborted;
    int final_sent;
    unsigned closes_pending;

    taz_stream_ops_t ops;
} monitor_ctx_t;

static void monitor_copy_info(taz_v1_ProcessInfo *dst,
                              const taz_process_entry_t *src, float cpu_percent)
{
    (void)memset(dst, 0, sizeof(*dst));
    dst->pid = src->pid;
    (void)strncpy(dst->name, src->name, sizeof(dst->name) - 1U);
    dst->name[sizeof(dst->name) - 1U] = '\0';
    (void)strncpy(dst->user, src->user, sizeof(dst->user) - 1U);
    dst->user[sizeof(dst->user) - 1U] = '\0';
    (void)strncpy(dst->state, src->state, sizeof(dst->state) - 1U);
    dst->state[sizeof(dst->state) - 1U] = '\0';
    dst->cpu_percent = cpu_percent;
    dst->memory_bytes = src->memory_bytes;
}

/* Sends one update RESPONSE (CONTINUATION set) for info/cpu_percent, unless
 * the stream has already ended or the connection is closing. Always counts
 * the update, even when the send itself was suppressed, since that can
 * only happen on a state this function's callers never reach in practice
 * (final_sent implies the caller already stopped calling this). */
static void monitor_send_update(monitor_ctx_t *m,
                                const taz_process_entry_t *info,
                                float cpu_percent)
{
    taz_v1_ProcessMonitorResponse resp =
        taz_v1_ProcessMonitorResponse_init_zero;

    resp.has_info = true;
    monitor_copy_info(&resp.info, info, cpu_percent);
    resp.exited = false;
    resp.exit_code = 0;
    resp.exit_code_known = false;

    if (!m->final_sent && !taz_dispatch_conn_closing(m->d))
    {
        taz_process_send_frame(
            m->write_fn, m->write_ctx, m->stream_id, m->opcode,
            taz_v1_ProcessMonitorResponse_fields, &resp,
            (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
    }
    m->updates_sent++;
}

/* Sends the one and only final RESPONSE for this stream (CONTINUATION
 * clear), unless it was already sent or the connection is closing. Marks
 * the stream ending/ended either way: every other end path (ERROR) must
 * see final_sent already set and refuse to send anything more. Shared by
 * the exit path (monitor_send_final) and the cancel path
 * (monitor_send_cancelled). */
static void monitor_send_final_frame(monitor_ctx_t *m,
                                     const taz_process_entry_t *info,
                                     bool exited, int32_t exit_code,
                                     int exit_code_known, const char *reason)
{
    taz_v1_ProcessMonitorResponse resp =
        taz_v1_ProcessMonitorResponse_init_zero;

    resp.has_info = true;
    monitor_copy_info(&resp.info, info, 0.0F);
    resp.exited = exited;
    resp.exit_code = exit_code;
    resp.exit_code_known = (exit_code_known != 0);
    (void)strncpy(resp.reason, reason, sizeof(resp.reason) - 1U);
    resp.reason[sizeof(resp.reason) - 1U] = '\0';

    m->state = MON_ENDING;
    if (!m->final_sent && !taz_dispatch_conn_closing(m->d))
    {
        taz_process_send_frame(m->write_fn, m->write_ctx, m->stream_id,
                               m->opcode, taz_v1_ProcessMonitorResponse_fields,
                               &resp,
                               (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
    }
    m->final_sent = 1;
}

static void monitor_send_final(monitor_ctx_t *m,
                               const taz_process_entry_t *info,
                               int32_t exit_code, int exit_code_known)
{
    monitor_send_final_frame(m, info, true, exit_code, exit_code_known,
                             "exited");
}

/* The final frame for an accepted CANCEL always reports
 * exited=false, even when the sample that completed it saw the process
 * gone - the cancel's own verdict (sent by cancel.c once monitor_fire_
 * cancel_done fires) must agree with what the stream itself just said. */
static void monitor_send_cancelled(monitor_ctx_t *m)
{
    monitor_send_final_frame(m, &m->last_info, false, 0, 0, "cancelled");
}

/* Fires a pending CANCEL's done callback exactly once; a no-op once
 * already fired (or if no cancel is pending). Precedent: file_transfer.c's
 * put_fire_cancel_done/get_fire_cancel_done. */
static void monitor_fire_cancel_done(monitor_ctx_t *m)
{
    if (m->cancel_done != NULL)
    {
        const taz_stream_done_fn_t done = m->cancel_done;
        void *const arg = m->cancel_done_arg;
        m->cancel_done = NULL;
        m->cancel_done_arg = NULL;
        done(arg);
    }
}

/* Ends the stream on a failure that is not "the process is gone". Before
 * the first update the request itself failed, so an ERROR frame carries the
 * code, as for a nonexistent pid. Once updates have flowed, the stream ends
 * as api.md §3.4 says: a final RESPONSE with reason "error" and the last
 * known info. */
static void monitor_send_failure(monitor_ctx_t *m, taz_v1_ErrorCode code,
                                 const char *message, const char *detail)
{
    if (m->updates_sent > 0)
    {
        monitor_send_final_frame(m, &m->last_info, false, 0, 0, "error");
        return;
    }
    m->state = MON_ENDING;
    if (!m->final_sent && !taz_dispatch_conn_closing(m->d))
    {
        taz_error_send(m->write_fn, m->write_ctx, m->stream_id, m->opcode, code,
                       message, detail);
    }
    m->final_sent = 1;
}

/* Last close callback: taz_process_watch_close only after the poll handle
 * (if any) has closed - libuv requires the fd to stay open until then. */
static void on_monitor_handle_closed(uv_handle_t *handle)
{
    monitor_ctx_t *m = (monitor_ctx_t *)handle->data;

    if (--m->closes_pending > 0U)
    {
        return;
    }

    taz_process_watch_close(&m->watch);

    /* Backstop: every path that can leave a cancel pending at this point
     * already fired it (monitor_sample_done's closing/aborted and
     * cancel-pending branches); never lost regardless, since cancel.c
     * would otherwise leak its ctx and keep the CANCEL stream open. */
    monitor_fire_cancel_done(m);

    taz_dispatch_stream_done(m->d, m->stream_id);
    taz_dispatch_conn_unref(m->d);
    free(m);
}

/* Idempotent: a deferred call (sample_in_flight was set) is resumed by
 * monitor_sample_done once the sample completes. */
static void monitor_teardown(monitor_ctx_t *m)
{
    if (m->state == MON_CLOSING)
    {
        return;
    }
    if (m->sample_in_flight)
    {
        return;
    }

    m->state = MON_CLOSING;
    uv_timer_stop(&m->timer);
    m->closes_pending = 1U + (m->poll_open ? 1U : 0U);
    uv_close((uv_handle_t *)&m->timer, on_monitor_handle_closed);
    if (m->poll_open)
    {
        uv_close((uv_handle_t *)&m->poll, on_monitor_handle_closed);
    }
}

static void monitor_sample_work(void *user)
{
    monitor_ctx_t *m = (monitor_ctx_t *)user;

    (void)memset(&m->sample, 0, sizeof(m->sample));
    m->sample_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    m->sample_detail = NULL;
    m->sample_ok = (m->sample_fn(&m->watch, &m->sample, &m->sample_code,
                                 &m->sample_detail) == 0);
    m->sample_hrtime = uv_hrtime();
}

static void monitor_sample_done(void *user, int closing)
{
    monitor_ctx_t *m = (monitor_ctx_t *)user;

    m->sample_in_flight = 0;

    if (closing || m->aborted)
    {
        /* No frame either way (abort: never again; closing: suppressed by
         * monitor_send_final_frame's own check) - but a CANCEL accepted
         * while this sample was in flight still needs its done fired, or
         * cancel.c would leak its ctx and keep the CANCEL stream open. */
        monitor_fire_cancel_done(m);
        monitor_teardown(m);
        return;
    }

    if (m->cancel_done != NULL)
    {
        /* A pending cancel wins over a simultaneous exit (EXITED or
         * exit_pending), even though the sample that just completed may
         * say otherwise. */
        monitor_send_cancelled(m);
        monitor_fire_cancel_done(m);
        monitor_teardown(m);
        return;
    }

    if (!m->sample_ok)
    {
        /* A process that vanished after at least one update is "exited".
         * NOT_FOUND before any update means the pid never existed (the
         * timer-only path does not check at open), and any other failure
         * is a failure. */
        if (m->sample_code == taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND &&
            m->updates_sent > 0)
        {
            monitor_send_final(m, &m->last_info, 0, 0);
        }
        else
        {
            monitor_send_failure(m, m->sample_code, "monitor failed",
                                 m->sample_detail);
        }
        monitor_teardown(m);
        return;
    }

    if (m->sample.state == TAZ_PROCESS_SAMPLE_EXITED || m->exit_pending)
    {
        /* The ending sample's own fields when it reported the exit and has
         * them (a Linux zombie). Otherwise the last live sample's: the
         * pidfd fired while this sample still saw the process live, or
         * the sample came from Windows, which reports only the pid of an
         * exited process. */
        const taz_process_entry_t *info =
            (m->sample.state == TAZ_PROCESS_SAMPLE_EXITED &&
             m->sample.info.name[0] != '\0')
                ? &m->sample.info
                : &m->last_info;
        monitor_send_final(m, info, m->sample.exit_code,
                           m->sample.exit_code_known);
        monitor_teardown(m);
        return;
    }

    /* LIVE. */
    {
        float cpu_percent = 0.0F;

        if (m->updates_sent > 0)
        {
            cpu_percent = taz_process_interval_cpu_percent(
                m->sample.cpu_time_ns - m->prev_cpu_ns,
                m->sample_hrtime - m->prev_hrtime);
        }
        m->prev_cpu_ns = m->sample.cpu_time_ns;
        m->prev_hrtime = m->sample_hrtime;
        m->last_info = m->sample.info;
        if (m->watch.first_starttime == 0U)
        {
            m->watch.first_starttime = m->sample.starttime;
        }
        monitor_send_update(m, &m->sample.info, cpu_percent);
    }
}

static void on_tick(uv_timer_t *handle)
{
    monitor_ctx_t *m = (monitor_ctx_t *)handle->data;

    if (m->state != MON_RUNNING || m->sample_in_flight ||
        taz_work_shutdown_requested() || taz_dispatch_conn_closing(m->d))
    {
        return;
    }

    m->sample_in_flight = 1;
    m->sample_fn = g_sample_fn;
    if (taz_work_submit_step(m->d, monitor_sample_work, monitor_sample_done,
                             m) != 0)
    {
        m->sample_in_flight = 0;
        monitor_send_failure(m, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                             "work submit failed", NULL);
        monitor_teardown(m);
    }
}

static void on_exit_readable(uv_poll_t *handle, int status, int events)
{
    monitor_ctx_t *m = (monitor_ctx_t *)handle->data;

    (void)status;
    (void)events;

    uv_poll_stop(&m->poll);

    if (m->state != MON_RUNNING)
    {
        return;
    }

    m->exit_pending = 1;
    if (m->sample_in_flight)
    {
        return; /* monitor_sample_done's exit_pending check handles it. */
    }

    /* The exit code is unknown on POSIX - this callback only ever fires on
     * Linux, where the watch's exit_fd is a pidfd. */
    monitor_send_final(m, &m->last_info, 0, 0);
    monitor_teardown(m);
}

static void monitor_on_chunk(void *user, const taz_frame_header_t *header,
                             const uint8_t *payload)
{
    (void)user;
    (void)header;
    (void)payload;
}

/* Refuses (0) once the stream has ended, is ending, or is closing
 * (state != MON_RUNNING), or a CANCEL is already pending for it -
 * CancelSameTargetTwiceInARowReturnsTrueThenFalse and "CANCEL for an ended
 * stream replies false" both fall out of this one check. Otherwise
 * accepts: if a sample is in flight, done fires later from
 * monitor_sample_done's cancel branch; idle, it fires synchronously, right
 * after the final "cancelled" frame (wire order: that frame, then
 * cancel.c's CancelResponse). */
static int monitor_cancel(void *user, taz_stream_done_fn_t done, void *done_arg)
{
    monitor_ctx_t *m = (monitor_ctx_t *)user;

    if (m->state != MON_RUNNING || m->cancel_done != NULL)
    {
        return 0;
    }

    m->cancel_done = done;
    m->cancel_done_arg = done_arg;

    if (m->sample_in_flight)
    {
        return 1;
    }

    monitor_send_cancelled(m);
    monitor_fire_cancel_done(m);
    monitor_teardown(m);
    return 1;
}

static void monitor_abort(void *user)
{
    monitor_ctx_t *m = (monitor_ctx_t *)user;

    m->aborted = 1;
    /* Teardown already began in this loop iteration (e.g. a CANCEL and the
     * client's disconnect arrived in one read): the stream stays in the
     * dispatch table until its close callbacks run, its handles are
     * closing, and libuv asserts on stopping a closing handle. */
    if (m->state == MON_CLOSING)
    {
        return;
    }
    uv_timer_stop(&m->timer);
    if (m->poll_open)
    {
        uv_poll_stop(&m->poll);
    }
    if (!m->sample_in_flight)
    {
        monitor_teardown(m);
    }
    /* else: deferred to monitor_sample_done's closing/aborted branch. */
}

/* Frees a watch opened with no handles/ops/ref registered yet - only
 * handle_process_monitor's early-return paths use this. */
static void monitor_ctx_free_unopened(monitor_ctx_t *m)
{
    taz_process_watch_close(&m->watch);
    free(m);
}

void handle_process_monitor(taz_dispatch_t *d, const taz_frame_header_t *header,
                            const uint8_t *payload,
                            taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_ProcessMonitorRequest req = taz_v1_ProcessMonitorRequest_init_zero;
    monitor_ctx_t *m;
    taz_v1_ErrorCode open_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
    const char *open_detail = NULL;
    int use_pidfd;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_ProcessMonitorRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode ProcessMonitorRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    {
        const char *pid_msg = taz_process_pid_check(req.pid);
        if (pid_msg != NULL)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST, pid_msg,
                           NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    m = (monitor_ctx_t *)calloc(1U, sizeof(*m));
    if (m == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    m->d = d;
    m->write_fn = write_fn;
    m->write_ctx = ctx;
    m->stream_id = header->stream_id;
    m->opcode = header->opcode;
    m->pid = req.pid;
    m->interval_ms = taz_process_monitor_interval_ms(req.interval_ms);
    m->last_info.pid = req.pid;
    m->state = MON_RUNNING;

    use_pidfd =
        taz_process_monitor_pidfd_enabled() && !monitor_env_disables_pidfd();

    if (taz_process_watch_open(m->pid, use_pidfd, &m->watch, &open_code,
                               &open_detail) != 0)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       open_code, "monitor failed", open_detail);
        free(m);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    m->ops.on_chunk = monitor_on_chunk;
    m->ops.cancel = monitor_cancel;
    m->ops.abort = monitor_abort;
    m->ops.on_writable = NULL;
    taz_dispatch_set_stream_ops(d, header->stream_id, &m->ops, m);
    taz_dispatch_conn_ref(d);

    if (uv_timer_init(d->loop, &m->timer) != 0)
    {
        taz_dispatch_conn_unref(d);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "monitor failed",
                       "uv_timer_init failed");
        taz_dispatch_stream_done(d, header->stream_id);
        monitor_ctx_free_unopened(m);
        return;
    }
    m->timer.data = m;

    if (m->watch.exit_fd >= 0)
    {
        if (uv_poll_init(d->loop, &m->poll, m->watch.exit_fd) == 0)
        {
            /* Owned from here on: teardown closes it, counted in
             * closes_pending, and the pidfd stays open until its close
             * callback (libuv needs the fd until then). If it cannot be
             * started, the timer's samples still catch the exit: they
             * check for a zombie and a changed start time whether or not a
             * pidfd exists. */
            m->poll.data = m;
            m->poll_open = 1;
            (void)uv_poll_start(&m->poll, UV_READABLE, on_exit_readable);
        }
        else
        {
            taz_v1_ErrorCode reopen_code = taz_v1_ErrorCode_ERROR_CODE_UNKNOWN;
            const char *reopen_detail = NULL;

            /* No handle took the pidfd: close the watch and reopen with
             * use_pidfd 0, which never fails and never syscalls, for the
             * timer-only path. */
            taz_process_watch_close(&m->watch);
            (void)taz_process_watch_open(m->pid, 0, &m->watch, &reopen_code,
                                         &reopen_detail);
        }
    }

    if (uv_timer_start(&m->timer, on_tick, 0, m->interval_ms) != 0)
    {
        monitor_send_failure(m, taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                             "monitor failed", "uv_timer_start failed");
        monitor_teardown(m);
        return;
    }
}
