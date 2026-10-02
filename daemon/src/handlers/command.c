#include "command.h"

#include <stdlib.h>
#include <string.h>

#include <pb_encode.h>

#include "taz/error.h"
#include "taz/response.h"
#include "taz/v1/command.pb.h"

/* Context for encode_bytes: a pointer/length pair into the taz_exec_result_t
 * buffers, which stay valid for the duration of the taz_response_send call
 * below. */
typedef struct
{
    const uint8_t *data;
    size_t len;
} bytes_ctx_t;

static bool encode_bytes(pb_ostream_t *stream, const pb_field_iter_t *field,
                         void *const *arg)
{
    const bytes_ctx_t *bctx = (const bytes_ctx_t *)*arg;
    if (!pb_encode_tag_for_field(stream, field))
    {
        return false;
    }
    return pb_encode_string(stream, bctx->data, bctx->len);
}

void taz_command_send_exec_response(taz_dispatch_write_fn_t write_fn, void *ctx,
                                    uint32_t stream_id, uint16_t opcode,
                                    const taz_exec_result_t *result)
{
    taz_v1_CommandExecResponse *resp;
    bytes_ctx_t out_ctx;
    bytes_ctx_t err_ctx;

    resp = (taz_v1_CommandExecResponse *)malloc(sizeof(*resp));
    if (resp == NULL)
    {
        taz_error_send(write_fn, ctx, stream_id, opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        return;
    }
    (void)memset(resp, 0, sizeof(*resp));

    resp->exit_code = (result->term_signal != 0) ? -(int32_t)result->term_signal
                                                 : (int32_t)result->exit_status;
    resp->timed_out = result->timed_out;
    resp->truncated = result->truncated;

    if (result->out_len > 0U)
    {
        out_ctx.data = result->out;
        out_ctx.len = result->out_len;
        resp->stdout_data.funcs.encode = encode_bytes;
        resp->stdout_data.arg = &out_ctx;
    }
    if (result->err_len > 0U)
    {
        err_ctx.data = result->err;
        err_ctx.len = result->err_len;
        resp->stderr_data.funcs.encode = encode_bytes;
        resp->stderr_data.arg = &err_ctx;
    }

    taz_response_send(write_fn, ctx, stream_id, opcode,
                      taz_v1_CommandExecResponse_fields, resp);
    free(resp);
}
