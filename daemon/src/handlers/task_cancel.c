#include "task_cancel.h"

#include <stdbool.h>
#include <stddef.h>

#include <pb_decode.h>
#include <uv.h>

#include "taz/error.h"
#include "taz/exec.h"
#include "taz/response.h"
#include "taz/task.h"
#include "taz/v1/advanced.pb.h"

static void send_cancel_response(taz_dispatch_write_fn_t write_fn, void *ctx,
                                 uint32_t stream_id, uint16_t opcode,
                                 bool accepted)
{
    taz_v1_TaskCancelResponse resp = taz_v1_TaskCancelResponse_init_zero;
    resp.accepted = accepted;
    taz_response_send(write_fn, ctx, stream_id, opcode,
                      taz_v1_TaskCancelResponse_fields, &resp);
}

void handle_task_cancel(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_TaskCancelRequest req = taz_v1_TaskCancelRequest_init_zero;
    detached_task_t *task;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_TaskCancelRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode TaskCancelRequest failed", NULL);
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

    /* Already finished: on_done has already dropped the exec pointer, so
     * there is nothing left to cancel. */
    if (task->exec == NULL)
    {
        send_cancel_response(write_fn, ctx, header->stream_id, header->opcode,
                             false);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    if (req.signal == 0)
    {
        taz_task_set_cancel_requested(&d->tasks, req.task_id);
        taz_exec_cancel(task->exec);
    }
    else
    {
        const int rc = taz_exec_signal(task->exec, (int)req.signal);
        if (rc == UV_EINVAL)
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "invalid signal", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
        taz_task_set_cancel_requested(&d->tasks, req.task_id);
    }

    send_cancel_response(write_fn, ctx, header->stream_id, header->opcode,
                         true);
    taz_dispatch_stream_done(d, header->stream_id);
}
