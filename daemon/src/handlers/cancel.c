#include "cancel.h"

#include <stdbool.h>
#include <stdlib.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "taz/error.h"
#include "taz/response.h"
#include "taz/v1/advanced.pb.h"

/* Carries what on_cancelled needs to answer the CANCEL REQUEST: the
 * dispatch context for taz_dispatch_stream_done, and the write
 * sink/stream/opcode taz_response_send forwards along. Heap-allocated per
 * in-flight CANCEL, freed once on_cancelled runs. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
} cancel_ctx_t;

static void send_cancel_response(taz_dispatch_write_fn_t write_fn, void *ctx,
                                 uint32_t stream_id, uint16_t opcode,
                                 bool cancelled)
{
    taz_v1_CancelResponse resp = taz_v1_CancelResponse_init_zero;
    resp.cancelled = cancelled;
    taz_response_send(write_fn, ctx, stream_id, opcode,
                      taz_v1_CancelResponse_fields, &resp);
}

/* Invoked by the target's stream_ops_t::cancel once its cancellation has
 * actually taken effect (e.g. for FILE_PUT, once the temp file is gone). */
static void on_cancelled(void *arg)
{
    cancel_ctx_t *cctx = (cancel_ctx_t *)arg;

    if (!taz_dispatch_conn_closing(cctx->d))
    {
        send_cancel_response(cctx->write_fn, cctx->write_ctx, cctx->stream_id,
                             cctx->opcode, true);
    }
    taz_dispatch_stream_done(cctx->d, cctx->stream_id);
    free(cctx);
}

void handle_cancel(taz_dispatch_t *d, const taz_frame_header_t *header,
                   const uint8_t *payload, taz_dispatch_write_fn_t write_fn,
                   void *ctx)
{
    taz_v1_CancelRequest req = taz_v1_CancelRequest_init_zero;
    cancel_ctx_t *cctx;

    /* An empty payload decodes to target_stream_id == 0, which is refused
     * below like any other explicit zero target. */
    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_CancelRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode CancelRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    if (req.target_stream_id == 0U || req.target_stream_id == header->stream_id)
    {
        send_cancel_response(write_fn, ctx, header->stream_id, header->opcode,
                             false);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    cctx = (cancel_ctx_t *)malloc(sizeof(*cctx));
    if (cctx == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    cctx->d = d;
    cctx->write_fn = write_fn;
    cctx->write_ctx = ctx;
    cctx->stream_id = header->stream_id;
    cctx->opcode = header->opcode;

    if (taz_dispatch_cancel_stream(d, req.target_stream_id, on_cancelled,
                                   cctx) == 0)
    {
        free(cctx);
        send_cancel_response(write_fn, ctx, header->stream_id, header->opcode,
                             false);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    /* Accepted: on_cancelled fires later (or, for an ops implementation that
     * completes synchronously, has already fired by the time
     * taz_dispatch_cancel_stream returns) and closes this stream itself. */
}
