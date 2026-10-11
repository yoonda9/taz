#include "task_status.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/response.h"
#include "taz/task.h"
#include "taz/v1/advanced.pb.h"
#include "taz/v1/common.pb.h"

uint64_t taz_task_status_interval_ms(uint32_t requested)
{
    return (requested < TAZ_TASK_STATUS_MIN_INTERVAL_MS)
               ? (uint64_t)TAZ_TASK_STATUS_MIN_INTERVAL_MS
               : (uint64_t)requested;
}

static bool is_terminal_state(taz_v1_TaskState state)
{
    switch (state)
    {
        case taz_v1_TaskState_TASK_STATE_COMPLETED:
        case taz_v1_TaskState_TASK_STATE_FAILED:
        case taz_v1_TaskState_TASK_STATE_CANCELLED:
            return true;
        default:
            return false;
    }
}

/* Context for encode_result: a pointer/length pair into a detached_task_t's
 * own stored result buffer, which outlives any single dispatch (freed only
 * by taz_task_free/_store_result), so it stays valid for the duration of
 * the taz_response_send call below. */
typedef struct
{
    const uint8_t *data;
    size_t len;
} bytes_ctx_t;

static bool encode_result(pb_ostream_t *stream, const pb_field_iter_t *field,
                          void *const *arg)
{
    const bytes_ctx_t *bctx = (const bytes_ctx_t *)*arg;
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, bctx->data, bctx->len);
}

/* Populates *resp from *task. result is attached (via encode_result,
 * pointing at *result_ctx, which must outlive the caller's encode) only
 * when include_result and the task is COMPLETED/FAILED - a subscribed
 * stream's frames never carry it (result_ctx is then unused and may be
 * NULL): it can be 1 MiB, and protocol.md says a streamed frame is a
 * complete, independent message. error is attached whenever the task is
 * FAILED, in either mode. */
static void build_status_response(taz_v1_TaskStatusResponse *resp,
                                  const detached_task_t *task,
                                  bool include_result, bytes_ctx_t *result_ctx)
{
    (void)memset(resp, 0, sizeof(*resp));
    resp->state = task->state;
    resp->pid = task->pid;

    if (include_result && task->result_len > 0U &&
        (task->state == taz_v1_TaskState_TASK_STATE_COMPLETED ||
         task->state == taz_v1_TaskState_TASK_STATE_FAILED))
    {
        result_ctx->data = task->result;
        result_ctx->len = task->result_len;
        resp->result.funcs.encode = encode_result;
        resp->result.arg = result_ctx;
    }

    if (task->state == taz_v1_TaskState_TASK_STATE_FAILED)
    {
        resp->has_error = true;
        resp->error.code = task->error_code;
        (void)strncpy(resp->error.message, task->error_message,
                      sizeof(resp->error.message) - 1U);
        resp->error.message[sizeof(resp->error.message) - 1U] = '\0';
    }
}

/* Packs and writes one RESPONSE frame carrying payload[0..pay_len). Mirrors
 * response.c's own (private) flush_frame and process.c's
 * process_flush_single_frame: crossing a function boundary here keeps
 * GCC's -fanalyzer from losing track of the malloc'd pointer's nullability
 * across the whole malloc-encode-pack-memcpy sequence. */
static void task_status_flush_frame(taz_dispatch_write_fn_t write_fn, void *ctx,
                                    uint32_t stream_id, uint16_t opcode,
                                    const uint8_t *payload, size_t pay_len,
                                    uint8_t flags)
{
    const size_t total = (size_t)TAZ_FRAME_HEADER_SIZE + pay_len;
    uint8_t *buf = (uint8_t *)malloc(total);

    if (buf == NULL)
    {
        return;
    }

    {
        taz_frame_header_t h;
        h.type = (uint8_t)taz_v1_FrameType_FRAME_TYPE_RESPONSE;
        h.flags = flags;
        h.opcode = opcode;
        h.length = (uint32_t)pay_len;
        h.stream_id = stream_id;
        taz_frame_pack_header(&h, buf);
    }
    if (pay_len > 0U)
    {
        (void)memcpy(buf + TAZ_FRAME_HEADER_SIZE, payload, pay_len);
    }

    write_fn(buf, total, ctx);
    free(buf);
}

