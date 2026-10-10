#include "timeout.h"

#include <pb_decode.h>

#include "taz/error.h"
#include "taz/response.h"
#include "taz/v1/advanced.pb.h"

void handle_timeout_set(taz_dispatch_t *d, const taz_frame_header_t *header,
                        const uint8_t *payload,
                        taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_TimeoutSetRequest req = taz_v1_TimeoutSetRequest_init_zero;
    taz_v1_TimeoutSetResponse resp = taz_v1_TimeoutSetResponse_init_zero;

    if (payload != NULL && header->length > 0U)
    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_TimeoutSetRequest_fields, &req))
        {
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode TimeoutSetRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    resp.previous_ms = d->conn_timeout_ms;
    d->conn_timeout_ms = req.timeout_ms;

    taz_response_send(write_fn, ctx, header->stream_id, header->opcode,
                      taz_v1_TimeoutSetResponse_fields, &resp);
    taz_dispatch_stream_done(d, header->stream_id);
}
