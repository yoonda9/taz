#include "command.h"

#include <stdlib.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <uv.h>

#include "taz/config.h"
#include "taz/error.h"
#include "taz/log.h"
#include "taz/response.h"
#include "taz/v1/command.pb.h"

/* Carries what on_exec_done needs to send the response and close the
 * stream: the dispatch context for taz_dispatch_stream_done, and the
 * write sink/stream/opcode taz_command_send_exec_response forwards along.
 * Heap-allocated per in-flight exec, freed once on_exec_done runs. */
typedef struct
{
    taz_dispatch_t *d;
    taz_dispatch_write_fn_t write_fn;
    void *write_ctx;
    uint32_t stream_id;
    uint16_t opcode;
} exec_ctx_t;

static void on_exec_done(const taz_exec_result_t *result, void *arg)
{
    exec_ctx_t *ectx = (exec_ctx_t *)arg;
    const char *timed_out = "";
    const char *cancelled = "";

    if (result->timed_out)
    {
        timed_out = " timed_out";
    }
    if (result->cancelled)
    {
        cancelled = " cancelled";
    }
    taz_log(TAZ_LOG_INFO,
            "executed command: exit=%lld signal=%d%s%s output=%zu",
            (long long)result->exit_status, result->term_signal, timed_out,
            cancelled, result->out_len);

    taz_command_send_exec_response(ectx->write_fn, ectx->write_ctx,
                                   ectx->stream_id, ectx->opcode, result);
    taz_dispatch_stream_done(ectx->d, ectx->stream_id);
    /* Matches the taz_dispatch_conn_ref taken in handle_command_exec right
     * after a successful taz_exec_start; this is the last point this
     * handler ever touches the connection. */
    taz_dispatch_conn_unref(ectx->d);
    free(ectx);
}

/* Map a taz_exec_start spawn-failure return (a UV_E* code, never a raw
 * errno - these differ on Windows) to an ErrorCode per the plan's §6.2
 * subset for this step. */
static taz_v1_ErrorCode map_spawn_error(int rc)
{
    if (rc == UV_ENOENT)
    {
        return taz_v1_ErrorCode_ERROR_CODE_NOT_FOUND;
    }
    if (rc == UV_EACCES)
    {
        return taz_v1_ErrorCode_ERROR_CODE_PERMISSION_DENIED;
    }
    return taz_v1_ErrorCode_ERROR_CODE_INTERNAL;
}

void handle_command_exec(taz_dispatch_t *d, const taz_frame_header_t *header,
                         const uint8_t *payload,
                         taz_dispatch_write_fn_t write_fn, void *ctx)
{
    taz_v1_CommandExecRequest *req;
    exec_ctx_t *ectx;
    const char *argv_ptrs[32];
    taz_exec_spec_t spec;
    taz_exec_t *exec_handle = NULL;
    pb_size_t i;
    int rc;

    if (payload == NULL || header->length == 0U)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                       "empty COMMAND_EXEC payload", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    /* ~40 KiB; never on the stack (design: 1 MiB Windows thread stacks). */
    req = (taz_v1_CommandExecRequest *)malloc(sizeof(*req));
    if (req == NULL)
    {
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    {
        pb_istream_t istream =
            pb_istream_from_buffer(payload, (size_t)header->length);
        if (!pb_decode(&istream, taz_v1_CommandExecRequest_fields, req))
        {
            free(req);
            taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                           taz_v1_ErrorCode_ERROR_CODE_INVALID_REQUEST,
                           "decode CommandExecRequest failed", NULL);
            taz_dispatch_stream_done(d, header->stream_id);
            return;
        }
    }

    /* RUN_AS support via a non-empty as_user comes later; do not silently
     * run as the daemon's own identity. */
    if (req->as_user[0] != '\0')
    {
        free(req);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_NOT_SUPPORTED,
                       "as_user is not supported yet", NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    ectx = (exec_ctx_t *)malloc(sizeof(*ectx));
    if (ectx == NULL)
    {
        free(req);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode,
                       taz_v1_ErrorCode_ERROR_CODE_INTERNAL, "out of memory",
                       NULL);
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }
    ectx->d = d;
    ectx->write_fn = write_fn;
    ectx->write_ctx = ctx;
    ectx->stream_id = header->stream_id;
    ectx->opcode = header->opcode;

    for (i = 0; i < req->args_count; i++)
    {
        argv_ptrs[i] = req->args[i];
    }

    (void)memset(&spec, 0, sizeof(spec));
    spec.file = req->command;
    spec.args = argv_ptrs;
    spec.args_count = (size_t)req->args_count;
    spec.env = req->env;
    spec.env_count = (size_t)req->env_count;
    spec.cwd = req->working_dir;
    /* Snapshot the effective timeout now, before taz_exec_start: a
     * nonzero per-call timeout_ms wins, otherwise fall back to the
     * connection default set by TIMEOUT_SET. Taking it here means a
     * TIMEOUT_SET that lands after this point never retargets an
     * already-dispatched exec. */
    spec.timeout_ms =
        (req->timeout_ms != 0U) ? req->timeout_ms : d->conn_timeout_ms;
    spec.max_output_bytes = taz_config_exec_max_output_bytes();

    rc = taz_exec_start(d->loop, &spec, on_exec_done, ectx, &exec_handle);
    free(req);

    if (rc != 0)
    {
        const taz_v1_ErrorCode code = map_spawn_error(rc);
        free(ectx);
        taz_error_send(write_fn, ctx, header->stream_id, header->opcode, code,
                       "spawn failed", uv_strerror(rc));
        taz_dispatch_stream_done(d, header->stream_id);
        return;
    }

    /* The exec now owns a reference on the connection until on_exec_done
     * unrefs it, so a client disconnect can never free the connection out
     * from under this in-flight process. */
    taz_dispatch_conn_ref(d);
    taz_dispatch_set_stream_exec(d, header->stream_id, exec_handle);
}

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