/* Encodes *resp (built with include_result false, so it never carries
 * result) and sends it as exactly one RESPONSE frame with flags - never
 * chunks, unlike taz_response_send, and never clears/sets CONTINUATION on
 * its own: the caller decides. A malloc/encode failure drops the frame
 * silently, matching taz_process_send_frame. */
static void send_stream_frame(taz_dispatch_write_fn_t write_fn, void *ctx,
                              uint32_t stream_id, uint16_t opcode,
                              const taz_v1_TaskStatusResponse *resp,
                              uint8_t flags)
{
    size_t size = 0U;
    uint8_t *payload;

    if (!pb_get_encoded_size(&size, taz_v1_TaskStatusResponse_fields, resp))
    {
        return;
    }

    payload = (size > 0U) ? (uint8_t *)malloc(size) : NULL;
    if (size > 0U && payload == NULL)
    {
        return;
    }

    if (size > 0U)
    {
        pb_ostream_t ostream = pb_ostream_from_buffer(payload, size);
        if (!pb_encode(&ostream, taz_v1_TaskStatusResponse_fields, resp))
        {
            free(payload);
            return;
        }
        size = ostream.bytes_written;
    }

    task_status_flush_frame(write_fn, ctx, stream_id, opcode, payload, size,
                            flags);
    free(payload);
}

typedef enum
{
    TASK_STATUS_STREAM_RUNNING = 0,
    TASK_STATUS_STREAM_CLOSING = 1
} task_status_stream_state_t;

/* Heap-allocated per subscribed TASK_STATUS stream; freed by
 * on_timer_closed once the timer handle has closed. ~100 B - fine on the
 * heap, never a VLA. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
    uint32_t task_id;
    uint64_t interval_ms;

    uv_timer_t timer;
    task_status_stream_state_t state;

    taz_stream_ops_t ops;
} task_status_stream_t;

static void on_timer_closed(uv_handle_t *handle)
{
    task_status_stream_t *s = (task_status_stream_t *)handle->data;

    taz_dispatch_stream_done(s->d, s->stream_id);
    taz_dispatch_conn_unref(s->d);
    free(s);
}

/* Idempotent: a second call (e.g. abort racing a just-accepted cancel) is a
 * no-op once state is already CLOSING - the precedent for every close path
 * handling "already closing" is process_monitor.c's monitor_teardown. Drops
 * this stream's subscription, which frees a finished monitor=false task
 * that was only kept for this stream's final frame. */
static void teardown(task_status_stream_t *s)
{
    if (s->state == TASK_STATUS_STREAM_CLOSING)
    {
        return;
    }
    s->state = TASK_STATUS_STREAM_CLOSING;
    taz_task_unsubscribe(&s->d->tasks, s->task_id);
    uv_timer_stop(&s->timer);
    uv_close((uv_handle_t *)&s->timer, on_timer_closed);
}

/* Sends the stream's final frame (CONTINUATION cleared) with the task's
 * current state, unless the connection is already closing. */
static void send_final_frame(task_status_stream_t *s,
                             const detached_task_t *task)
{
    taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;

    if (taz_dispatch_conn_closing(s->d))
    {
        return;
    }
    if (task != NULL)
    {
        build_status_response(&resp, task, false, NULL);
    }
    send_stream_frame(s->write_fn, s->write_ctx, s->stream_id, s->opcode, &resp,
                      (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_NONE);
}

static void on_tick(uv_timer_t *handle)
{
    task_status_stream_t *s = (task_status_stream_t *)handle->data;
    const detached_task_t *task;
    taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;

    if (s->state != TASK_STATUS_STREAM_RUNNING)
    {
        return;
    }

    task = taz_task_find(&s->d->tasks, s->task_id);
    if (task == NULL || is_terminal_state(task->state))
    {
        /* A subscribed task is never evicted or freed (it counts this
         * stream as a subscriber), so task is NULL only if that invariant
         * broke; end the stream with a final frame either way. */
        send_final_frame(s, task);
        teardown(s);
        return;
    }

    build_status_response(&resp, task, false, NULL);

    if (!taz_dispatch_conn_closing(s->d))
    {
        send_stream_frame(s->write_fn, s->write_ctx, s->stream_id, s->opcode,
                          &resp,
                          (uint8_t)taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
    }
}

/* Refuses (0) once the stream has already ended, is ending, or is closing
 * - a second CANCEL for the same target falls out of this one check, like
 * process_monitor.c's monitor_cancel. Otherwise ends the subscription (not
 * the task itself - TASK_CANCEL, a different opcode, does that): the final
 * frame api.md requires of a cancelled stream, then the CANCEL's own reply
 * via done, synchronously - nothing here is deferred to a pool thread,
 * unlike a MONITOR sample. */
static int task_status_cancel(void *user, taz_stream_done_fn_t done,
                              void *done_arg)
{
    task_status_stream_t *s = (task_status_stream_t *)user;

    if (s->state != TASK_STATUS_STREAM_RUNNING)
    {
        return 0;
    }

    send_final_frame(s, taz_task_find(&s->d->tasks, s->task_id));
    teardown(s);
    done(done_arg);
    return 1;
}

/* Mirrors process_monitor.c's monitor_abort: teardown may already be under
 * way (e.g. a CANCEL and the client's disconnect arriving in one read), in
 * which case the timer handle is already closing and libuv asserts on
 * stopping a closing handle again. */
static void task_status_abort(void *user)
{
    task_status_stream_t *s = (task_status_stream_t *)user;

    if (s->state == TASK_STATUS_STREAM_CLOSING)
    {
        return;
    }
    teardown(s);
}

static void task_status_on_chunk(void *user, const taz_frame_header_t *header,
                                 const uint8_t *payload)
{
    (void)user;
    (void)header;
    (void)payload;
}

void taz_task_status_on_task_finished(taz_dispatch_t *d, uint32_t task_id)
{
    for (size_t i = 0; i < d->active_count; i++)
    {
        task_status_stream_t *s;

        if (d->stream_ops[i] == NULL ||
            d->stream_ops[i]->cancel != task_status_cancel)
        {
            continue;
        }
        s = (task_status_stream_t *)d->stream_user[i];
        if (s->task_id == task_id && s->state == TASK_STATUS_STREAM_RUNNING)
        {
            (void)uv_timer_start(&s->timer, on_tick, 0, s->interval_ms);
        }
    }
}

void handle_task_status(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_TaskStatusRequest req = taz_v1_TaskStatusRequest_init_zero;
    const detached_task_t *task;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_TaskStatusRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode TaskStatusRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    task = taz_task_find(&d->tasks, req.task_id);
    if (task == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND, "unknown task_id",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    if (!req.subscribe)
    {
        taz_v1_TaskStatusResponse resp = taz_v1_TaskStatusResponse_init_zero;
        bytes_ctx_t result_ctx;

        build_status_response(&resp, task, true, &result_ctx);
        taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                          taz_v1_TaskStatusResponse_fields, &resp);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    {
        task_status_stream_t *s =
            (task_status_stream_t *)calloc(1U, sizeof(*s));

        if (s == NULL)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                           "out of memory", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
        s->d = d;
        s->write_fn = write_fn;
        s->write_ctx = ctx;
        s->stream_id = header->stream_id;
        s->opcode = header->opcode;
        s->task_id = req.task_id;
        s->interval_ms = taz_task_status_interval_ms(req.interval_ms);
        s->state = TASK_STATUS_STREAM_RUNNING;

        if (uv_timer_init(d->loop, &s->timer) != 0)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                           "task status failed", "uv_timer_init failed");
            taz_dispatch_stream_done(d, header->stream_id);
            free(s);
            return;
        }
        s->timer.data = s;

        s->ops.on_chunk = task_status_on_chunk;
        s->ops.cancel = task_status_cancel;
        s->ops.abort = task_status_abort;
        s->ops.on_writable = NULL;
        taz_dispatch_set_stream_ops(d, header->stream_id, &s->ops, s);
        taz_dispatch_conn_ref(d);
        taz_task_subscribe(&d->tasks, req.task_id);

        if (uv_timer_start(&s->timer, on_tick, 0, s->interval_ms) != 0)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INTERNAL,
                           "task status failed", "uv_timer_start failed");
            teardown(s);
            return;
        }
    }
}
